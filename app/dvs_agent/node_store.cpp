#include "node_store.h"

#include <json-c/json.h>
#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

const char* NodeStore::SCOPES[6] = {"role", "input", "output", "network", "registration", "sip"};

static const char* now_iso() {
    static char buf[64];
    time_t t = time(NULL);
    struct tm* tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

NodeStore::NodeStore(const std::string& db_path) : db_(nullptr), node_(nullptr), config_(nullptr) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        fprintf(stderr, "cannot open db: %s\n", sqlite3_errmsg(db_));
        return;
    }
    ensure_schema();
    load();
}

NodeStore::~NodeStore() {
    if (node_) json_object_put(node_);
    if (config_) json_object_put(config_);
    if (db_) sqlite3_close(db_);
}

static std::string get_kv(sqlite3* db, const char* key) {
    std::string out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k=?", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            if (t) out = reinterpret_cast<const char*>(t);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

static void set_kv(sqlite3* db, const char* key, const std::string& val) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO kv(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, val.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void NodeStore::ensure_schema() {
    char* err = nullptr;
    sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL)", nullptr, nullptr, &err);
    if (err) { fprintf(stderr, "%s\n", err); sqlite3_free(err); }
}

void NodeStore::load() {
    std::string n = get_kv(db_, "node");
    if (n.empty()) {
        // seed
        json_object* seed = json_object_new_object();
        json_object_object_add(seed, "id", json_object_new_string("local-node"));
        json_object_object_add(seed, "name", json_object_new_string("DVS Node"));
        json_object_object_add(seed, "type", json_object_new_string("output"));
        json_object_object_add(seed, "role", json_object_new_string("output"));
        json_object_object_add(seed, "model", json_object_new_string("DVS-NODE-R1"));
        json_object_object_add(seed, "board", json_object_new_string("RK-3588"));
        json_object_object_add(seed, "sn", json_object_new_string("RK3588-LOCAL"));
        json_object_object_add(seed, "mac", json_object_new_string("00:00:00:00:00:00"));
        json_object_object_add(seed, "ip", json_object_new_string("127.0.0.1"));
        json_object_object_add(seed, "fw", json_object_new_string("V1.0.0"));
        json_object_object_add(seed, "online", json_object_new_boolean(1));
        json_object_object_add(seed, "lastHeartbeat", json_object_new_null());
        json_object_object_add(seed, "registerMode", json_object_new_string("auto"));
        json_object_object_add(seed, "registerState", json_object_new_string("unregistered"));
        json_object_object_add(seed, "network", json_object_new_object());
        json_object_object_add(seed, "capabilities", json_object_new_object());
        json_object* outp = json_object_new_object();
        json_object_object_add(outp, "inputPorts", json_object_new_array());
        json_object_object_add(outp, "outputPorts", json_object_new_array());
        json_object_object_add(seed, "capabilities", outp);
        json_object_object_add(seed, "output", json_object_new_null());
        json_object_object_add(seed, "input", json_object_new_object());
        json_object* sip = json_object_new_object();
        json_object_object_add(sip, "enabled", json_object_new_boolean(0));
        json_object_object_add(sip, "state", json_object_new_string("disabled"));
        json_object_object_add(seed, "sip", sip);
        json_object_object_add(seed, "currentPlayback", json_object_new_null());
        json_object_object_add(seed, "exception", json_object_new_null());
        json_object_object_add(seed, "bind", json_object_new_null());
        json_object_object_add(seed, "lifecycle", json_object_new_string("active"));
        json_object_object_add(seed, "metrics", json_object_new_object());
        json_object* cs = json_object_new_object();
        for (int i = 0; i < 6; i++) json_object_object_add(cs, SCOPES[i], json_object_new_string("idle"));
        json_object_object_add(seed, "configStates", cs);
        json_object_object_add(seed, "configRevision", json_object_new_object());
        json_object_object_add(seed, "pendingRole", json_object_new_null());
        json_object_object_add(seed, "password_hash", json_object_new_string(hash("admin123").c_str()));
        set_kv(db_, "node", json_object_to_json_string(seed));
        json_object_put(seed);
        n = get_kv(db_, "node");
    }
    node_ = json_tokener_parse(n.c_str());

    std::string c = get_kv(db_, "config");
    if (c.empty()) {
        json_object* base = json_object_new_object();
        for (int i = 0; i < 6; i++) {
            json_object* st = json_object_new_object();
            json_object_object_add(st, "state", json_object_new_string("idle"));
            json_object_object_add(st, "desired", json_object_new_int(1));
            json_object_object_add(st, "applied", json_object_new_int(0));
            json_object_object_add(st, "desiredValue", json_object_new_object());
            json_object_object_add(st, "appliedValue", json_object_new_object());
            json_object_object_add(st, "operationId", json_object_new_null());
            json_object_object_add(st, "errorCode", json_object_new_null());
            json_object_object_add(st, "errorReason", json_object_new_null());
            json_object_object_add(st, "updatedAt", json_object_new_null());
            json_object_object_add(base, SCOPES[i], st);
        }
        set_kv(db_, "config", json_object_to_json_string(base));
        json_object_put(base);
        c = get_kv(db_, "config");
    }
    config_ = json_tokener_parse(c.c_str());
}

json_object* NodeStore::node() { return node_; }
json_object* NodeStore::config() { return config_; }

void NodeStore::save_node() {
    set_kv(db_, "node", json_object_to_json_string(node_));
}

void NodeStore::update_node(json_object* patch) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object_object_foreach(patch, key, val) {
        // merge dict if both dict
        json_object* existing = nullptr;
        if (json_object_object_get_ex(node_, key, &existing) && json_object_is_type(existing, json_type_object) && json_object_is_type(val, json_type_object)) {
            json_object_object_foreach(val, k2, v2) {
                json_object_object_add(existing, k2, json_object_get(v2));
            }
        } else {
            json_object_object_add(node_, key, json_object_get(val));
        }
    }
    // update configStates sync
    json_object* cs = nullptr;
    if (json_object_object_get_ex(node_, "configStates", &cs)) {
        // no-op, kept by caller
    }
    save_node();
}

