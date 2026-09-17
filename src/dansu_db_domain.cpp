#include "dansu_db.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>

namespace godot {

namespace {

constexpr int CURRENT_SCHEMA_VERSION = 3;

String dictionary_string(const Dictionary &p_value, const String &p_key, const String &p_default = String()) {
    return static_cast<String>(p_value.get(p_key, p_default));
}

int64_t dictionary_int(const Dictionary &p_value, const String &p_key, int64_t p_default = 0) {
    return static_cast<int64_t>(p_value.get(p_key, p_default));
}

double dictionary_float(const Dictionary &p_value, const String &p_key, double p_default = 0.0) {
    return static_cast<double>(p_value.get(p_key, p_default));
}

bool dictionary_bool(const Dictionary &p_value, const String &p_key, bool p_default = false) {
    return static_cast<bool>(p_value.get(p_key, p_default));
}

int clamped_limit(int p_limit) {
    return std::clamp(p_limit, 1, 1000);
}

int clamped_offset(int p_offset) {
    return std::max(p_offset, 0);
}

String escaped_like_pattern(const String &p_search) {
    String escaped = p_search.strip_edges().to_lower();
    escaped = escaped.replace("\\", "\\\\");
    escaped = escaped.replace("%", "\\%");
    escaped = escaped.replace("_", "\\_");
    return String("%") + escaped + String("%");
}

String chart_select_sql() {
    return R"SQL(
        SELECT
            c.id,
            c.chart_set_id,
            cs.folder_name,
            c.file_name,
            c.uuid,
            c.version,
            c.file_modified_time,
            c.file_size,
            c.file_hash,
            c.is_built_in,
            c.title,
            c.artist,
            c.creator,
            c.source,
            c.tags,
            c.difficulty,
            c.rating,
            c.preview_time,
            c.play_time_ms,
            c.file_audio,
            c.file_cover_art,
            c.file_skin,
            c.search_text,
            c.is_present,
            COALESCE(history_stats.last_played_at, 0) AS last_played_at,
            COALESCE(version_stats.best_score, 0.0) AS best_score,
            COALESCE(history_stats.play_count, 0) AS play_count,
            COALESCE(version_stats.play_count, 0) AS current_version_play_count
        FROM charts c
        JOIN chart_sets cs ON cs.id = c.chart_set_id
        LEFT JOIN (
            SELECT
                chart_id,
                MAX(played_at) AS last_played_at,
                MAX(score_value) AS best_score,
                COUNT(*) AS play_count
            FROM play_records
            GROUP BY chart_id
        ) history_stats ON history_stats.chart_id = c.id
        LEFT JOIN (
            SELECT
                chart_id,
                chart_hash,
                MAX(score_value) AS best_score,
                COUNT(*) AS play_count
            FROM play_records
            GROUP BY chart_id, chart_hash
        ) version_stats
            ON version_stats.chart_id = c.id
            AND version_stats.chart_hash = c.file_hash
    )SQL";
}

String play_select_sql() {
    return R"SQL(
        SELECT
            p.id,
            p.chart_id,
            p.chart_hash,
            p.submission_id,
            p.played_at,
            p.score_value,
            p.raw_score,
            p.max_score,
            p.notes,
            p.perfect_plus,
            p.perfect,
            p.great,
            p.ok,
            p.bad,
            p.miss,
            p.high_combo,
            p.avg_signed_timing,
            p.unstable_rate,
            p.scoring_version,
            c.uuid,
            c.title,
            c.artist,
            c.difficulty,
            cs.folder_name,
            c.file_name
        FROM play_records p
        JOIN charts c ON c.id = p.chart_id
        JOIN chart_sets cs ON cs.id = c.chart_set_id
    )SQL";
}

} // namespace

