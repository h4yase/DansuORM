#include "dansu_db.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <sqlite3.h>

#include <cstring>

namespace godot {

namespace {

class StatementGuard {
public:
    explicit StatementGuard(sqlite3_stmt *p_statement) : statement_(p_statement) {}
    ~StatementGuard() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    StatementGuard(const StatementGuard &) = delete;
    StatementGuard &operator=(const StatementGuard &) = delete;

private:
    sqlite3_stmt *statement_ = nullptr;
};

String sqlite_message(sqlite3 *p_database, int p_code) {
    if (p_database != nullptr) {
        const char *message = sqlite3_errmsg(p_database);
        if (message != nullptr) {
            return String::utf8(message);
        }
    }

    const char *message = sqlite3_errstr(p_code);
    return message != nullptr ? String::utf8(message) : String("Unknown SQLite error");
}

} // namespace

DansuDB::~DansuDB() {
    std::lock_guard<std::mutex> lock(mutex_);
    _close_unlocked();
}

void DansuDB::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open", "path", "read_only"), &DansuDB::open, DEFVAL(false));
    ClassDB::bind_method(D_METHOD("close"), &DansuDB::close);
    ClassDB::bind_method(D_METHOD("is_open"), &DansuDB::is_open);
    ClassDB::bind_method(D_METHOD("get_path"), &DansuDB::get_path);

    ClassDB::bind_method(D_METHOD("begin_transaction", "immediate"), &DansuDB::begin_transaction, DEFVAL(true));
    ClassDB::bind_method(D_METHOD("commit"), &DansuDB::commit);
    ClassDB::bind_method(D_METHOD("rollback"), &DansuDB::rollback);
    ClassDB::bind_method(D_METHOD("is_in_transaction"), &DansuDB::is_in_transaction);

    ClassDB::bind_method(D_METHOD("prepare_rating_cache", "calculation_version"), &DansuDB::prepare_rating_cache);
    ClassDB::bind_method(D_METHOD("begin_chart_scan"), &DansuDB::begin_chart_scan);
    ClassDB::bind_method(D_METHOD("upsert_chart_set", "chart_set", "scan_generation"), &DansuDB::upsert_chart_set, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("touch_chart", "chart_id", "modified_time", "file_size", "scan_generation"), &DansuDB::touch_chart);
    ClassDB::bind_method(D_METHOD("upsert_chart", "chart", "scan_generation"), &DansuDB::upsert_chart, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("finish_chart_scan", "scan_generation"), &DansuDB::finish_chart_scan);
    ClassDB::bind_method(D_METHOD("set_chart_present", "chart_id", "present"), &DansuDB::set_chart_present);

    ClassDB::bind_method(
            D_METHOD("load_chart_library", "chart_set_factory", "chart_factory", "timing_factory", "present_only", "filesystem_validated"),
            &DansuDB::load_chart_library,
            DEFVAL(true),
            DEFVAL(false));
    ClassDB::bind_method(D_METHOD("record_play", "chart", "score"), &DansuDB::record_play);
    ClassDB::bind_method(D_METHOD("get_recent_plays", "score_factory", "limit", "offset"), &DansuDB::get_recent_plays, DEFVAL(50), DEFVAL(0));
    ClassDB::bind_method(D_METHOD("get_chart_plays", "chart", "score_factory", "limit", "offset"), &DansuDB::get_chart_plays, DEFVAL(50), DEFVAL(0));
    ClassDB::bind_method(D_METHOD("get_best_play", "chart", "score_factory"), &DansuDB::get_best_play);

    ClassDB::bind_method(D_METHOD("set_busy_timeout", "milliseconds"), &DansuDB::set_busy_timeout);
    ClassDB::bind_method(D_METHOD("get_busy_timeout"), &DansuDB::get_busy_timeout);

    ClassDB::bind_method(D_METHOD("get_last_insert_rowid"), &DansuDB::get_last_insert_rowid);
    ClassDB::bind_method(D_METHOD("get_changes"), &DansuDB::get_changes);
    ClassDB::bind_method(D_METHOD("get_total_changes"), &DansuDB::get_total_changes);

    ClassDB::bind_method(D_METHOD("get_last_error_code"), &DansuDB::get_last_error_code);
    ClassDB::bind_method(D_METHOD("get_last_error_message"), &DansuDB::get_last_error_message);
    ClassDB::bind_method(D_METHOD("clear_error"), &DansuDB::clear_error);

    ClassDB::bind_method(D_METHOD("get_schema_version"), &DansuDB::get_schema_version);

