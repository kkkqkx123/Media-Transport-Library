#include "dvs_contract.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

namespace dvs::contract {

const char* kHello = "hello";
const char* kWelcome = "welcome";
const char* kHeartbeat = "heartbeat";
const char* kLayoutPrepare = "layout.prepare";
const char* kBarrierAck = "barrier.ack";
const char* kConfigUpdate = "config.update";
const char* kConfigAck = "config.ack";
const char* kUpgradeTask = "upgrade.task";
const char* kUpgradeProgress = "upgrade.progress";
const char* kAck = "ack";
const char* kError = "error";

namespace {
std::atomic<uint64_t> g_req_seq{0};
std::atomic<uint64_t> g_msg_seq{0};

bool is_obj(json_object* o) {
    return o && json_object_is_type(o, json_type_object);
}
bool is_arr(json_object* o) {
    return o && json_object_is_type(o, json_type_array);
}
json_object* field(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v)) return v;
    return nullptr;
}
bool str_field(json_object* o, const char* k, std::string& out) {
    json_object* v = field(o, k);
    if (!v || !json_object_is_type(v, json_type_string)) return false;
    out = json_object_get_string(v);
    return true;
}
bool int_field(json_object* o, const char* k, int& out) {
    json_object* v = field(o, k);
    if (!v || (!json_object_is_type(v, json_type_int))) return false;
    out = json_object_get_int(v);
    return true;
}
bool rect_ok(json_object* o) {
    if (!is_obj(o)) return false;
    json_object* v = nullptr;
    const char* keys[4] = {"x", "y", "w", "h"};
    for (auto k : keys) {
        if (!json_object_object_get_ex(o, k, &v) || !v) return false;
        if (!json_object_is_type(v, json_type_int)) return false;
    }
    return true;
}
}  // namespace

std::string next_request_id(const char* origin) {
    uint64_t n = ++g_req_seq;
    char b[64];
    snprintf(b, sizeof(b), "%s-%llu", origin && *origin ? origin : "x",
             (unsigned long long)n);
    return b;
}

std::string next_msg_id(const char* origin) {
    uint64_t n = ++g_msg_seq;
    char b[64];
    snprintf(b, sizeof(b), "%s-m%llu", origin && *origin ? origin : "x",
             (unsigned long long)n);
    return b;
}

json_object* make_ok_with_id(json_object* data, const std::string& msg,
                             const std::string& request_id) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_int(200));
    json_object_object_add(o, "code", json_object_new_string("OK"));
    json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
    json_object_object_add(o, "data", data ? data : json_object_new_object());
    json_object_object_add(o, "requestId",
                           json_object_new_string(request_id.c_str()));
    return o;
}

json_object* make_ok(json_object* data, const std::string& msg, const char* origin) {
    return make_ok_with_id(data, msg, next_request_id(origin));
}

json_object* make_fail_with_id(const char* code, int status, const std::string& msg,
                               const std::string& request_id) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(status));
    json_object_object_add(o, "code", json_object_new_string(code ? code : "ERROR"));
    json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
    json_object_object_add(o, "data", json_object_new_object());
    json_object_object_add(o, "requestId",
                           json_object_new_string(request_id.c_str()));
    return o;
}

json_object* make_fail(const char* code, int status, const std::string& msg,
                       const char* origin) {
    return make_fail_with_id(code, status, msg, next_request_id(origin));
}

std::string strip_bearer(const std::string& v) {
    if (v.rfind("Bearer ", 0) == 0) return v.substr(7);
    return v;
}

bool bearer_ok(const std::string& provided, const std::string& expected) {
    if (expected.empty()) return true;
    std::string p = strip_bearer(provided);
    // 空令牌含义一致：需要校验时空令牌一律拒绝。
    if (p.empty()) return false;
    if (p.size() != expected.size()) return false;
    return p == expected;
}

std::string extract_bearer(const std::map<std::string, std::string>& headers) {
    for (auto& kv : headers) {
        std::string k = kv.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "authorization") return strip_bearer(kv.second);
    }
    return "";
}