bool DansuDB::_initialize_schema_unlocked() {
    const Array version_rows = _query_unlocked("PRAGMA user_version;");
    if (version_rows.is_empty()) {
        return false;
    }

    const Dictionary version_row = version_rows[0];
    const int version = static_cast<int>(version_row.get("user_version", 0));
    // Roll back the canceled creator cache without touching charts or play history.
    if (version == 4) {
        if (!_execute_script_unlocked(R"SQL(
            BEGIN IMMEDIATE;
            DROP TABLE IF EXISTS chart_creators;
            PRAGMA user_version = 3;
            COMMIT;
        )SQL")) {
            _execute_script_unlocked("ROLLBACK;");
            return false;
        }
        return true;
    }
    if (version > CURRENT_SCHEMA_VERSION) {
        _set_error_unlocked(SQLITE_ERROR, "Database schema is newer than this DansuDB build");
        return false;
    }
    if (version == CURRENT_SCHEMA_VERSION) {
        return true;
    }

    if (version == 0) {
        if (!_execute_script_unlocked(R"SQL(
            BEGIN IMMEDIATE;

            CREATE TABLE chart_sets (
                id INTEGER PRIMARY KEY,
                uuid TEXT NOT NULL UNIQUE,
                folder_name TEXT NOT NULL,
                is_present INTEGER NOT NULL DEFAULT 1 CHECK (is_present IN (0, 1)),
                scan_generation INTEGER NOT NULL DEFAULT 0
            );

            CREATE UNIQUE INDEX chart_sets_present_folder_unique
                ON chart_sets(folder_name)
                WHERE is_present = 1;

            CREATE TABLE charts (
                id INTEGER PRIMARY KEY,
                chart_set_id INTEGER NOT NULL REFERENCES chart_sets(id) ON DELETE RESTRICT,
                file_name TEXT NOT NULL,
                uuid TEXT NOT NULL DEFAULT '',
                version INTEGER NOT NULL DEFAULT 1,
                file_modified_time INTEGER NOT NULL DEFAULT 0,
                file_size INTEGER NOT NULL DEFAULT 0,
                file_hash TEXT NOT NULL DEFAULT '',
                is_built_in INTEGER NOT NULL DEFAULT 0 CHECK (is_built_in IN (0, 1)),
                title TEXT NOT NULL DEFAULT '?',
                artist TEXT NOT NULL DEFAULT '?',
                creator TEXT NOT NULL DEFAULT '?',
                source TEXT NOT NULL DEFAULT '?',
                tags TEXT NOT NULL DEFAULT '',
                difficulty TEXT NOT NULL DEFAULT '?',
                rating REAL NOT NULL DEFAULT -1.0,
                preview_time REAL NOT NULL DEFAULT -1.0,
                play_time_ms INTEGER NOT NULL DEFAULT 0,
                file_audio TEXT NOT NULL DEFAULT '',
                file_cover_art TEXT NOT NULL DEFAULT '',
                file_skin TEXT NOT NULL DEFAULT '',
                search_text TEXT NOT NULL DEFAULT '',
                is_present INTEGER NOT NULL DEFAULT 1 CHECK (is_present IN (0, 1)),
                scan_generation INTEGER NOT NULL DEFAULT 0
            );

            CREATE UNIQUE INDEX charts_uuid_unique
                ON charts(uuid);
            CREATE UNIQUE INDEX charts_present_path_unique
                ON charts(chart_set_id, file_name)
                WHERE is_present = 1;
            CREATE INDEX charts_set_index ON charts(chart_set_id);
            CREATE INDEX charts_title_index ON charts(title COLLATE NOCASE);
            CREATE INDEX charts_artist_index ON charts(artist COLLATE NOCASE);
            CREATE INDEX charts_rating_index ON charts(rating DESC);
            CREATE INDEX charts_presence_index ON charts(is_present);

            CREATE TABLE chart_timings (
                chart_id INTEGER NOT NULL REFERENCES charts(id) ON DELETE CASCADE,
                position INTEGER NOT NULL,
                time INTEGER NOT NULL,
                bpm REAL NOT NULL,
                PRIMARY KEY (chart_id, position)
            );

            CREATE TABLE play_records (
                id INTEGER PRIMARY KEY,
                chart_id INTEGER NOT NULL REFERENCES charts(id) ON DELETE RESTRICT,
                chart_hash TEXT NOT NULL DEFAULT '',
                submission_id TEXT,
                played_at INTEGER NOT NULL,
                score_value REAL NOT NULL,
                raw_score REAL NOT NULL DEFAULT 0.0,
                max_score REAL NOT NULL DEFAULT 0.0,
                notes INTEGER NOT NULL DEFAULT 0,
                perfect_plus INTEGER NOT NULL DEFAULT 0,
                perfect INTEGER NOT NULL DEFAULT 0,
                great INTEGER NOT NULL DEFAULT 0,
                ok INTEGER NOT NULL DEFAULT 0,
                bad INTEGER NOT NULL DEFAULT 0,
                miss INTEGER NOT NULL DEFAULT 0,
                high_combo INTEGER NOT NULL DEFAULT 0,
                avg_signed_timing REAL NOT NULL DEFAULT 0.0,
                unstable_rate REAL NOT NULL DEFAULT 0.0,
                scoring_version INTEGER NOT NULL DEFAULT 1
            );

            CREATE INDEX plays_chart_time_index
                ON play_records(chart_id, played_at DESC);
            CREATE INDEX plays_time_index
                ON play_records(played_at DESC);
            CREATE INDEX plays_chart_score_index
                ON play_records(chart_id, score_value DESC, played_at DESC);
            CREATE INDEX plays_chart_hash_score_index
                ON play_records(chart_id, chart_hash, score_value DESC, played_at DESC);
            CREATE UNIQUE INDEX plays_submission_id_unique
                ON play_records(submission_id)
                WHERE submission_id IS NOT NULL AND submission_id <> '';

            CREATE TABLE dansu_state (
                key TEXT PRIMARY KEY,
                integer_value INTEGER NOT NULL
            );
            INSERT INTO dansu_state(key, integer_value)
                VALUES ('chart_scan_generation', 0);
            INSERT INTO dansu_state(key, integer_value)
                VALUES ('rating_calculation_version', 0);

            PRAGMA user_version = 3;
            COMMIT;
        )SQL")) {
            _execute_script_unlocked("ROLLBACK;");
            return false;
        }
        return true;
    }

    if (version == 2) {
        if (!_execute_script_unlocked(R"SQL(
            BEGIN IMMEDIATE;
            ALTER TABLE play_records ADD COLUMN submission_id TEXT;
            CREATE UNIQUE INDEX plays_submission_id_unique
                ON play_records(submission_id)
                WHERE submission_id IS NOT NULL AND submission_id <> '';
            PRAGMA user_version = 3;
            COMMIT;
        )SQL")) {
            _execute_script_unlocked("ROLLBACK;");
            return false;
        }
        return true;
    }

    _set_error_unlocked(SQLITE_ERROR, "Database schema version is incompatible with this DansuDB build");
    return false;
}

int64_t DansuDB::begin_chart_scan() {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    const Array rows = _query_unlocked(R"SQL(
        UPDATE dansu_state
        SET integer_value = integer_value + 1
        WHERE key = 'chart_scan_generation'
        RETURNING integer_value;
    )SQL");
    if (rows.is_empty()) {
        _set_error_unlocked(SQLITE_CORRUPT, "Chart scan generation state is missing");
        return -1;
    }
    return dictionary_int(rows[0], "integer_value", -1);
}

bool DansuDB::prepare_rating_cache(int p_calculation_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    if (p_calculation_version <= 0) {
        _set_error_unlocked(SQLITE_MISUSE, "calculation_version must be positive");
        return false;
    }

    const Array rows = _query_unlocked(
            "SELECT integer_value FROM dansu_state WHERE key = 'rating_calculation_version';");
    if (last_error_code_ != SQLITE_OK) {
        return false;
    }
    if (!rows.is_empty() && dictionary_int(rows[0], "integer_value", 0) == p_calculation_version) {
        return true;
    }

    if (!_execute_script_unlocked("SAVEPOINT dansusql_rating_cache;")) {
        return false;
    }
    if (!_execute_unlocked(R"SQL(
        INSERT INTO dansu_state(key, integer_value)
        VALUES ('rating_calculation_version', ?)
        ON CONFLICT(key) DO UPDATE SET integer_value = excluded.integer_value;
    )SQL", Array::make(p_calculation_version)) ||
            !_execute_unlocked("UPDATE charts SET rating = -1.0;")) {
        const int saved_code = last_error_code_;
        const String saved_message = last_error_message_;
        _execute_script_unlocked("ROLLBACK TO dansusql_rating_cache; RELEASE dansusql_rating_cache;");
        last_error_code_ = saved_code;
        last_error_message_ = saved_message;
        return false;
    }
    return _execute_script_unlocked("RELEASE dansusql_rating_cache;");
}