json_object* NodeStore::get_config(const std::string& scope, const std::string& node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* st = nullptr;
    json_object_object_get_ex(config_, scope.c_str(), &st);
    json_object* out = json_object_new_object();
    json_object_object_add(out, "nodeId", json_object_new_string(node_id.c_str()));
    json_object_object_add(out, "scope", json_object_new_string(scope.c_str()));
    json_object* desired = nullptr, *applied = nullptr;
    json_object_object_get_ex(st, "desired", &desired);
    json_object_object_get_ex(st, "applied", &applied);
    json_object_object_add(out, "desiredRevision", json_object_get(desired));
    json_object_object_add(out, "appliedRevision", json_object_get(applied));
    json_object* state = nullptr;
    json_object_object_get_ex(st, "state", &state);
    json_object_object_add(out, "state", json_object_get(state));
    json_object* dv = nullptr, *av = nullptr;
    json_object_object_get_ex(st, "desiredValue", &dv);
    json_object_object_get_ex(st, "appliedValue", &av);
    json_object_object_add(out, "desiredValue", json_object_get(dv));
    json_object_object_add(out, "appliedValue", json_object_get(av));
    json_object* op = nullptr, *ec = nullptr, *er = nullptr, *ua = nullptr;
    json_object_object_get_ex(st, "operationId", &op);
    json_object_object_get_ex(st, "errorCode", &ec);
    json_object_object_get_ex(st, "errorReason", &er);
    json_object_object_get_ex(st, "updatedAt", &ua);
    json_object_object_add(out, "operationId", json_object_get(op));
    json_object_object_add(out, "errorCode", json_object_get(ec));
    json_object_object_add(out, "errorReason", json_object_get(er));
    json_object_object_add(out, "updatedAt", json_object_get(ua));
    return out;
}