std::string validate_hello(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:hello must be object";
    std::string id;
    json_object* v = field(data, "id");
    if (!v) v = field(data, "sn");
    if (!v || !json_object_is_type(v, json_type_string) ||
        json_object_get_string(v)[0] == '\0')
        return "INVALID_ARGUMENT:hello missing id/sn";
    json_object* geo = field(data, "geometry");
    if (!rect_ok(geo)) return "INVALID_ARGUMENT:hello geometry must be {x,y,w,h}";
    json_object* cap = field(data, "capability");
    if (cap == nullptr) cap = field(data, "capabilities");
    if (cap && !is_obj(cap)) return "INVALID_ARGUMENT:hello capability must be object";
    json_object* net = field(data, "network");
    if (net && !is_obj(net)) return "INVALID_ARGUMENT:hello network must be object";
    return "";
}

std::string validate_heartbeat(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:heartbeat must be object";
    std::string id;
    if (!str_field(data, "id", id) && !str_field(data, "nodeId", id))
        return "INVALID_ARGUMENT:heartbeat missing id";
    if (id.empty()) return "INVALID_ARGUMENT:heartbeat missing id";
    json_object* d = field(data, "lanesDigest");
    if (!is_obj(d)) return "INVALID_ARGUMENT:heartbeat missing lanesDigest";
    int ready = 0, total = 0;
    if (!int_field(d, "ready", ready) || !int_field(d, "total", total))
        return "INVALID_ARGUMENT:heartbeat lanesDigest ready/total must be int";
    if (ready < 0 || total < 0 || ready > total)
        return "INVALID_ARGUMENT:heartbeat lanesDigest range illegal";
    return "";
}

std::string validate_layout_prepare(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:layout.prepare must be object";
    std::string op;
    if (!str_field(data, "operationId", op) || op.empty())
        return "INVALID_ARGUMENT:layout.prepare missing operationId";
    json_object* rv = field(data, "revision");
    if (!rv || !json_object_is_type(rv, json_type_int))
        return "INVALID_ARGUMENT:layout.prepare revision must be int";
    json_object* at = field(data, "applyAtNs");
    if (!at || !json_object_is_type(at, json_type_int))
        return "INVALID_ARGUMENT:layout.prepare applyAtNs must be int";
    json_object* wins = field(data, "windows");
    if (!is_arr(wins)) return "INVALID_ARGUMENT:layout.prepare windows must be array";
    json_object* geo = field(data, "geometry");
    if (!rect_ok(geo)) return "INVALID_ARGUMENT:layout.prepare geometry must be {x,y,w,h}";
    return "";
}

std::string validate_barrier_ack(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:barrier.ack must be object";
    std::string op, node, st;
    if (!str_field(data, "operationId", op) || op.empty())
        return "INVALID_ARGUMENT:barrier.ack missing operationId";
    if (!str_field(data, "node", node) || node.empty())
        return "INVALID_ARGUMENT:barrier.ack missing node";
    if (!str_field(data, "status", st) || st.empty())
        return "INVALID_ARGUMENT:barrier.ack missing status";
    if (st != "ready" && st != "blocked")
        return "INVALID_ARGUMENT:barrier.ack status must be ready|blocked";
    json_object* lanes = field(data, "lanes");
    if (lanes && !is_arr(lanes))
        return "INVALID_ARGUMENT:barrier.ack lanes must be array";
    return "";
}

std::string validate_config_update(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:config.update must be object";
    json_object* cfg = field(data, "config");
    if (!is_obj(cfg)) return "INVALID_ARGUMENT:config.update missing config object";
    bool has_geo = false, has_role = false;
    json_object_object_foreach(cfg, key, val) {
        std::string k(key);
        if (k == "geometry") {
            if (!rect_ok(val))
                return "INVALID_ARGUMENT:config.update geometry must be {x,y,w,h}";
            has_geo = true;
        } else if (k == "role") {
            if (json_object_is_type(val, json_type_string)) {
                std::string r = json_object_get_string(val);
                if (r != "input" && r != "output")
                    return "INVALID_ARGUMENT:config.update role must be input|output";
            } else if (is_obj(val)) {
                json_object* rv = field(val, "role");
                if (!rv || !json_object_is_type(rv, json_type_string))
                    return "INVALID_ARGUMENT:config.update role.role must be string";
                std::string r = json_object_get_string(rv);
                if (r != "input" && r != "output")
                    return "INVALID_ARGUMENT:config.update role must be input|output";
            } else {
                return "INVALID_ARGUMENT:config.update role must be string|object";
            }
            has_role = true;
        } else {
            return std::string("INVALID_ARGUMENT:config.update unknown key: ") + k;
        }
    }
    if (!has_geo && !has_role)
        return "INVALID_ARGUMENT:config.update needs geometry and/or role";
    return "";
}

