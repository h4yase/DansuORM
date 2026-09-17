#ifndef DANSU_DB_HPP
#define DANSU_DB_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

namespace godot {

class Object;

class DansuDB : public RefCounted {
    GDCLASS(DansuDB, RefCounted)

public:
    enum ChartSort {
        SORT_TITLE = 0,
        SORT_ARTIST = 1,
        SORT_RATING = 2,
        SORT_RECENT = 3,
    };

    DansuDB() = default;
    ~DansuDB() override;

    bool open(const String &p_path, bool p_read_only = false);
    void close();
    bool is_open() const;
    String get_path() const;

    bool begin_transaction(bool p_immediate = true);
    bool commit();
    bool rollback();
    bool is_in_transaction() const;

    bool prepare_rating_cache(int p_calculation_version);
    int64_t begin_chart_scan();
    int64_t upsert_chart_set(Object *p_chart_set, int64_t p_scan_generation = 0);
    bool touch_chart(int64_t p_chart_id, int64_t p_modified_time, int64_t p_file_size, int64_t p_scan_generation);
    int64_t upsert_chart(Object *p_chart, int64_t p_scan_generation = 0);
    bool finish_chart_scan(int64_t p_scan_generation);
    bool set_chart_present(int64_t p_chart_id, bool p_present);

    Array load_chart_library(
            const Callable &p_chart_set_factory,
            const Callable &p_chart_factory,
            const Callable &p_timing_factory,
            bool p_present_only = true,
            bool p_filesystem_validated = false);
    int64_t record_play(Object *p_chart, Object *p_score);
    Array get_recent_plays(const Callable &p_score_factory, int p_limit = 50, int p_offset = 0);
    Array get_chart_plays(Object *p_chart, const Callable &p_score_factory, int p_limit = 50, int p_offset = 0);
    Variant get_best_play(Object *p_chart, const Callable &p_score_factory);

    void set_busy_timeout(int p_milliseconds);
    int get_busy_timeout() const;

    int64_t get_last_insert_rowid() const;
    int64_t get_changes() const;
    int64_t get_total_changes() const;

    int get_last_error_code() const;
    String get_last_error_message() const;
    void clear_error();

    int get_schema_version();

    static String get_sqlite_version();
    static String get_bundled_sqlite_version();

protected:
    static void _bind_methods();

private:
    sqlite3 *database_ = nullptr;
    String path_;
    int busy_timeout_ms_ = 5000;
    int last_error_code_ = 0;
    String last_error_message_;
    mutable std::mutex mutex_;

    void _close_unlocked();
    void _clear_error_unlocked();
    void _set_error_unlocked(int p_code, const String &p_context = String());
    bool _execute_unlocked(const String &p_sql, const Variant &p_parameters = Variant());
    Array _query_unlocked(const String &p_sql, const Variant &p_parameters = Variant());
    bool _execute_script_unlocked(const String &p_sql);
    bool _initialize_schema_unlocked();
    int64_t _resolve_chart_id_for_play(Object *p_chart);
    int64_t _upsert_chart_record(const Dictionary &p_chart, int64_t p_scan_generation);
    int64_t _record_play_record(const Dictionary &p_play);
    Array _get_chart_sets_rows(bool p_present_only);
    Array _get_charts_rows(int p_sort, const String &p_search, bool p_present_only);
    Dictionary _get_chart_row(int64_t p_chart_id);
    Array _play_rows_to_objects(const Array &p_rows, const Callable &p_score_factory);
    bool _attach_timings_unlocked(Array &p_charts);
    bool _write_timings_unlocked(int64_t p_chart_id, const Array &p_timings);
    bool _prepare_unlocked(const String &p_sql, sqlite3_stmt **r_statement);
    bool _bind_parameters_unlocked(sqlite3_stmt *p_statement, const Variant &p_parameters);
    bool _bind_value_unlocked(sqlite3_stmt *p_statement, int p_index, const Variant &p_value);
    Variant _column_value_unlocked(sqlite3_stmt *p_statement, int p_column) const;
    String _resolve_path(const String &p_path) const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DansuDB::ChartSort)

#endif // DANSU_DB_HPP