int64_t DansuDB::upsert_chart_set(Object *p_chart_set, int64_t p_scan_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    if (p_chart_set == nullptr) {
        _set_error_unlocked(SQLITE_MISUSE, "chart_set is required");
        return -1;
    }
    const String uuid = static_cast<String>(p_chart_set->get("uuid")).strip_edges();
    const String folder_name = static_cast<String>(p_chart_set->get("folder_name")).strip_edges();
    if (uuid.is_empty() || folder_name.is_empty() || p_scan_generation < 0) {
        _set_error_unlocked(SQLITE_MISUSE, "chart set uuid and folder_name are required, and scan_generation cannot be negative");
        return -1;
    }

    if (!_execute_script_unlocked("SAVEPOINT dansusql_upsert_chart_set;")) {
        return -1;
    }
    if (!_execute_unlocked(R"SQL(
        UPDATE charts
        SET is_present = 0
        WHERE chart_set_id IN (
            SELECT id FROM chart_sets
            WHERE folder_name = ? AND uuid <> ? AND is_present = 1
        );
    )SQL", Array::make(folder_name, uuid)) ||
            !_execute_unlocked(
                    "UPDATE chart_sets SET is_present = 0 WHERE folder_name = ? AND uuid <> ? AND is_present = 1;",
                    Array::make(folder_name, uuid))) {
        _execute_script_unlocked("ROLLBACK TO dansusql_upsert_chart_set; RELEASE dansusql_upsert_chart_set;");
        return -1;
    }

    const Array rows = _query_unlocked(R"SQL(
        INSERT INTO chart_sets(uuid, folder_name, is_present, scan_generation)
        VALUES (?, ?, 1, ?)
        ON CONFLICT(uuid) DO UPDATE SET
            folder_name = excluded.folder_name,
            is_present = 1,
            scan_generation = CASE
                WHEN excluded.scan_generation > 0 THEN excluded.scan_generation
                ELSE chart_sets.scan_generation
            END
        RETURNING id;
    )SQL", Array::make(uuid, folder_name, p_scan_generation));

    if (rows.is_empty() || !_execute_script_unlocked("RELEASE dansusql_upsert_chart_set;")) {
        _execute_script_unlocked("ROLLBACK TO dansusql_upsert_chart_set; RELEASE dansusql_upsert_chart_set;");
        return -1;
    }
    return dictionary_int(rows[0], "id", -1);
}

bool DansuDB::touch_chart(
        int64_t p_chart_id,
        int64_t p_modified_time,
        int64_t p_file_size,
        int64_t p_scan_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    if (p_chart_id <= 0 || p_scan_generation <= 0) {
        _set_error_unlocked(SQLITE_MISUSE, "chart_id and scan_generation must be positive");
        return false;
    }

    if (!_execute_unlocked(R"SQL(
        UPDATE charts
        SET
            file_modified_time = ?,
            file_size = ?,
            is_present = 1,
            scan_generation = ?
        WHERE id = ?;
    )SQL", Array::make(p_modified_time, p_file_size, p_scan_generation, p_chart_id))) {
        return false;
    }

    if (sqlite3_changes64(database_) == 0) {
        _set_error_unlocked(SQLITE_NOTFOUND, "Chart was not found");
        return false;
    }
    return true;
}

int64_t DansuDB::upsert_chart(Object *p_chart, int64_t p_scan_generation) {
    if (p_chart == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        _set_error_unlocked(SQLITE_MISUSE, "chart is required");
        return -1;
    }

    const Variant chart_set_value = p_chart->get("chart_set");
    Object *chart_set = chart_set_value.get_type() == Variant::OBJECT ? static_cast<Object *>(chart_set_value) : nullptr;
    Dictionary chart;
    chart["chart_set_id"] = chart_set != nullptr ? chart_set->get("db_id") : Variant(-1);
    chart["file_name"] = p_chart->get("file_name");
    chart["uuid"] = p_chart->get("uuid");
    chart["version"] = p_chart->get("version");
    chart["file_modified_time"] = p_chart->get("file_modified_time");
    chart["file_size"] = p_chart->get("file_size");
    chart["file_hash"] = p_chart->get("filehash");
    chart["is_built_in"] = p_chart->get("is_built_in");
    chart["title"] = p_chart->get("title");
    chart["artist"] = p_chart->get("artist");
    chart["creator"] = p_chart->get("creator");
    chart["source"] = p_chart->get("source");
    chart["tags"] = p_chart->get("tags");
    chart["difficulty"] = p_chart->get("difficulty");
    chart["rating"] = static_cast<bool>(p_chart->get("rating_calculated"))
            ? p_chart->get("rating")
            : Variant(-1.0);
    chart["preview_time"] = p_chart->get("preview_time");
    chart["play_time_ms"] = p_chart->get("play_time_ms");
    chart["file_audio"] = p_chart->get("file_audio");
    chart["file_cover_art"] = p_chart->get("file_cover_art");
    chart["file_skin"] = p_chart->get("file_skin");
    chart["search_text"] = p_chart->get("search_string_lower");

    Array timing_records;
    const Variant timings_value = p_chart->get("timings");
    if (timings_value.get_type() == Variant::ARRAY) {
        const Array timings = timings_value;
        for (int index = 0; index < timings.size(); ++index) {
            if (timings[index].get_type() != Variant::OBJECT) {
                continue;
            }
            Object *timing = timings[index];
            if (timing == nullptr) {
                continue;
            }
            Dictionary timing_record;
            timing_record["time"] = timing->get("time");
            timing_record["bpm"] = timing->get("bpm");
            timing_records.append(timing_record);
        }
    }
    chart["timings"] = timing_records;
    return _upsert_chart_record(chart, p_scan_generation);
}