std::string validate_config_ack(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:config.ack must be object";
    std::string node;
    if (!str_field(data, "node", node) || node.empty())
        return "INVALID_ARGUMENT:config.ack missing node";
    json_object* av = field(data, "appliedRevision");
    if (av && !json_object_is_type(av, json_type_int))
        return "INVALID_ARGUMENT:config.ack appliedRevision must be int";
    return "";
}

std::string validate_upgrade_task(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:upgrade.task must be object";
    std::string tid, fn;
    if (!str_field(data, "taskId", tid) || tid.empty())
        return "INVALID_ARGUMENT:upgrade.task missing taskId";
    if (!str_field(data, "filename", fn) || fn.empty())
        return "INVALID_ARGUMENT:upgrade.task missing filename";
    json_object* sha = field(data, "sha256");
    if (sha && !json_object_is_type(sha, json_type_string))
        return "INVALID_ARGUMENT:upgrade.task sha256 must be string";
    return "";
}

std::string validate_upgrade_progress(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:upgrade.progress must be object";
    std::string tid, node, st;
    if (!str_field(data, "taskId", tid) || tid.empty())
        return "INVALID_ARGUMENT:upgrade.progress missing taskId";
    if (!str_field(data, "node", node) || node.empty())
        return "INVALID_ARGUMENT:upgrade.progress missing node";
    if (!str_field(data, "status", st) || st.empty())
        return "INVALID_ARGUMENT:upgrade.progress missing status";
    return "";
}

json_object* wrap(const std::string& msg_id, const std::string& type,
                 json_object* data, const std::string& request_id) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "msgId", json_object_new_string(msg_id.c_str()));
    json_object_object_add(o, "type", json_object_new_string(type.c_str()));
    json_object_object_add(o, "data", data ? data : json_object_new_object());
    json_object_object_add(o, "requestId",
                           json_object_new_string(request_id.c_str()));
    return o;
}

bool unwrap(json_object* msg, std::string& msg_id_out, std::string& type_out,
            json_object*& data_out, std::string& request_id_out) {
    data_out = nullptr;
    if (!is_obj(msg)) return false;
    json_object* m = field(msg, "msgId");
    json_object* t = field(msg, "type");
    json_object* d = field(msg, "data");
    if (!m || !json_object_is_type(m, json_type_string)) return false;
    if (!t || !json_object_is_type(t, json_type_string)) return false;
    if (!is_obj(d) && !is_arr(d)) return false;
    msg_id_out = json_object_get_string(m);
    type_out = json_object_get_string(t);
    data_out = json_object_get(d);
    json_object* r = field(msg, "requestId");
    request_id_out =
        (r && json_object_is_type(r, json_type_string)) ? json_object_get_string(r) : "";
    return !(msg_id_out.empty() || type_out.empty());
}

int heartbeat_interval_ms() {
    const char* v = std::getenv("DVS_HEARTBEAT_MS");
    if (!v || !*v) return 3000;
    int n = atoi(v);
    return n > 0 ? n : 3000;
}

int barrier_timeout_ms() {
    const char* v = std::getenv("DVS_BARRIER_TIMEOUT_MS");
    if (!v || !*v) return 10000;
    int n = atoi(v);
    return n > 0 ? n : 10000;
}

std::string transport_mode() {
    const char* v = std::getenv("DVS_TRANSPORT_MODE");
    std::string m = v ? v : "auto";
    for (auto& c : m) c = (char)tolower((unsigned char)c);
    if (m == "ws" || m == "short") return m;
    return "auto";
}

bool use_ws() { return transport_mode() != "short"; }
bool ws_only() { return transport_mode() == "ws"; }

}  // namespace dvs::contract