json_object* NodeStore::set_desired(const std::string& scope, json_object* value, const char* opid) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* st = nullptr;
    json_object_object_get_ex(config_, scope.c_str(), &st);
    json_object* desired = nullptr;
    json_object_object_get_ex(st, "desired", &desired);
    int d = json_object_get_int(desired) + 1;
    json_object_object_del(st, "desired");
    json_object_object_add(st, "desired", json_object_new_int(d));
    json_object_object_del(st, "desiredValue");
    json_object_object_add(st, "desiredValue", json_object_get(value));
    json_object_object_del(st, "state");
    json_object_object_add(st, "state", json_object_new_string("pending_apply"));
    json_object_object_del(st, "operationId");
    json_object_object_add(st, "operationId", opid ? json_object_new_string(opid) : json_object_new_null());
    json_object_object_del(st, "errorCode");
    json_object_object_add(st, "errorCode", json_object_new_null());
    json_object_object_del(st, "errorReason");
    json_object_object_add(st, "errorReason", json_object_new_null());
    json_object_object_del(st, "updatedAt");
    json_object_object_add(st, "updatedAt", json_object_new_string(now_iso()));
    save_config();
    // update node configStates
    json_object* cs = nullptr;
    if (json_object_object_get_ex(node_, "configStates", &cs)) {
        json_object_object_del(cs, scope.c_str());
        json_object_object_add(cs, scope.c_str(), json_object_new_string("pending_apply"));
    }
    json_object* cr = nullptr;
    if (!json_object_object_get_ex(node_, "configRevision", &cr)) {
        cr = json_object_new_object();
        json_object_object_add(node_, "configRevision", cr);
    }
    json_object* r = json_object_new_object();
    json_object_object_add(r, "desired", json_object_new_int(d));
    json_object* applied = nullptr;
    json_object_object_get_ex(st, "applied", &applied);
    json_object_object_add(r, "applied", json_object_get(applied));
    json_object_object_del(cr, scope.c_str());
    json_object_object_add(cr, scope.c_str(), r);
    save_node();
    return nullptr;
}

json_object* NodeStore::mark_applied(const std::string& scope, int revision) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* st = nullptr;
    json_object_object_get_ex(config_, scope.c_str(), &st);
    if (revision > 0) {
        json_object_object_del(st, "applied");
        json_object_object_add(st, "applied", json_object_new_int(revision));
    } else {
        json_object* desired = nullptr;
        json_object_object_get_ex(st, "desired", &desired);
        json_object_object_del(st, "applied");
        json_object_object_add(st, "applied", json_object_new_int(json_object_get_int(desired)));
    }
    json_object* dv = nullptr;
    json_object_object_get_ex(st, "desiredValue", &dv);
    json_object_object_del(st, "appliedValue");
    json_object_object_add(st, "appliedValue", json_object_get(dv));
    json_object_object_del(st, "state");
    json_object_object_add(st, "state", json_object_new_string("applied"));
    json_object_object_del(st, "errorCode");
    json_object_object_add(st, "errorCode", json_object_new_null());
    json_object_object_del(st, "errorReason");
    json_object_object_add(st, "errorReason", json_object_new_null());
    json_object_object_del(st, "updatedAt");
    json_object_object_add(st, "updatedAt", json_object_new_string(now_iso()));
    save_config();
    json_object* cs = nullptr;
    if (json_object_object_get_ex(node_, "configStates", &cs)) {
        json_object_object_del(cs, scope.c_str());
        json_object_object_add(cs, scope.c_str(), json_object_new_string("applied"));
    }
    save_node();
    return nullptr;
}

