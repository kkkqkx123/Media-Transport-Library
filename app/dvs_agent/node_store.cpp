#include "node_store.h"

#include "sha256.h"

#include <json-c/json.h>
#include <sqlite3.h>

#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

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
        json_object* outp = json_object_new_object();
        json_object* in_ports = json_object_new_array();
        json_object* in0 = json_object_new_object();
        json_object_object_add(in0, "id", json_object_new_string("HDMI1"));
        json_object_object_add(in0, "label", json_object_new_string("HDMI 输入 1"));
        json_object_object_add(in0, "kind", json_object_new_string("HDMI 2.0 RX"));
        json_object_object_add(in0, "maxWidth", json_object_new_int(3840));
        json_object_object_add(in0, "maxHeight", json_object_new_int(2160));
        json_object_object_add(in0, "maxRefreshHz", json_object_new_int(60));
        json_object_array_add(in_ports, in0);
        json_object_object_add(outp, "inputPorts", in_ports);
        json_object* out_ports = json_object_new_array();
        json_object* out0 = json_object_new_object();
        json_object_object_add(out0, "id", json_object_new_string("HDMI-A"));
        json_object_object_add(out0, "label", json_object_new_string("HDMI 输出 A"));
        json_object_object_add(out0, "kind", json_object_new_string("HDMI 2.1 TX"));
        json_object_object_add(out0, "maxWidth", json_object_new_int(7680));
        json_object_object_add(out0, "maxHeight", json_object_new_int(4320));
        json_object_object_add(out0, "maxRefreshHz", json_object_new_int(60));
        json_object_array_add(out_ports, out0);
        json_object_object_add(outp, "outputPorts", out_ports);
        json_object_object_add(outp, "maxDecodeSessions", json_object_new_int(4));
        json_object_object_add(outp, "maxLayers", json_object_new_int(4));
        /* TX static ceiling (SoC limits, not probed). The engine reports the
         * same shape live; agent merge prefers the engine value per-field. */
        json_object* txp = json_object_new_object();
        json_object* codecs = json_object_new_array();
        json_object_array_add(codecs, json_object_new_string("h264"));
        json_object_array_add(codecs, json_object_new_string("h265"));
        json_object_object_add(txp, "codecs", codecs);
        json_object_object_add(txp, "maxWidth", json_object_new_int(3840));
        json_object_object_add(txp, "maxHeight", json_object_new_int(2160));
        json_object_object_add(txp, "maxFps", json_object_new_int(60));
        json_object_object_add(txp, "maxBitrateKbps", json_object_new_int(20000));
        json_object_object_add(outp, "tx", txp);
        json_object* cap_ptp = json_object_new_object();
        json_object_object_add(cap_ptp, "state", json_object_new_string("unknown"));
        json_object_object_add(cap_ptp, "offsetUs", json_object_new_int(0));
        json_object_object_add(outp, "ptp", cap_ptp);
        json_object* cap_mtl = json_object_new_object();
        json_object_object_add(cap_mtl, "version", json_object_new_string("unknown"));
        json_object_object_add(cap_mtl, "backend", json_object_new_string("dpdk"));
        json_object_object_add(outp, "mtl", cap_mtl);
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
        json_object_object_add(seed, "password_hash", json_object_new_string(make_hash("admin123").c_str()));
        json_object_object_add(seed, "password_change_required", json_object_new_boolean(1));
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

static const int kPbkdf2Iter = 100000;
static const int kSaltBytes = 16;

std::string NodeStore::make_hash(const std::string& p) {
    uint8_t salt[kSaltBytes];
    ssize_t total = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = 0;
        while (total < kSaltBytes && (n = read(fd, salt + total, kSaltBytes - total)) > 0)
            total += n;
        close(fd);
    }
    if (fd < 0 || total != kSaltBytes) {
        /* deterministic fallback (still salted) if urandom unavailable */
        for (int i = 0; i < kSaltBytes; i++) salt[i] = (uint8_t)(i * 37 + 11);
    }
    uint8_t out[32];
    dvs::pbkdf2_hmac_sha256((const uint8_t*)p.data(), p.size(), salt, kSaltBytes,
                            kPbkdf2Iter, out);
    return "pbkdf2$" + dvs::to_hex(salt, kSaltBytes) + "$" +
           std::to_string(kPbkdf2Iter) + "$" + dvs::to_hex(out, 32);
}