int64_t DansuDB::_upsert_chart_record(const Dictionary &p_chart, int64_t p_scan_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    const int64_t chart_set_id = dictionary_int(p_chart, "chart_set_id", -1);
    const String file_name = dictionary_string(p_chart, "file_name").strip_edges();
    const String uuid = dictionary_string(p_chart, "uuid").strip_edges();
    if (chart_set_id <= 0 || file_name.is_empty() || uuid.is_empty() || p_scan_generation < 0) {
        _set_error_unlocked(SQLITE_MISUSE, "chart_set_id, uuid, and file_name are required, and scan_generation cannot be negative");
        return -1;
    }

    const String title = dictionary_string(p_chart, "title", "?");
    const String artist = dictionary_string(p_chart, "artist", "?");
    const String creator = dictionary_string(p_chart, "creator", "?");
    const String source = dictionary_string(p_chart, "source", "?");
    const String tags = dictionary_string(p_chart, "tags");
    const String difficulty = dictionary_string(p_chart, "difficulty", "?");
    const String search_text = dictionary_string(
            p_chart,
            "search_text",
            (title + String("-") + artist + String("-") + creator + String("-") + tags + String("-") + difficulty + String("-") + source).to_lower());

    Array parameters;
    parameters.append(chart_set_id);
    parameters.append(file_name);
    parameters.append(uuid);
    parameters.append(dictionary_int(p_chart, "version", 1));
    parameters.append(dictionary_int(p_chart, "file_modified_time"));
    parameters.append(dictionary_int(p_chart, "file_size"));
    parameters.append(dictionary_string(p_chart, "file_hash"));
    parameters.append(dictionary_bool(p_chart, "is_built_in"));
    parameters.append(title);
    parameters.append(artist);
    parameters.append(creator);
    parameters.append(source);
    parameters.append(tags);
    parameters.append(difficulty);
    parameters.append(p_chart.get("rating", Variant()));
    parameters.append(dictionary_float(p_chart, "preview_time", -1.0));
    parameters.append(std::max<int64_t>(dictionary_int(p_chart, "play_time_ms"), 0));
    parameters.append(dictionary_string(p_chart, "file_audio"));
    parameters.append(dictionary_string(p_chart, "file_cover_art"));
    parameters.append(dictionary_string(p_chart, "file_skin"));
    parameters.append(search_text);
    parameters.append(p_scan_generation);

    if (!_execute_script_unlocked("SAVEPOINT dansusql_upsert_chart;")) {
        return -1;
    }

    if (!_execute_unlocked(R"SQL(
        UPDATE charts
        SET is_present = 0
        WHERE chart_set_id = ? AND file_name = ? AND uuid <> ? AND is_present = 1;
    )SQL", Array::make(chart_set_id, file_name, uuid))) {
        const int saved_code = last_error_code_;
        const String saved_message = last_error_message_;
        _execute_script_unlocked("ROLLBACK TO dansusql_upsert_chart; RELEASE dansusql_upsert_chart;");
        last_error_code_ = saved_code;
        last_error_message_ = saved_message;
        return -1;
    }

    const Array rows = _query_unlocked(R"SQL(
        INSERT INTO charts(
            chart_set_id, file_name, uuid, version,
            file_modified_time, file_size, file_hash, is_built_in,
            title, artist, creator, source, tags, difficulty,
            rating, preview_time, play_time_ms, file_audio, file_cover_art, file_skin,
            search_text, is_present, scan_generation
        ) VALUES (
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, 1, ?
        )
        ON CONFLICT(uuid) DO UPDATE SET
            chart_set_id = excluded.chart_set_id,
            file_name = excluded.file_name,
            version = excluded.version,
            file_modified_time = excluded.file_modified_time,
            file_size = excluded.file_size,
            file_hash = excluded.file_hash,
            is_built_in = excluded.is_built_in,
            title = excluded.title,
            artist = excluded.artist,
            creator = excluded.creator,
            source = excluded.source,
            tags = excluded.tags,
            difficulty = excluded.difficulty,
            rating = excluded.rating,
            preview_time = excluded.preview_time,
            play_time_ms = excluded.play_time_ms,
            file_audio = excluded.file_audio,
            file_cover_art = excluded.file_cover_art,
            file_skin = excluded.file_skin,
            search_text = excluded.search_text,
            is_present = 1,
            scan_generation = CASE
                WHEN excluded.scan_generation > 0 THEN excluded.scan_generation
                ELSE charts.scan_generation
            END
        RETURNING id;
    )SQL", parameters);

    if (rows.is_empty()) {
        const int saved_code = last_error_code_;
        const String saved_message = last_error_message_;
        _execute_script_unlocked("ROLLBACK TO dansusql_upsert_chart; RELEASE dansusql_upsert_chart;");
        last_error_code_ = saved_code;
        last_error_message_ = saved_message;
        return -1;
    }

    const int64_t chart_id = dictionary_int(rows[0], "id", -1);
    const Array timings = static_cast<Array>(p_chart.get("timings", Array()));
    if (!_write_timings_unlocked(chart_id, timings)) {
        const int saved_code = last_error_code_;
        const String saved_message = last_error_message_;
        _execute_script_unlocked("ROLLBACK TO dansusql_upsert_chart; RELEASE dansusql_upsert_chart;");
        last_error_code_ = saved_code;
        last_error_message_ = saved_message;
        return -1;
    }

    if (!_execute_script_unlocked("RELEASE dansusql_upsert_chart;")) {
        return -1;
    }
    return chart_id;
}