    ClassDB::bind_static_method("DansuDB", D_METHOD("get_sqlite_version"), &DansuDB::get_sqlite_version);
    ClassDB::bind_static_method("DansuDB", D_METHOD("get_bundled_sqlite_version"), &DansuDB::get_bundled_sqlite_version);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "busy_timeout"), "set_busy_timeout", "get_busy_timeout");

    BIND_ENUM_CONSTANT(SORT_TITLE);
    BIND_ENUM_CONSTANT(SORT_ARTIST);
    BIND_ENUM_CONSTANT(SORT_RATING);
    BIND_ENUM_CONSTANT(SORT_RECENT);
}

bool DansuDB::open(const String &p_path, bool p_read_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    _close_unlocked();
    _clear_error_unlocked();

    if (p_path.strip_edges().is_empty()) {
        _set_error_unlocked(SQLITE_MISUSE, "Database path is empty");
        return false;
    }

    const String resolved_path = _resolve_path(p_path);
    if (!p_read_only && resolved_path != ":memory:" && !resolved_path.begins_with("file:")) {
        const String base_directory = resolved_path.get_base_dir();
        if (!base_directory.is_empty()) {
            const Error directory_error = DirAccess::make_dir_recursive_absolute(base_directory);
            if (directory_error != OK && directory_error != ERR_ALREADY_EXISTS) {
                _set_error_unlocked(SQLITE_CANTOPEN, "Failed to create database directory: " + base_directory);
                return false;
            }
        }
    }

    const int flags = p_read_only
            ? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI
            : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;

    const CharString utf8_path = resolved_path.utf8();
    const int result = sqlite3_open_v2(utf8_path.get_data(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
        _set_error_unlocked(result, "Failed to open database");
        _close_unlocked();
        return false;
    }

    path_ = resolved_path;
    sqlite3_extended_result_codes(database_, 1);
    sqlite3_busy_timeout(database_, busy_timeout_ms_);

    if (!_execute_script_unlocked("PRAGMA foreign_keys = ON;")) {
        _close_unlocked();
        return false;
    }

    if (!p_read_only) {
        if (!_execute_script_unlocked("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;")) {
            _close_unlocked();
            return false;
        }
        if (!_initialize_schema_unlocked()) {
            _close_unlocked();
            return false;
        }
    }

    _clear_error_unlocked();
    return true;
}

void DansuDB::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    _close_unlocked();
    _clear_error_unlocked();
}

bool DansuDB::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return database_ != nullptr;
}

String DansuDB::get_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

bool DansuDB::_execute_unlocked(const String &p_sql, const Variant &p_parameters) {
    sqlite3_stmt *statement = nullptr;
    if (!_prepare_unlocked(p_sql, &statement)) {
        return false;
    }
    StatementGuard guard(statement);

    if (!_bind_parameters_unlocked(statement, p_parameters)) {
        return false;
    }

    int result = SQLITE_OK;
    do {
        result = sqlite3_step(statement);
    } while (result == SQLITE_ROW);

    if (result != SQLITE_DONE) {
        _set_error_unlocked(result, "Failed to execute SQL statement");
        return false;
    }

    return true;
}

Array DansuDB::_query_unlocked(const String &p_sql, const Variant &p_parameters) {
    Array rows;
    sqlite3_stmt *statement = nullptr;
    if (!_prepare_unlocked(p_sql, &statement)) {
        return rows;
    }
    StatementGuard guard(statement);

    if (!_bind_parameters_unlocked(statement, p_parameters)) {
        return rows;
    }

    const int column_count = sqlite3_column_count(statement);
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        Dictionary row;
        for (int column = 0; column < column_count; ++column) {
            const char *raw_name = sqlite3_column_name(statement, column);
            const String name = raw_name != nullptr ? String::utf8(raw_name) : String::num_int64(column);
            row[name] = _column_value_unlocked(statement, column);
        }
        rows.append(row);
    }

    if (result != SQLITE_DONE) {
        rows.clear();
        _set_error_unlocked(result, "Failed to query SQL statement");
    }

    return rows;
}

bool DansuDB::begin_transaction(bool p_immediate) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    return _execute_script_unlocked(p_immediate ? "BEGIN IMMEDIATE;" : "BEGIN;");
}

bool DansuDB::commit() {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    return _execute_script_unlocked("COMMIT;");
}

bool DansuDB::rollback() {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    return _execute_script_unlocked("ROLLBACK;");
}

bool DansuDB::is_in_transaction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return database_ != nullptr && sqlite3_get_autocommit(database_) == 0;
}

void DansuDB::set_busy_timeout(int p_milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_timeout_ms_ = p_milliseconds < 0 ? 0 : p_milliseconds;
    if (database_ != nullptr) {
        sqlite3_busy_timeout(database_, busy_timeout_ms_);
    }
}

