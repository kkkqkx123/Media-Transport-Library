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

// Optional clock summary attached to hello/heartbeat. Additive only:
// absent means unknown, present-but-malformed is rejected.

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
// Optional ptp summary: {state, offsetUs, domain}. Absent is unknown
// and valid; present must be an object with a non-empty state string and an
// integer offset. Frame clock and vsync stay additive and unchecked.
std::string ptp_opt_err(json_object* o) {
    json_object* p = field(o, "ptp");
    if (!p) return "";
    if (!is_obj(p)) return "INVALID_ARGUMENT:ptp must be object";
    std::string st;
    if (!str_field(p, "state", st) || st.empty())
        return "INVALID_ARGUMENT:ptp missing state";
    json_object* off = field(p, "offsetUs");
    if (off && !json_object_is_type(off, json_type_int))
        return "INVALID_ARGUMENT:ptp offsetUs must be int";
    return "";
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
    if (!str_field(data, "id", id) || id.empty())
        return "INVALID_ARGUMENT:hello missing id";
    json_object* geo = field(data, "geometry");
    if (!rect_ok(geo)) return "INVALID_ARGUMENT:hello geometry must be {x,y,w,h}";
    json_object* cap = field(data, "capability");
    if (cap && !is_obj(cap)) return "INVALID_ARGUMENT:hello capability must be object";
    if (field(data, "capabilities"))
        return "INVALID_ARGUMENT:hello capability must be object";
    json_object* net = field(data, "network");
    if (net && !is_obj(net)) return "INVALID_ARGUMENT:hello network must be object";
    std::string pe = ptp_opt_err(data);
    if (!pe.empty()) return pe;
    return "";
}

std::string validate_heartbeat(json_object* data) {
    if (!is_obj(data)) return "INVALID_ARGUMENT:heartbeat must be object";
    std::string id;
    if (!str_field(data, "id", id) || id.empty())
        return "INVALID_ARGUMENT:heartbeat missing id";
    json_object* d = field(data, "lanesDigest");
    if (!is_obj(d)) return "INVALID_ARGUMENT:heartbeat missing lanesDigest";
    int ready = 0, total = 0;
    if (!int_field(d, "ready", ready) || !int_field(d, "total", total))
        return "INVALID_ARGUMENT:heartbeat lanesDigest ready/total must be int";
    if (ready < 0 || total < 0 || ready > total)
        return "INVALID_ARGUMENT:heartbeat lanesDigest range illegal";
    std::string pe = ptp_opt_err(data);
    if (!pe.empty()) return pe;
    return "";
}