bool NodeStore::check_hash(const std::string& stored, const std::string& p) {
    /* Legacy dev DBs used a bare std::hash. Migrate on successful verify. */
    if (stored.empty() || stored.rfind("pbkdf2$", 0) != 0) {
        std::hash<std::string> h;
        char buf[64];
        snprintf(buf, sizeof(buf), "%zu", h("dvs-salt:" + p));
        return stored == std::string(buf);
    }
    size_t p1 = stored.find('$', 7);          // after "pbkdf2$"
    size_t p2 = stored.find('$', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return false;
    std::string salt_hex = stored.substr(7, p1 - 7);
    std::string iter_str = stored.substr(p1 + 1, p2 - p1 - 1);
    std::string hash_hex = stored.substr(p2 + 1);

    uint8_t salt[64];
    size_t salt_len = 0;
    if (!dvs::from_hex(salt_hex.c_str(), salt, sizeof(salt), &salt_len)) return false;
    uint32_t iter = (uint32_t)strtoul(iter_str.c_str(), nullptr, 10);
    uint8_t exp[32];
    size_t exp_len = 0;
    if (!dvs::from_hex(hash_hex.c_str(), exp, sizeof(exp), &exp_len)) return false;
    if (exp_len != 32) return false;

    uint8_t out[32];
    dvs::pbkdf2_hmac_sha256((const uint8_t*)p.data(), p.size(), salt, salt_len,
                            iter, out);
    return memcmp(out, exp, 32) == 0;
}

bool NodeStore::verify_password(const std::string& user, const std::string& pass) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (user != "admin") return false;
    json_object* ph = nullptr;
    json_object_object_get_ex(node_, "password_hash", &ph);
    std::string stored = ph ? json_object_get_string(ph) : "";
    bool ok = check_hash(stored, pass);
    /* Migrate legacy hash to pbkdf2 on first successful login. */
    if (ok && stored.rfind("pbkdf2$", 0) != 0) {
        json_object_object_del(node_, "password_hash");
        json_object_object_add(node_, "password_hash",
                               json_object_new_string(make_hash(pass).c_str()));
        save_node();
    }
    return ok;
}

void NodeStore::set_password(const std::string& pass) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object_object_del(node_, "password_hash");
    json_object_object_add(node_, "password_hash",
                           json_object_new_string(make_hash(pass).c_str()));
    json_object_object_del(node_, "password_change_required");
    json_object_object_add(node_, "password_change_required",
                           json_object_new_boolean(0));
    save_node();
}

void NodeStore::set_node_id(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object_object_del(node_, "id");
    json_object_object_add(node_, "id", json_object_new_string(id.c_str()));
    save_node();
}

bool NodeStore::password_change_required() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* v = nullptr;
    if (json_object_object_get_ex(node_, "password_change_required", &v) && v)
        return json_object_get_boolean(v) ? true : false;
    return false;
}

void NodeStore::clear_password_change_required() {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object_object_del(node_, "password_change_required");
    json_object_object_add(node_, "password_change_required",
                           json_object_new_boolean(0));
    save_node();
}

bool NodeStore::apply_pending_role() {
    std::lock_guard<std::mutex> lk(mtx_);
    json_object* pr = nullptr;
    if (!json_object_object_get_ex(node_, "pendingRole", &pr) || pr == nullptr ||
        json_object_get_type(pr) == json_type_null) {
        return false;
    }
    json_object_object_del(node_, "role");
    json_object_object_add(node_, "role", json_object_get(pr));
    json_object_object_del(node_, "pendingRole");
    json_object_object_add(node_, "pendingRole", json_object_new_null());
    save_node();
    return true;
}

void NodeStore::save_config() {
    set_kv(db_, "config", json_object_to_json_string(config_));
}