bool DansuDB::finish_chart_scan(int64_t p_scan_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    const Array generation_rows = _query_unlocked(R"SQL(
        SELECT integer_value
        FROM dansu_state
        WHERE key = 'chart_scan_generation';
    )SQL");
    if (generation_rows.is_empty() || dictionary_int(generation_rows[0], "integer_value", -1) != p_scan_generation) {
        _set_error_unlocked(SQLITE_ABORT, "A newer chart scan has already started");
        return false;
    }

    if (!_execute_script_unlocked("SAVEPOINT dansusql_finish_scan;")) {
        return false;
    }
    if (!_execute_unlocked(
                "UPDATE charts SET is_present = 0 WHERE scan_generation <> ?;",
                Array::make(p_scan_generation)) ||
            !_execute_unlocked(
                    "UPDATE chart_sets SET is_present = 0 WHERE scan_generation <> ?;",
                    Array::make(p_scan_generation))) {
        const int saved_code = last_error_code_;
        const String saved_message = last_error_message_;
        _execute_script_unlocked("ROLLBACK TO dansusql_finish_scan; RELEASE dansusql_finish_scan;");
        last_error_code_ = saved_code;
        last_error_message_ = saved_message;
        return false;
    }
    return _execute_script_unlocked("RELEASE dansusql_finish_scan;");
}

bool DansuDB::set_chart_present(int64_t p_chart_id, bool p_present) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    if (!_execute_unlocked(
                "UPDATE charts SET is_present = ? WHERE id = ?;",
                Array::make(p_present, p_chart_id))) {
        return false;
    }
    if (sqlite3_changes64(database_) == 0) {
        _set_error_unlocked(SQLITE_NOTFOUND, "Chart was not found");
        return false;
    }

    if (p_present && !_execute_unlocked(R"SQL(
        UPDATE chart_sets
        SET is_present = 1
        WHERE id = (SELECT chart_set_id FROM charts WHERE id = ?);
    )SQL", Array::make(p_chart_id))) {
        return false;
    }
    return true;
}

Array DansuDB::_get_chart_sets_rows(bool p_present_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    return _query_unlocked(R"SQL(
        SELECT
            cs.id,
            cs.uuid,
            cs.folder_name,
            cs.is_present,
            COUNT(CASE WHEN c.is_present = 1 THEN 1 END) AS chart_count
        FROM chart_sets cs
        LEFT JOIN charts c ON c.chart_set_id = cs.id
        WHERE (? = 0 OR cs.is_present = 1)
        GROUP BY cs.id
        ORDER BY cs.folder_name COLLATE NOCASE, cs.id;
    )SQL", Array::make(p_present_only));
}

Array DansuDB::_get_charts_rows(int p_sort, const String &p_search, bool p_present_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    String order_clause;
    switch (p_sort) {
        case SORT_ARTIST:
            order_clause = " ORDER BY c.artist COLLATE NOCASE, c.title COLLATE NOCASE, c.rating;";
            break;
        case SORT_RATING:
            order_clause = " ORDER BY c.rating DESC, c.title COLLATE NOCASE, c.id;";
            break;
        case SORT_RECENT:
            order_clause = " ORDER BY history_stats.last_played_at IS NULL, history_stats.last_played_at DESC, c.title COLLATE NOCASE, c.id;";
            break;
        case SORT_TITLE:
        default:
            order_clause = " ORDER BY c.title COLLATE NOCASE, c.artist COLLATE NOCASE, c.rating;";
            break;
    }

    const String stripped_search = p_search.strip_edges();
    const String sql = chart_select_sql() + R"SQL(
        WHERE
            (? = 0 OR (c.is_present = 1 AND cs.is_present = 1))
            AND (? = '' OR c.search_text LIKE ? ESCAPE '\')
    )SQL" + order_clause;

    Array charts = _query_unlocked(
            sql,
            Array::make(p_present_only, stripped_search, escaped_like_pattern(stripped_search)));
    if (last_error_code_ != SQLITE_OK) {
        return Array();
    }
    if (!_attach_timings_unlocked(charts)) {
        return Array();
    }
    return charts;
}

Dictionary DansuDB::_get_chart_row(int64_t p_chart_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    Array charts = _query_unlocked(
            chart_select_sql() + " WHERE c.id = ?;",
            Array::make(p_chart_id));
    if (charts.is_empty()) {
        if (last_error_code_ == SQLITE_OK) {
            _set_error_unlocked(SQLITE_NOTFOUND, "Chart was not found");
        }
        return Dictionary();
    }
    if (!_attach_timings_unlocked(charts)) {
        return Dictionary();
    }
    return charts[0];
}