json_object* NodeStore::mark_failed(const std::string& scope, const char* code, const char* reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* st = nullptr;
    json_object_object_get_ex(config_, scope.c_str(), &st);
    json_object_object_del(st, "state");
    json_object_object_add(st, "state", json_object_new_string("failed"));
    json_object_object_del(st, "errorCode");
    json_object_object_add(st, "errorCode", code ? json_object_new_string(code) : json_object_new_null());
    json_object_object_del(st, "errorReason");
    json_object_object_add(st, "errorReason", reason ? json_object_new_string(reason) : json_object_new_null());
    json_object_object_del(st, "updatedAt");
    json_object_object_add(st, "updatedAt", json_object_new_string(now_iso()));
    save_config();
    json_object* cs = nullptr;
    if (json_object_object_get_ex(node_, "configStates", &cs)) {
        json_object_object_del(cs, scope.c_str());
        json_object_object_add(cs, scope.c_str(), json_object_new_string("failed"));
    }
    save_node();
    return nullptr;
}

json_object* NodeStore::retry(const std::string& scope) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* st = nullptr;
    json_object_object_get_ex(config_, scope.c_str(), &st);
    json_object_object_del(st, "state");
    json_object_object_add(st, "state", json_object_new_string("pending_apply"));
    json_object_object_del(st, "errorCode");
    json_object_object_add(st, "errorCode", json_object_new_null());
    json_object_object_del(st, "errorReason");
    json_object_object_add(st, "errorReason", json_object_new_null());
    json_object_object_del(st, "updatedAt");
    json_object_object_add(st, "updatedAt", json_object_new_string(now_iso()));
    save_config();
    json_object* cs = nullptr;
    if (json_object_object_get_ex(node_, "configStates", &cs)) {
        json_object_object_del(cs, scope.c_str());
        json_object_object_add(cs, scope.c_str(), json_object_new_string("pending_apply"));
    }
    save_node();
    return nullptr;
}

json_object* NodeStore::all_states() {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* out = json_object_new_object();
    for (int i = 0; i < 6; i++) {
        json_object* st = nullptr;
        json_object_object_get_ex(config_, SCOPES[i], &st);
        json_object* s = nullptr;
        json_object_object_get_ex(st, "state", &s);
        json_object_object_add(out, SCOPES[i], json_object_new_string(json_object_get_string(s)));
    }
    return out;
}

json_object* NodeStore::config_revision() {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* out = json_object_new_object();
    for (int i = 0; i < 6; i++) {
        json_object* st = nullptr;
        json_object_object_get_ex(config_, SCOPES[i], &st);
        json_object* d = nullptr, *a = nullptr;
        json_object_object_get_ex(st, "desired", &d);
        json_object_object_get_ex(st, "applied", &a);
        json_object* r = json_object_new_object();
        json_object_object_add(r, "desired", json_object_new_int(json_object_get_int(d)));
        json_object_object_add(r, "applied", json_object_new_int(json_object_get_int(a)));
        json_object_object_add(out, SCOPES[i], r);
    }
    return out;
}

std::string NodeStore::hash(const std::string& p) {
    // simple sha256 not available without openssl; use std::hash (not crypto) for prototype
    std::hash<std::string> h;
    char buf[64];
    snprintf(buf, sizeof(buf), "%zu", h("dvs-salt:" + p));
    return std::string(buf);
}

bool NodeStore::verify_password(const std::string& user, const std::string& pass) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (user != "admin") return false;
    json_object* ph = nullptr;
    json_object_object_get_ex(node_, "password_hash", &ph);
    std::string stored = ph ? json_object_get_string(ph) : "";
    return stored == hash(pass);
}

void NodeStore::set_password(const std::string& pass) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object_object_del(node_, "password_hash");
    json_object_object_add(node_, "password_hash", json_object_new_string(hash(pass).c_str()));
    save_node();
}

void NodeStore::save_config() {
    set_kv(db_, "config", json_object_to_json_string(config_));
}
