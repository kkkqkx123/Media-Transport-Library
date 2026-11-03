#include "platform_store.h"

namespace {
const int64_t kTombstoneTtlSec = 30LL * 24 * 3600;
} // namespace

PlatformStore::PlatformStore(const std::string& db_path) {
    sqlite3_open(db_path.c_str(), &db_);
    ensure_schema();
}

PlatformStore::~PlatformStore() {
    if (db_) sqlite3_close(db_);
}

void PlatformStore::ensure_schema() {
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS docs(kind TEXT, id TEXT, data TEXT,"
        " PRIMARY KEY(kind,id));"
        "CREATE TABLE IF NOT EXISTS tombstones(id TEXT PRIMARY KEY,"
        " expires_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS audit_log(seq INTEGER PRIMARY KEY"
        " AUTOINCREMENT, event TEXT);";
    char* err = nullptr;
    sqlite3_exec(db_, ddl, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

bool PlatformStore::put_doc(const std::string& kind, const std::string& id,
                            json_object* data) {
    std::lock_guard<std::mutex> lk(mu_);
    const char* s = data ? json_object_to_json_string(data) : "{}";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO docs(kind,id,data)"
                            " VALUES(?,?,?)", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s ? s : "{}", -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

json_object* PlatformStore::get_doc(const std::string& kind, const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT data FROM docs WHERE kind=? AND id=?",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    json_object* out = nullptr;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = (const char*)sqlite3_column_text(st, 0);
        if (s) out = json_tokener_parse(s);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<std::string> PlatformStore::list_ids(const std::string& kind) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> ids;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id FROM docs WHERE kind=?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = (const char*)sqlite3_column_text(st, 0);
        if (s) ids.emplace_back(s);
    }
    sqlite3_finalize(st);
    return ids;
}

bool PlatformStore::del_doc(const std::string& kind, const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM docs WHERE kind=? AND id=?",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

void PlatformStore::audit(const std::string& event_json) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO audit_log(event) VALUES(?)",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, event_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

json_object* PlatformStore::audit_list(int limit) {
    std::lock_guard<std::mutex> lk(mu_);
    json_object* arr = json_object_new_array();
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT event FROM audit_log ORDER BY seq DESC LIMIT ?",
                       -1, &st, nullptr);
    sqlite3_bind_int(st, 1, limit > 0 ? limit : 100);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = (const char*)sqlite3_column_text(st, 0);
        json_object* o = s ? json_tokener_parse(s) : nullptr;
        json_object_array_add(arr, o ? o : json_object_new_object());
    }
    sqlite3_finalize(st);
    json_object* d = json_object_new_object();
    json_object_object_add(d, "items", arr);
    return d;
}

void PlatformStore::tombstone_add(const std::string& id, int64_t now_sec) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO tombstones(id,expires_at)"
                            " VALUES(?,?)", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now_sec + kTombstoneTtlSec);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool PlatformStore::tombstone_check(const std::string& id, int64_t now_sec) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT expires_at FROM tombstones WHERE id=?",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool hit = false;
    if (sqlite3_step(st) == SQLITE_ROW)
        hit = (sqlite3_column_int64(st, 0) > now_sec);
    sqlite3_finalize(st);
    return hit;
}

void PlatformStore::tombstone_gc(int64_t now_sec) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM tombstones WHERE expires_at<=?",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, now_sec);
    sqlite3_step(st);
    sqlite3_finalize(st);
}