Array DansuDB::load_chart_library(
        const Callable &p_chart_set_factory,
        const Callable &p_chart_factory,
        const Callable &p_timing_factory,
        bool p_present_only,
        bool p_filesystem_validated) {
    if (!p_chart_set_factory.is_valid() || !p_chart_factory.is_valid() || !p_timing_factory.is_valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        _set_error_unlocked(SQLITE_MISUSE, "chart factories must be valid");
        return Array();
    }

    const Array set_rows = _get_chart_sets_rows(p_present_only);
    if (get_last_error_code() != SQLITE_OK) {
        return Array();
    }
    const Array chart_rows = _get_charts_rows(SORT_TITLE, String(), p_present_only);
    if (get_last_error_code() != SQLITE_OK) {
        return Array();
    }

    Array chart_sets;
    Dictionary chart_sets_by_id;
    for (int index = 0; index < set_rows.size(); ++index) {
        const Dictionary row = set_rows[index];
        const int64_t set_id = dictionary_int(row, "id", -1);
        const Variant chart_set_value = p_chart_set_factory.call(set_id);
        if (chart_set_value.get_type() != Variant::OBJECT || static_cast<Object *>(chart_set_value) == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            _set_error_unlocked(SQLITE_MISMATCH, "chart_set_factory must return an object");
            return Array();
        }

        Object *chart_set = chart_set_value;
        chart_set->set("db_id", set_id);
        chart_set->set("uuid", row.get("uuid", String()));
        chart_set->set("folder_name", row.get("folder_name", String()));
        const Variant charts_value = chart_set->get("charts");
        if (charts_value.get_type() != Variant::ARRAY) {
            std::lock_guard<std::mutex> lock(mutex_);
            _set_error_unlocked(SQLITE_MISMATCH, "chart set object must expose a charts Array");
            return Array();
        }
        Array charts = charts_value;
        charts.clear();
        chart_sets_by_id[set_id] = chart_set_value;
        chart_sets.append(chart_set_value);
    }

    for (int index = 0; index < chart_rows.size(); ++index) {
        const Dictionary row = chart_rows[index];
        const int64_t chart_id = dictionary_int(row, "id", -1);
        const int64_t set_id = dictionary_int(row, "chart_set_id", -1);
        if (!chart_sets_by_id.has(set_id)) {
            continue;
        }

        const Variant chart_value = p_chart_factory.call(chart_id);
        if (chart_value.get_type() != Variant::OBJECT || static_cast<Object *>(chart_value) == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            _set_error_unlocked(SQLITE_MISMATCH, "chart_factory must return an object");
            return Array();
        }

        Object *chart = chart_value;
        const Variant chart_set_value = chart_sets_by_id[set_id];
        Object *chart_set = chart_set_value;
        chart->set("db_id", chart_id);
        chart->set("chart_set", chart_set_value);
        chart->set("folder_name", row.get("folder_name", String()));
        chart->set("file_name", row.get("file_name", String()));
        chart->set("uuid", row.get("uuid", String()));
        chart->set("version", row.get("version", 1));
        chart->set("file_modified_time", row.get("file_modified_time", 0));
        chart->set("file_size", row.get("file_size", 0));
        chart->set("filehash", row.get("file_hash", String()));
        chart->set("is_built_in", row.get("is_built_in", false));
        chart->set("title", row.get("title", String("?")));
        chart->set("artist", row.get("artist", String("?")));
        chart->set("creator", row.get("creator", String("?")));
        chart->set("source", row.get("source", String("?")));
        chart->set("tags", row.get("tags", String()));
        chart->set("difficulty", row.get("difficulty", String("?")));
        const Variant rating = row.get("rating", Variant());
        const bool has_rating = (rating.get_type() == Variant::FLOAT || rating.get_type() == Variant::INT) && static_cast<double>(rating) >= 0.0;
        chart->set("rating", has_rating ? rating : Variant(0.0));
        chart->set("rating_calculated", has_rating);
        chart->set("preview_time", row.get("preview_time", -1.0));
        chart->set("play_time_ms", row.get("play_time_ms", 0));
        chart->set("file_audio", row.get("file_audio", String()));
        chart->set("file_cover_art", row.get("file_cover_art", String()));
        chart->set("file_skin", row.get("file_skin", String()));
        chart->set("last_played_at", row.get("last_played_at", 0));
        chart->set("best_score", row.get("best_score", 0.0));
        chart->set("play_count", row.get("play_count", 0));
        chart->set("current_version_play_count", row.get("current_version_play_count", 0));

        const bool is_present = dictionary_bool(row, "is_present", true);
        chart->set("availability", is_present ? (p_filesystem_validated ? 1 : 0) : 2);

        const Variant timings_value = chart->get("timings");
        if (timings_value.get_type() != Variant::ARRAY) {
            std::lock_guard<std::mutex> lock(mutex_);
            _set_error_unlocked(SQLITE_MISMATCH, "chart object must expose a timings Array");
            return Array();
        }
        Array timings = timings_value;
        timings.clear();
        const Array timing_rows = row.get("timings", Array());
        for (int timing_index = 0; timing_index < timing_rows.size(); ++timing_index) {
            const Variant timing_value = p_timing_factory.call();
            if (timing_value.get_type() != Variant::OBJECT || static_cast<Object *>(timing_value) == nullptr) {
                std::lock_guard<std::mutex> lock(mutex_);
                _set_error_unlocked(SQLITE_MISMATCH, "timing_factory must return an object");
                return Array();
            }
            const Dictionary timing_row = timing_rows[timing_index];
            Object *timing = timing_value;
            timing->set("time", timing_row.get("time", 0));
            timing->set("bpm", timing_row.get("bpm", 1.0));
            timings.append(timing_value);
        }

        if (chart->has_method("build_search_string")) {
            chart->call("build_search_string");
        }
        Array set_charts = chart_set->get("charts");
        set_charts.append(chart_value);
    }

    return chart_sets;
}

int64_t DansuDB::_resolve_chart_id_for_play(Object *p_chart) {
    int64_t chart_id = static_cast<int64_t>(p_chart->get("db_id"));
    if (chart_id > 0) {
        return chart_id;
    }

    const String uuid = static_cast<String>(p_chart->get("uuid")).strip_edges();
    const Variant chart_set_value = p_chart->get("chart_set");
    Object *chart_set = chart_set_value.get_type() == Variant::OBJECT ? static_cast<Object *>(chart_set_value) : nullptr;
    const String chart_set_uuid = chart_set != nullptr ? static_cast<String>(chart_set->get("uuid")).strip_edges() : String();
    if (uuid.is_empty() || chart_set_uuid.is_empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        _set_error_unlocked(SQLITE_MISUSE, "chart and chart set UUIDs are required to record a play");
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    const Array existing = _query_unlocked(
            "SELECT id FROM charts WHERE uuid = ? COLLATE NOCASE LIMIT 1;",
            Array::make(uuid));
    if (!existing.is_empty()) {
        chart_id = dictionary_int(existing[0], "id", -1);
        p_chart->set("db_id", chart_id);
        return chart_id;
    }

    if (!_execute_script_unlocked("SAVEPOINT dansusql_record_play_chart;")) {
        return -1;
    }
    const String folder_name = chart_set != nullptr
            ? static_cast<String>(chart_set->get("folder_name")).strip_edges()
            : chart_set_uuid;
    const Array chart_set_rows = _query_unlocked(R"SQL(
        INSERT INTO chart_sets(uuid, folder_name, is_present, scan_generation)
        VALUES (?, ?, 0, 0)
        ON CONFLICT(uuid) DO UPDATE SET uuid = excluded.uuid
        RETURNING id;
    )SQL", Array::make(chart_set_uuid, folder_name.is_empty() ? chart_set_uuid : folder_name));
    if (chart_set_rows.is_empty()) {
        _execute_script_unlocked("ROLLBACK TO dansusql_record_play_chart; RELEASE dansusql_record_play_chart;");
        return -1;
    }

    String file_name = static_cast<String>(p_chart->get("file_name")).strip_edges();
    if (file_name.is_empty()) {
        file_name = uuid + String(".dansu");
    }
    const Array chart_rows = _query_unlocked(R"SQL(
        INSERT INTO charts(
            chart_set_id, file_name, uuid, file_hash,
            title, artist, creator, source, tags, difficulty,
            is_present, scan_generation
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0)
        ON CONFLICT(uuid) DO UPDATE SET uuid = excluded.uuid
        RETURNING id;
    )SQL", Array::make(
            dictionary_int(chart_set_rows[0], "id", -1),
            file_name,
            uuid,
            static_cast<String>(p_chart->get("filehash")),
            static_cast<String>(p_chart->get("title")),
            static_cast<String>(p_chart->get("artist")),
            static_cast<String>(p_chart->get("creator")),
            static_cast<String>(p_chart->get("source")),
            static_cast<String>(p_chart->get("tags")),
            static_cast<String>(p_chart->get("difficulty"))));
    if (chart_rows.is_empty() || !_execute_script_unlocked("RELEASE dansusql_record_play_chart;")) {
        _execute_script_unlocked("ROLLBACK TO dansusql_record_play_chart; RELEASE dansusql_record_play_chart;");
        return -1;
    }

    chart_id = dictionary_int(chart_rows[0], "id", -1);
    p_chart->set("db_id", chart_id);
    if (chart_set != nullptr) {
        chart_set->set("db_id", dictionary_int(chart_set_rows[0], "id", -1));
    }
    return chart_id;
}