namespace {
bool valid_ip4_literal(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int parts = 0;
    const char* p = s.c_str();
    while (true) {
        if (*p < '0' || *p > '9') return false;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) return false;
        }
        if (v > 255) return false;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return false;
        p++;
        if (*p == '\0') return false;
    }
    return parts == 4;
}
}  // namespace

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
    // Frame-anchored commit (optional, additive): when present the node maps
    // applyAtNs to the target frame instead of sleeping to a bare timestamp.
    json_object* ep = field(data, "epochTaiNs");
    if (ep) {
        if (!json_object_is_type(ep, json_type_int) || json_object_get_int64(ep) < 0)
            return "INVALID_ARGUMENT:layout.prepare epochTaiNs must be int >= 0";
    }
    json_object* fp = field(data, "fps");
    if (fp) {
        double f = 0.0;
        if (json_object_is_type(fp, json_type_int))
            f = (double)json_object_get_int(fp);
        else if (json_object_is_type(fp, json_type_double))
            f = json_object_get_double(fp);
        else
            return "INVALID_ARGUMENT:layout.prepare fps must be numeric";
        if (!(f > 0.0) || f > 240.0)
            return "INVALID_ARGUMENT:layout.prepare fps must be 0 < fps <= 240";
    }
    json_object* fi = field(data, "frameIndex");
    if (fi) {
        if (!json_object_is_type(fi, json_type_int) || json_object_get_int64(fi) < 0)
            return "INVALID_ARGUMENT:layout.prepare frameIndex must be int >= 0";
    }
    json_object* wins = field(data, "windows");
    if (!is_arr(wins)) return "INVALID_ARGUMENT:layout.prepare windows must be array";
    // Per-fragment window shape (platform fragment_for_node output): each
    // element names its window and signal and carries the node-local target
    // rect plus the source crop. Key-level check so an untranslatable shape
    // is refused at the gate instead of sailing through to the translator.
    for (size_t i = 0; i < json_object_array_length(wins); i++) {
        json_object* w = json_object_array_get_idx(wins, i);
        if (!is_obj(w)) return "INVALID_ARGUMENT:layout.prepare window must be object";
        std::string wid;
        if (!str_field(w, "windowId", wid) || wid.empty())
            return "INVALID_ARGUMENT:layout.prepare window missing windowId";
        std::string sig;
        if (!str_field(w, "sig", sig) || sig.empty())
            return "INVALID_ARGUMENT:layout.prepare window missing sig";
        if (!is_obj(field(w, "dst")) || !is_obj(field(w, "src")))
            return "INVALID_ARGUMENT:layout.prepare window missing dst/src";
    }
    json_object* eps = field(data, "endpoints");
    if (eps && !is_arr(eps))
        return "INVALID_ARGUMENT:layout.prepare endpoints must be array";
    if (eps) {
        for (size_t i = 0; i < json_object_array_length(eps); i++) {
            json_object* e = json_object_array_get_idx(eps, i);
            if (!is_obj(e))
                return "INVALID_ARGUMENT:layout.prepare endpoint must be object";
            json_object* sip = field(e, "sourceIp");
            if (sip) {
                if (!json_object_is_type(sip, json_type_string) ||
                    !valid_ip4_literal(json_object_get_string(sip)))
                    return "INVALID_ARGUMENT:layout.prepare endpoint sourceIp must be IPv4";
            }
        }
    }
    json_object* geo = field(data, "geometry");
    if (!rect_ok(geo)) return "INVALID_ARGUMENT:layout.prepare geometry must be {x,y,w,h}";
    // Base image descriptor (optional, additive): wall-level still plus the
    // node-local sub-rectangle to paint. The node fetches the bytes itself;
    // the payload only names them.
    json_object* bi = field(data, "baseImage");
    if (bi) {
        if (!is_obj(bi))
            return "INVALID_ARGUMENT:layout.prepare baseImage must be object";
        json_object* brev = field(bi, "revision");
        if (!brev || !json_object_is_type(brev, json_type_int) ||
            json_object_get_int(brev) < 0)
            return "INVALID_ARGUMENT:layout.prepare baseImage revision must be int >= 0";
        std::string url;
        if (!str_field(bi, "url", url) || url.empty() || url.size() > 1024)
            return "INVALID_ARGUMENT:layout.prepare baseImage url required";
        json_object* sub = field(bi, "sub");
        if (sub && !rect_ok(sub))
            return "INVALID_ARGUMENT:layout.prepare baseImage sub must be {x,y,w,h}";
    }
    // Image adjustment targets (optional, additive): the node applies what
    // its display backend supports and reports the rest as unsupported.
    json_object* ia = field(data, "imageAdjust");
    if (ia) {
        if (!is_obj(ia))
            return "INVALID_ARGUMENT:layout.prepare imageAdjust must be object";
        static const char* kKeys[] = {"brightness", "contrast", "saturation",
                                      "hue"};
        for (auto k : kKeys) {
            json_object* kv = field(ia, k);
            if (kv) {
                if (!json_object_is_type(kv, json_type_int))
                    return std::string("INVALID_ARGUMENT:layout.prepare imageAdjust.") +
                           k + " must be int";
                int n = json_object_get_int(kv);
                if (n < -100 || n > 100)
                    return std::string("INVALID_ARGUMENT:layout.prepare imageAdjust.") +
                           k + " must be -100..100";
            }
        }
    }
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
    // Frame-anchored commit report (optional, additive): the node echoes the
    // frame it actually committed plus when, so the platform can verify the
    // same-frame invariant instead of trusting bare status strings.
    json_object* fi = field(data, "frameIndex");
    if (fi) {
        if (!json_object_is_type(fi, json_type_int) || json_object_get_int64(fi) < 0)
            return "INVALID_ARGUMENT:barrier.ack frameIndex must be int >= 0";
    }
    json_object* aat = field(data, "appliedAtTaiNs");
    if (aat) {
        if (!json_object_is_type(aat, json_type_int) || json_object_get_int64(aat) < 0)
            return "INVALID_ARGUMENT:barrier.ack appliedAtTaiNs must be int >= 0";
    }
    json_object* lh = field(data, "layoutHash");
    if (lh && (!json_object_is_type(lh, json_type_string) ||
                json_object_get_string(lh)[0] == '\0'))
        return "INVALID_ARGUMENT:barrier.ack layoutHash must be non-empty string";
    json_object* late = field(data, "late");
    if (late && !json_object_is_type(late, json_type_boolean) &&
        !json_object_is_type(late, json_type_int))
        return "INVALID_ARGUMENT:barrier.ack late must be boolean|int";
    json_object* reason = field(data, "reason");
    if (reason && (!json_object_is_type(reason, json_type_string) ||
                    json_object_get_string(reason)[0] == '\0'))
        return "INVALID_ARGUMENT:barrier.ack reason must be non-empty string";
    return "";
}

int64_t frame_index_for_apply_at(uint64_t epoch_ns, double fps,
                                  uint64_t apply_at_ns) {
    if (!(fps > 0.0) || apply_at_ns <= epoch_ns) return 0;
    double elapsed_s = (double)(apply_at_ns - epoch_ns) / 1000000000.0;
    double frame = elapsed_s * fps;
    if (!(frame >= 0.0)) return 0;
    if (frame > 9007199254740991.0) return 9007199254740991LL;
    return (int64_t)frame;
}

uint64_t frame_start_ns(uint64_t epoch_ns, double fps, int64_t frame) {
    if (!(fps > 0.0) || frame <= 0) return epoch_ns;
    double start_s = (double)frame / fps;
    if (!(start_s >= 0.0) || start_s > 18446744073.0) return epoch_ns;
    // Round (not truncate): a truncated start can land a nanosecond before
    // the true boundary and map back to frame-1, breaking the round-trip
    // the barrier relies on for the same-frame check.
    return epoch_ns + (uint64_t)(start_s * 1000000000.0 + 0.5);
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