int DansuDB::get_busy_timeout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_timeout_ms_;
}

int64_t DansuDB::get_last_insert_rowid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return database_ != nullptr ? sqlite3_last_insert_rowid(database_) : 0;
}

int64_t DansuDB::get_changes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return database_ != nullptr ? sqlite3_changes64(database_) : 0;
}

int64_t DansuDB::get_total_changes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return database_ != nullptr ? sqlite3_total_changes64(database_) : 0;
}

int DansuDB::get_last_error_code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_code_;
}

String DansuDB::get_last_error_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_message_;
}

void DansuDB::clear_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
}

int DansuDB::get_schema_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_error_unlocked();
    const Array rows = _query_unlocked("PRAGMA user_version;");
    if (rows.is_empty()) {
        return -1;
    }

    const Dictionary row = rows[0];
    return static_cast<int>(row.get("user_version", -1));
}

String DansuDB::get_sqlite_version() {
    return String::utf8(sqlite3_libversion());
}

String DansuDB::get_bundled_sqlite_version() {
    return DANSUSQL_SQLITE_VERSION;
}

void DansuDB::_close_unlocked() {
    if (database_ != nullptr) {
        const int result = sqlite3_close_v2(database_);
        if (result != SQLITE_OK) {
            UtilityFunctions::push_warning("DansuSQLite: sqlite3_close_v2 failed with code ", result);
        }
        database_ = nullptr;
    }
    path_ = String();
}

void DansuDB::_clear_error_unlocked() {
    last_error_code_ = SQLITE_OK;
    last_error_message_ = String();
}

void DansuDB::_set_error_unlocked(int p_code, const String &p_context) {
    last_error_code_ = p_code;
    last_error_message_ = sqlite_message(database_, p_code);
    if (!p_context.is_empty()) {
        last_error_message_ = p_context + String(": ") + last_error_message_;
    }
}

bool DansuDB::_execute_script_unlocked(const String &p_sql) {
    if (database_ == nullptr) {
        _set_error_unlocked(SQLITE_MISUSE, "Database is not open");
        return false;
    }

    char *raw_error = nullptr;
    const CharString utf8_sql = p_sql.utf8();
    const int result = sqlite3_exec(database_, utf8_sql.get_data(), nullptr, nullptr, &raw_error);
    if (result != SQLITE_OK) {
        last_error_code_ = result;
        if (raw_error != nullptr) {
            last_error_message_ = String::utf8(raw_error);
            sqlite3_free(raw_error);
        } else {
            last_error_message_ = sqlite_message(database_, result);
        }
        return false;
    }
    return true;
}

bool DansuDB::_prepare_unlocked(const String &p_sql, sqlite3_stmt **r_statement) {
    if (database_ == nullptr) {
        _set_error_unlocked(SQLITE_MISUSE, "Database is not open");
        return false;
    }
    if (p_sql.strip_edges().is_empty()) {
        _set_error_unlocked(SQLITE_MISUSE, "SQL statement is empty");
        return false;
    }

    const CharString utf8_sql = p_sql.utf8();
    const char *tail = nullptr;
    const int result = sqlite3_prepare_v3(
            database_,
            utf8_sql.get_data(),
            static_cast<int>(utf8_sql.length()),
            SQLITE_PREPARE_PERSISTENT,
            r_statement,
            &tail);

    if (result != SQLITE_OK) {
        _set_error_unlocked(result, "Failed to prepare SQL statement");
        return false;
    }
    if (*r_statement == nullptr) {
        _set_error_unlocked(SQLITE_MISUSE, "SQL statement produced no executable statement");
        return false;
    }

    if (tail != nullptr && String::utf8(tail).strip_edges().length() > 0) {
        sqlite3_finalize(*r_statement);
        *r_statement = nullptr;
        _set_error_unlocked(SQLITE_MISUSE, "Only one SQL statement is allowed; use execute_script for scripts");
        return false;
    }

    return true;
}