int64_t DansuDB::record_play(Object *p_chart, Object *p_score) {
    if (p_chart == nullptr || p_score == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        _set_error_unlocked(SQLITE_MISUSE, "chart and score are required");
        return -1;
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const int64_t requested_played_at = static_cast<int64_t>(p_score->get("played_at"));
    const int64_t played_at = requested_played_at > 0 ? requested_played_at : now;
    String chart_hash = static_cast<String>(p_score->get("object_hash"));
    if (chart_hash.is_empty()) {
        chart_hash = static_cast<String>(p_chart->get("filehash"));
        p_score->set("object_hash", chart_hash);
    }

    const int64_t chart_id = _resolve_chart_id_for_play(p_chart);
    if (chart_id <= 0) {
        return -1;
    }

    Dictionary play;
    play["chart_id"] = chart_id;
    play["chart_hash"] = chart_hash;
    play["submission_id"] = p_score->get("submission_id");
    play["played_at"] = played_at;
    play["score_value"] = p_score->get("total_score");
    play["raw_score"] = p_score->get("score");
    play["max_score"] = p_score->get("max_score");
    play["notes"] = p_score->get("notes");
    play["perfect_plus"] = p_score->get("perfect_plus");
    play["perfect"] = p_score->get("perfect");
    play["great"] = p_score->get("great");
    play["ok"] = p_score->get("ok");
    play["bad"] = p_score->get("bad");
    play["miss"] = p_score->get("miss");
    play["high_combo"] = p_score->get("high_combo");
    play["avg_signed_timing"] = p_score->get("avg_signed_timings");
    play["unstable_rate"] = p_score->get("unstable_rate");
    play["scoring_version"] = p_score->get("scoring_version");

    const int64_t play_id = _record_play_record(play);
    if (play_id > 0) {
        p_score->set("db_id", play_id);
        p_score->set("chart_db_id", chart_id);
        p_score->set("played_at", played_at);
    }
    return play_id;
}

int64_t DansuDB::_record_play_record(const Dictionary &p_play) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();

    const int64_t chart_id = dictionary_int(p_play, "chart_id", -1);
    if (chart_id <= 0 || !p_play.has("score_value")) {
        _set_error_unlocked(SQLITE_MISUSE, "chart_id and score_value are required");
        return -1;
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();

    const Array rows = _query_unlocked(R"SQL(
        INSERT INTO play_records(
            chart_id, chart_hash, submission_id, played_at,
            score_value, raw_score, max_score,
            notes, perfect_plus, perfect, great, ok, bad, miss,
            high_combo, avg_signed_timing, unstable_rate, scoring_version
        ) VALUES (
            ?, ?, ?, ?,
            ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?
        )
        RETURNING id;
    )SQL", Array::make(
            chart_id,
            dictionary_string(p_play, "chart_hash"),
            dictionary_string(p_play, "submission_id"),
            dictionary_int(p_play, "played_at", now),
            dictionary_float(p_play, "score_value"),
            dictionary_float(p_play, "raw_score"),
            dictionary_float(p_play, "max_score"),
            dictionary_int(p_play, "notes"),
            dictionary_int(p_play, "perfect_plus"),
            dictionary_int(p_play, "perfect"),
            dictionary_int(p_play, "great"),
            dictionary_int(p_play, "ok"),
            dictionary_int(p_play, "bad"),
            dictionary_int(p_play, "miss"),
            dictionary_int(p_play, "high_combo"),
            dictionary_float(p_play, "avg_signed_timing"),
            dictionary_float(p_play, "unstable_rate"),
            dictionary_int(p_play, "scoring_version", 1)));

    return rows.is_empty() ? -1 : dictionary_int(rows[0], "id", -1);
}

Array DansuDB::get_recent_plays(const Callable &p_score_factory, int p_limit, int p_offset) {
    Array rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        rows = _query_unlocked(
                play_select_sql() + " ORDER BY p.played_at DESC, p.id DESC LIMIT ? OFFSET ?;",
                Array::make(clamped_limit(p_limit), clamped_offset(p_offset)));
    }
    return _play_rows_to_objects(rows, p_score_factory);
}

Array DansuDB::get_chart_plays(Object *p_chart, const Callable &p_score_factory, int p_limit, int p_offset) {
    if (p_chart == nullptr) {
        return Array();
    }
    Array rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        const String uuid = static_cast<String>(p_chart->get("uuid")).strip_edges();
        const String chart_filter = uuid.is_empty()
                ? String("p.chart_id = ?")
                : String("p.chart_id = (SELECT id FROM charts WHERE uuid = ? COLLATE NOCASE LIMIT 1)");
        const Variant identity = uuid.is_empty() ? p_chart->get("db_id") : Variant(uuid);
        rows = _query_unlocked(
                play_select_sql() + " WHERE " + chart_filter + " ORDER BY p.played_at DESC, p.id DESC LIMIT ? OFFSET ?;",
                Array::make(identity, clamped_limit(p_limit), clamped_offset(p_offset)));
    }
    return _play_rows_to_objects(rows, p_score_factory);
}

