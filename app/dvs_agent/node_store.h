#pragma once
#include <json-c/json.h>
#include <sqlite3.h>
#include <string>
#include <mutex>

class NodeStore {
public:
    NodeStore(const std::string& db_path);
    ~NodeStore();

    // node
    json_object* node();          // borrowed
    void save_node();
    void update_node(json_object* patch);

    // config state machine
    json_object* config();        // borrowed, object with 6 scopes
    json_object* get_config(const std::string& scope, const std::string& node_id);
    json_object* set_desired(const std::string& scope, json_object* value, const char* opid);
    json_object* mark_applied(const std::string& scope, int revision);
    json_object* mark_failed(const std::string& scope, const char* code, const char* reason);
    json_object* retry(const std::string& scope);
    json_object* all_states();
    json_object* config_revision();

    bool verify_password(const std::string& user, const std::string& pass);
    void set_password(const std::string& pass);

    static const char* SCOPES[6];

private:
    void ensure_schema();
    void load();
    void save_config();
    std::string hash(const std::string& p);
    sqlite3* db_;
    std::mutex mtx_;
    json_object* node_;
    json_object* config_;
};