bool DansuDB::_bind_parameters_unlocked(sqlite3_stmt *p_statement, const Variant &p_parameters) {
    const int expected_count = sqlite3_bind_parameter_count(p_statement);

    if (p_parameters.get_type() == Variant::NIL) {
        if (expected_count == 0) {
            return true;
        }
        _set_error_unlocked(SQLITE_RANGE, "SQL statement expects parameters");
        return false;
    }

    if (p_parameters.get_type() == Variant::ARRAY) {
        const Array parameters = p_parameters;
        if (parameters.size() != expected_count) {
            _set_error_unlocked(
                    SQLITE_RANGE,
                    "Expected " + String::num_int64(expected_count) + " parameters, received " + String::num_int64(parameters.size()));
            return false;
        }

        for (int index = 0; index < parameters.size(); ++index) {
            if (!_bind_value_unlocked(p_statement, index + 1, parameters[index])) {
                return false;
            }
        }
        return true;
    }

    if (p_parameters.get_type() == Variant::DICTIONARY) {
        const Dictionary parameters = p_parameters;
        for (int index = 1; index <= expected_count; ++index) {
            const char *raw_name = sqlite3_bind_parameter_name(p_statement, index);
            if (raw_name == nullptr) {
                _set_error_unlocked(SQLITE_RANGE, "Dictionary parameters require named SQL placeholders");
                return false;
            }

            const String full_name = String::utf8(raw_name);
            const String short_name = full_name.length() > 1 ? full_name.substr(1) : full_name;
            Variant value;
            if (parameters.has(full_name)) {
                value = parameters[full_name];
            } else if (parameters.has(short_name)) {
                value = parameters[short_name];
            } else {
                _set_error_unlocked(SQLITE_RANGE, "Missing named parameter: " + full_name);
                return false;
            }

            if (!_bind_value_unlocked(p_statement, index, value)) {
                return false;
            }
        }
        return true;
    }

    _set_error_unlocked(SQLITE_MISMATCH, "Parameters must be an Array, Dictionary, or null");
    return false;
}

bool DansuDB::_bind_value_unlocked(sqlite3_stmt *p_statement, int p_index, const Variant &p_value) {
    int result = SQLITE_OK;
    switch (p_value.get_type()) {
        case Variant::NIL:
            result = sqlite3_bind_null(p_statement, p_index);
            break;
        case Variant::BOOL:
            result = sqlite3_bind_int64(p_statement, p_index, static_cast<bool>(p_value) ? 1 : 0);
            break;
        case Variant::INT:
            result = sqlite3_bind_int64(p_statement, p_index, static_cast<int64_t>(p_value));
            break;
        case Variant::FLOAT:
            result = sqlite3_bind_double(p_statement, p_index, static_cast<double>(p_value));
            break;
        case Variant::STRING:
        case Variant::STRING_NAME: {
            const String text = p_value;
            const CharString utf8_text = text.utf8();
            result = sqlite3_bind_text(
                    p_statement,
                    p_index,
                    utf8_text.get_data(),
                    static_cast<int>(utf8_text.length()),
                    SQLITE_TRANSIENT);
            break;
        }
        case Variant::PACKED_BYTE_ARRAY: {
            const PackedByteArray bytes = p_value;
            result = sqlite3_bind_blob64(
                    p_statement,
                    p_index,
                    bytes.is_empty() ? nullptr : bytes.ptr(),
                    static_cast<sqlite3_uint64>(bytes.size()),
                    SQLITE_TRANSIENT);
            break;
        }
        default:
            _set_error_unlocked(
                    SQLITE_MISMATCH,
                    "Unsupported parameter type at index " + String::num_int64(p_index) + ": " + Variant::get_type_name(p_value.get_type()));
            return false;
    }

    if (result != SQLITE_OK) {
        _set_error_unlocked(result, "Failed to bind parameter at index " + String::num_int64(p_index));
        return false;
    }
    return true;
}

Variant DansuDB::_column_value_unlocked(sqlite3_stmt *p_statement, int p_column) const {
    switch (sqlite3_column_type(p_statement, p_column)) {
        case SQLITE_INTEGER:
            return static_cast<int64_t>(sqlite3_column_int64(p_statement, p_column));
        case SQLITE_FLOAT:
            return sqlite3_column_double(p_statement, p_column);
        case SQLITE_TEXT: {
            const char *text = reinterpret_cast<const char *>(sqlite3_column_text(p_statement, p_column));
            const int byte_count = sqlite3_column_bytes(p_statement, p_column);
            return text != nullptr ? Variant(String::utf8(text, byte_count)) : Variant(String());
        }
        case SQLITE_BLOB: {
            const uint8_t *data = static_cast<const uint8_t *>(sqlite3_column_blob(p_statement, p_column));
            const int byte_count = sqlite3_column_bytes(p_statement, p_column);
            PackedByteArray bytes;
            bytes.resize(byte_count);
            if (byte_count > 0 && data != nullptr) {
                std::memcpy(bytes.ptrw(), data, static_cast<size_t>(byte_count));
            }
            return bytes;
        }
        case SQLITE_NULL:
        default:
            return Variant();
    }
}

String DansuDB::_resolve_path(const String &p_path) const {
    if (p_path.begins_with("user://") || p_path.begins_with("res://")) {
        return ProjectSettings::get_singleton()->globalize_path(p_path);
    }
    return p_path;
}

} // namespace godot
