#pragma once
#include <json-c/json.h>
#include <sqlite3.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Generic document store over SQLite: docs(kind,id,data JSON).
// Kinds: node, group, wall, scene, binding, whitelist, playback.
// Plus tombstones(id, expires_at_sec) and a local audit log.
class PlatformStore {
public:
    explicit PlatformStore(const std::string& db_path);
    ~PlatformStore();

    bool put_doc(const std::string& kind, const std::string& id, json_object* data);
    json_object* get_doc(const std::string& kind, const std::string& id); // new ref or null
    std::vector<std::string> list_ids(const std::string& kind);
    bool del_doc(const std::string& kind, const std::string& id);

    void audit(const std::string& event_json);
    json_object* audit_list(int limit); // {items:[...]}
    // Drop oldest audit rows beyond the retention cap. Safe to call often.
    void audit_trim();

    // Tombstones: retired node ids rejected from re-register for 30 days.
    void tombstone_add(const std::string& id, int64_t now_sec);
    bool tombstone_check(const std::string& id, int64_t now_sec);
    void tombstone_gc(int64_t now_sec);

private:
    void ensure_schema();
    sqlite3* db_ = nullptr;
    std::mutex mu_;
};
