#include "platform_store.h"

#include <cstdlib>
#include <sys/stat.h>
#include <cstdio>

namespace {
int env_int_local(const char* k, int dflt, int lo, int hi) {
    const char* v = std::getenv(k);
    if (!v || !*v) return dflt;
    int n = atoi(v);
    if (n < lo) return lo;
    if (n > hi) return hi;
    return n;
}
int64_t tombstone_ttl_sec_local() {
    int d = env_int_local("DVS_TOMBSTONE_TTL_DAYS", 30, 1, 365);
    return (int64_t)d * 24 * 3600;
}
int audit_keep_rows_local() {
    return env_int_local("DVS_AUDIT_KEEP_ROWS", 5000, 100, 100000);
}
void ensure_parent_dir(const std::string& path) {
    if (path.empty() || path == ":memory:") return;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = path.substr(0, slash);
    std::string cur;
    for (size_t i = 0; i < dir.size(); i++) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
}
} // namespace

PlatformStore::PlatformStore(const std::string& db_path) {
    ensure_parent_dir(db_path);
    sqlite3_open(db_path.c_str(), &db_);
    if (db_) sqlite3_busy_timeout(db_, 5000);
    ensure_schema();
}

PlatformStore::~PlatformStore() {
    if (db_) sqlite3_close(db_);
}

void PlatformStore::ensure_schema() {
    if (!db_) return;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
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
    sqlite3_exec(db_, "PRAGMA user_version=1", nullptr, nullptr, nullptr);
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
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO audit_log(event) VALUES(?)",
                       -1, &st, nullptr);
    if (!st) return;
    sqlite3_bind_text(st, 1, event_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    char* err = nullptr;
    std::string trim =
        "DELETE FROM audit_log WHERE seq <= (SELECT MAX(seq)-" +
        std::to_string(audit_keep_rows_local()) + " FROM audit_log)";
    sqlite3_exec(db_, trim.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

void PlatformStore::audit_trim() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;
    char* err = nullptr;
    std::string trim =
        "DELETE FROM audit_log WHERE seq <= (SELECT MAX(seq)-" +
        std::to_string(audit_keep_rows_local()) + " FROM audit_log)";
    sqlite3_exec(db_, trim.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
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
    sqlite3_bind_int64(st, 2, now_sec + tombstone_ttl_sec_local());
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