Variant DansuDB::get_best_play(Object *p_chart, const Callable &p_score_factory) {
    if (p_chart == nullptr) {
        return Variant();
    }
    Array rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_error_unlocked();
        const String uuid = static_cast<String>(p_chart->get("uuid")).strip_edges();
        String sql = play_select_sql() + (uuid.is_empty()
                ? String(" WHERE p.chart_id = ?")
                : String(" WHERE p.chart_id = (SELECT id FROM charts WHERE uuid = ? COLLATE NOCASE LIMIT 1)"));
        Array parameters = Array::make(uuid.is_empty() ? p_chart->get("db_id") : Variant(uuid));
        const String chart_hash = static_cast<String>(p_chart->get("filehash"));
        if (!chart_hash.is_empty()) {
            sql += " AND p.chart_hash = ?";
            parameters.append(chart_hash);
        }
        sql += " ORDER BY p.score_value DESC, p.played_at DESC, p.id DESC LIMIT 1;";
        rows = _query_unlocked(sql, parameters);
    }

    const Array scores = _play_rows_to_objects(rows, p_score_factory);
    return scores.is_empty() ? Variant() : scores[0];
}

Array DansuDB::_play_rows_to_objects(const Array &p_rows, const Callable &p_score_factory) {
    Array scores;
    if (!p_score_factory.is_valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        _set_error_unlocked(SQLITE_MISUSE, "score_factory must be valid");
        return scores;
    }

    for (int index = 0; index < p_rows.size(); ++index) {
        const Dictionary row = p_rows[index];
        const Variant score_value = p_score_factory.call(dictionary_int(row, "id", -1));
        if (score_value.get_type() != Variant::OBJECT || static_cast<Object *>(score_value) == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            _set_error_unlocked(SQLITE_MISMATCH, "score_factory must return an object");
            return Array();
        }

        Object *score = score_value;
        score->set("db_id", row.get("id", -1));
        score->set("chart_db_id", row.get("chart_id", -1));
        score->set("object_hash", row.get("chart_hash", String()));
        score->set("submission_id", dictionary_string(row, "submission_id"));
        score->set("played_at", row.get("played_at", 0));
        score->set("stored_total_score", row.get("score_value", 0.0));
        score->set("score", row.get("raw_score", 0.0));
        score->set("max_score", row.get("max_score", 0.0));
        score->set("notes", row.get("notes", 0));
        score->set("perfect_plus", row.get("perfect_plus", 0));
        score->set("perfect", row.get("perfect", 0));
        score->set("great", row.get("great", 0));
        score->set("ok", row.get("ok", 0));
        score->set("bad", row.get("bad", 0));
        score->set("miss", row.get("miss", 0));
        score->set("high_combo", row.get("high_combo", 0));
        score->set("stored_avg_signed_timing", row.get("avg_signed_timing", 0.0));
        score->set("unstable_rate", row.get("unstable_rate", 0.0));
        score->set("scoring_version", row.get("scoring_version", 1));
        score->set("chart_title", row.get("title", String()));
        score->set("chart_artist", row.get("artist", String()));
        score->set("chart_difficulty", row.get("difficulty", String()));
        score->set("chart_folder_name", row.get("folder_name", String()));
        score->set("chart_file_name", row.get("file_name", String()));
        scores.append(score_value);
    }

    return scores;
}

bool DansuDB::_attach_timings_unlocked(Array &p_charts) {
    Dictionary charts_by_id;
    for (int index = 0; index < p_charts.size(); ++index) {
        Dictionary chart = p_charts[index];
        chart["timings"] = Array();
        charts_by_id[chart["id"]] = chart;
    }

    if (charts_by_id.is_empty()) {
        return true;
    }

    constexpr int CHUNK_SIZE = 500;
    const int chart_count = static_cast<int>(p_charts.size());
    for (int start = 0; start < chart_count; start += CHUNK_SIZE) {
        const int end = std::min(start + CHUNK_SIZE, chart_count);
        String placeholders;
        Array parameters;
        for (int index = start; index < end; ++index) {
            if (!placeholders.is_empty()) {
                placeholders += ",";
            }
            placeholders += "?";
            const Dictionary chart = p_charts[index];
            parameters.append(chart["id"]);
        }

        const Array rows = _query_unlocked(
                String("SELECT chart_id, position, time, bpm FROM chart_timings WHERE chart_id IN (") +
                        placeholders + String(") ORDER BY chart_id, position;"),
                parameters);
        if (last_error_code_ != SQLITE_OK) {
            return false;
        }

        for (int index = 0; index < rows.size(); ++index) {
            const Dictionary row = rows[index];
            const Variant chart_id = row["chart_id"];
            Dictionary chart = charts_by_id[chart_id];
            Array timings = chart["timings"];
            Dictionary timing;
            timing["time"] = row["time"];
            timing["bpm"] = row["bpm"];
            timings.append(timing);
        }
    }
    return true;
}

bool DansuDB::_write_timings_unlocked(int64_t p_chart_id, const Array &p_timings) {
    if (!_execute_unlocked("DELETE FROM chart_timings WHERE chart_id = ?;", Array::make(p_chart_id))) {
        return false;
    }

    for (int position = 0; position < p_timings.size(); ++position) {
        if (p_timings[position].get_type() != Variant::DICTIONARY) {
            _set_error_unlocked(SQLITE_MISMATCH, "Each timing must be a Dictionary");
            return false;
        }

        const Dictionary timing = p_timings[position];
        const double bpm = dictionary_float(timing, "bpm", 1.0);
        if (bpm <= 0.0) {
            _set_error_unlocked(SQLITE_RANGE, "Timing bpm must be positive");
            return false;
        }

        if (!_execute_unlocked(R"SQL(
            INSERT INTO chart_timings(chart_id, position, time, bpm)
            VALUES (?, ?, ?, ?);
        )SQL", Array::make(
                p_chart_id,
                position,
                dictionary_int(timing, "time"),
                bpm))) {
            return false;
        }
    }
    return true;
}

} // namespace godot
