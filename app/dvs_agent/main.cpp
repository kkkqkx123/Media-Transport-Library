#include <json-c/json.h>
#include <sqlite3.h>

#include "node_store.h"
#include "media_ipc.h"
#include "network_apply.h"
#include "tx_config.h"
#include "dvs_contract.h"
#include "dvs_node_bundle.h"
#include "http_server.h"
#include "maintenance.h"
#include "registration.h"
#include "http_client.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>

static NodeStore* g_store = nullptr;
/* Command channel: layout and config mutations plus their async results. */
static MediaIpcClient* g_media = nullptr;
/* Query channel: status and capability reads share nothing with commands so
 * a slow query never holds up a layout verb. Both dial the same socket. */
static MediaIpcClient* g_media_q = nullptr;
static Maintenance* g_maint = nullptr;
static Registration* g_reg = nullptr;
static std::string g_web_root;
// Shared control-plane bearer token. Empty means no check; when set,
// platform-to-agent calls must carry it in an Authorization header.
static std::string g_api_token =
    (std::getenv("DVS_API_TOKEN") ? std::getenv("DVS_API_TOKEN") : "");

/* Per-source login rate limiter: 5 attempts / 60s window, then locked out. */
struct LoginLimiter {
    std::mutex mu;
    std::map<std::string, std::pair<int, time_t>> attempts;  // key -> (count, windowStart)
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        time_t now = time(nullptr);
        auto& e = attempts[key];
        if (now - e.second > 60) { e.first = 0; e.second = now; }
        if (e.first >= 5) return false;
        e.first++;
        return true;
    }
    void clear(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        attempts.erase(key);
    }
};
static LoginLimiter g_limiter;

/* Issued browser sessions with expiry. Random hex tokens replace the fixed
 * prototype value; logout invalidates. All access under the internal mutex. */
struct SessionStore {
    std::mutex mu;
    std::map<std::string, time_t> exp_;
    std::string issue() {
        uint8_t rnd[16] = {0};
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            size_t got = 0;
            while (got < sizeof(rnd)) {
                ssize_t n = read(fd, rnd + got, sizeof(rnd) - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
            close(fd);
        }
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        std::string tok(hex, 32);
        std::lock_guard<std::mutex> lk(mu);
        exp_[tok] = time(nullptr) + 3600;
        return tok;
    }
    bool valid(const std::string& tok) {
        if (tok.empty()) return false;
        std::lock_guard<std::mutex> lk(mu);
        auto it = exp_.find(tok);
        if (it == exp_.end()) return false;
        if (time(nullptr) > it->second) { exp_.erase(it); return false; }
        return true;
    }
    void invalidate(const std::string& tok) {
        std::lock_guard<std::mutex> lk(mu);
        exp_.erase(tok);
    }
};
static SessionStore g_sessions;

static std::string session_from_cookie(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("Cookie");
    if (it == headers.end()) return "";
    const std::string& c = it->second;
    size_t p = c.find("session=");
    if (p == std::string::npos) return "";
    size_t s = p + 8;
    size_t e = c.find(';', s);
    std::string v = c.substr(s, e == std::string::npos ? std::string::npos : e - s);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
}

static bool valid_ip4(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '.') {
            if (i == start || i - start > 3) return false;
            for (size_t j = start; j < i; j++) if (s[j] < '0' || s[j] > '9') return false;
            int v = atoi(s.substr(start, i - start).c_str());
            if (v < 0 || v > 255) return false;
            parts++;
            start = i + 1;
        }
    }
    return parts == 4;
}

static std::string trim_str(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) b--;
    return s.substr(a, b - a);
}

/* Single whitelist item: "ip" or "ip:port". */
static bool parse_endpoint_item(const std::string& raw, std::string& ip_out,
                                int& port_out, bool& has_port_out) {
    std::string t = trim_str(raw);
    if (t.empty()) return false;
    size_t colon = t.find(':');
    if (colon == std::string::npos) {
        if (!valid_ip4(t)) return false;
        ip_out = t; port_out = 0; has_port_out = false;
        return true;
    }
    if (t.find(':', colon + 1) != std::string::npos) return false;
    std::string ip = trim_str(t.substr(0, colon));
    std::string ps = trim_str(t.substr(colon + 1));
    if (!valid_ip4(ip) || ps.empty() || ps.size() > 5) return false;
    for (char c : ps) if (c < '0' || c > '9') return false;
    int p = atoi(ps.c_str());
    if (p < 1 || p > 65535) return false;
    ip_out = ip; port_out = p; has_port_out = true;
    return true;
}

/* Strict per-item whitelist validation. Accepts comma string, array of
 * string/object, or {whitelist|sources: ...} nesting. */
static bool validate_whitelist_value(json_object* wv, std::string& bad_item) {
    if (!wv) return true;
    if (json_object_is_type(wv, json_type_string)) {
        std::string s = json_object_get_string(wv);
        bool any = false;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ',') {
                std::string tok = trim_str(s.substr(start, i - start));
                start = i + 1;
                if (tok.empty()) continue;
                any = true;
                std::string ip; int port = 0; bool hp = false;
                if (!parse_endpoint_item(tok, ip, port, hp)) { bad_item = tok; return false; }
            }
        }
        if (!any) { bad_item = s; return false; }
        return true;
    }
    if (json_object_is_type(wv, json_type_array)) {
        int n = json_object_array_length(wv);
        if (n <= 0) { bad_item = "empty whitelist"; return false; }
        for (int i = 0; i < n; i++) {
            json_object* e = json_object_array_get_idx(wv, i);
            if (!e) { bad_item = "null entry"; return false; }
            if (json_object_is_type(e, json_type_string)) {
                std::string s = json_object_get_string(e);
                /* Allow comma string inside an array element. */
                size_t start = 0;
                bool any = false;
                for (size_t k = 0; k <= s.size(); k++) {
                    if (k == s.size() || s[k] == ',') {
                        std::string tok = trim_str(s.substr(start, k - start));
                        start = k + 1;
                        if (tok.empty()) continue;
                        any = true;
                        std::string ip; int port = 0; bool hp = false;
                        if (!parse_endpoint_item(tok, ip, port, hp)) { bad_item = tok; return false; }
                    }
                }
                if (!any) { bad_item = s; return false; }
            } else if (json_object_is_type(e, json_type_object)) {
                json_object* iv = nullptr;
                std::string ip;
                if (json_object_object_get_ex(e, "sip", &iv) && iv) ip = json_object_get_string(iv);
                else if (json_object_object_get_ex(e, "ip", &iv) && iv) ip = json_object_get_string(iv);
                else if (json_object_object_get_ex(e, "source", &iv) && iv) ip = json_object_get_string(iv);
                ip = trim_str(ip);
                json_object* pv = nullptr;
                bool has_p = false; int p = 0;
                if ((json_object_object_get_ex(e, "udp_port", &pv) ||
                     json_object_object_get_ex(e, "udpPort", &pv) ||
                     json_object_object_get_ex(e, "port", &pv)) && pv) {
                    /* json-c ints come back as int; strings are also accepted. */
                    if (json_object_is_type(pv, json_type_string)) {
                        std::string ps = trim_str(json_object_get_string(pv));
                        if (ps.empty()) return false;
                        for (char c : ps) if (c < '0' || c > '9') { bad_item = ip + ":" + ps; return false; }
                        p = atoi(ps.c_str());
                    } else {
                        p = json_object_get_int(pv);
                    }
                    has_p = true;
                }
                if (!valid_ip4(ip)) { bad_item = ip.empty() ? "missing ip" : ip; return false; }
                if (has_p && (p < 1 || p > 65535)) { bad_item = ip + ":" + std::to_string(p); return false; }
            } else { bad_item = "bad entry type"; return false; }
        }
        return true;
    }
    if (json_object_is_type(wv, json_type_object)) {
        json_object* inner = nullptr;
        if (json_object_object_get_ex(wv, "whitelist", &inner) && inner)
            return validate_whitelist_value(inner, bad_item);
        if (json_object_object_get_ex(wv, "sources", &inner) && inner)
            return validate_whitelist_value(inner, bad_item);
        bad_item = "bad whitelist object";
        return false;
    }
    bad_item = "bad whitelist value";
    return false;
}

static std::string iso_now() {
    char buf[64];
    time_t t = time(nullptr);
    struct tm* tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(buf);
}

/* System network apply is systemd-networkd single-style (see network_apply).
 * Without the full execution gate (env + confirm + operationId) the call
 * persists the intent for audit and returns success; applied in the config
 * state machine therefore means the intent was stored, not that the host
 * network was reconfigured. DVS_FORCE_NETWORK_FAIL=1 forces failure for
 * rollback testing. */
static bool intent_log_network(const std::string& intent, std::string& err) {
    if (std::getenv("DVS_FORCE_NETWORK_FAIL") &&
        std::string(std::getenv("DVS_FORCE_NETWORK_FAIL")) == "1") {
        err = "injected failure (DVS_FORCE_NETWORK_FAIL=1)";
        return false;
    }
    fprintf(stderr, "dvs-agent: network intent stored (system untouched): %s\n",
            intent.c_str());
    return true;
}

static long long agent_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* Continuous version gate. Warn-only at startup (the engine may start
 * later), but layout-affecting verbs are refused while the reachable engine
 * reports a different bundle contract. Queries and abort stay open so
 * observability and the two-phase escape hatch keep working. */
static std::mutex g_ver_mu;
static bool g_ver_known = false;
static bool g_ver_match = false;
static long long g_ver_at = 0;
static const long long kVerCacheMs = 5000;

static bool media_layout_allowed(std::string& reason_out) {
    reason_out.clear();
    if (!g_media_q) return true;
    {
        std::lock_guard<std::mutex> lk(g_ver_mu);
        if (g_ver_known && agent_now_ms() - g_ver_at < kVerCacheMs) {
            if (!g_ver_match)
                reason_out = "VERSION_MISMATCH: media engine bundle differs";
            return g_ver_match;
        }
    }
    bool answered = false, match = false;
    std::string eb;
    int ei = -1;
    MediaIpcClient::parse_hello(g_media_q->hello(), answered, match, eb, ei);
    std::lock_guard<std::mutex> lk(g_ver_mu);
    g_ver_at = agent_now_ms();
    if (!answered) {
        /* Engine silent: unknown, not mismatched. Let the verb itself
         * report the transport error instead of masking it here. */
        return true;
    }
    g_ver_known = true;
    g_ver_match = match;
    if (!match) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "VERSION_MISMATCH: engine bundle %s ipc %d, want %s/%d",
                 eb.empty() ? "?" : eb.c_str(), ei, DVS_NODE_BUNDLE_VERSION,
                 DVS_MEDIA_IPC_VERSION);
        reason_out = buf;
        fprintf(stderr, "dvs-agent: %s\n", buf);
    }
    return match;
}

static std::string gated_envelope(const std::string& reason) {
    std::string code = "OK";
    std::string msg = reason;
    size_t colon = reason.find(':');
    if (colon != std::string::npos) {
        code = reason.substr(0, colon);
        msg = reason.substr(colon + 1);
        while (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
    }
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(409));
    json_object_object_add(o, "code", json_object_new_string(code.c_str()));
    json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
    json_object_object_add(o, "data", json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("gate"));
    const char* s = json_object_to_json_string(o);
    std::string out(s ? s : "{}");
    json_object_put(o);
    return out;
}

/* Picture-affecting verbs go through the two-phase path; a configure body
 * carrying layout keys is a caller-side model error, refused locally. */
static bool configure_has_layout_keys(json_object* payload) {
    if (!payload || !json_object_is_type(payload, json_type_object)) return false;
    json_object* t = nullptr;
    return (json_object_object_get_ex(payload, "streams", &t) && t) ||
           (json_object_object_get_ex(payload, "windows", &t) && t) ||
           (json_object_object_get_ex(payload, "layout", &t) && t);
}

/* Mutating verbs on the command channel, guarded by the version gate.
 * Abort is the escape hatch and bypasses the gate (call g_media directly). */
static std::string media_cmd(const std::string& type, json_object* payload,
                             int timeout_ms = 2000) {
    if (!g_media) return gated_envelope("MEDIA_ENGINE_UNCONFIGURED: no IPC client");
    if (type == "prepare" || type == "commit" || type == "configure" ||
        type == "clear" || type == "tx_start" || type == "tx_stop" ||
        type == "set_sources") {
        std::string reason;
        if (!media_layout_allowed(reason)) return gated_envelope(reason);
        if (type == "configure" && configure_has_layout_keys(payload))
            return gated_envelope(
                "INVALID_ARGUMENT: picture changes must use prepare/commit");
    }
    return g_media->request(type, payload, timeout_ms);
}

/* Translate a platform sub-layout (unified wire shape from
 * DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md §4: flat windows with
 * windowId/sig/dst/src/z/muted/partial, an endpoint table, audio owner and
 * scene volume) into the engine "streams" shape. Returns false on a
 * whole-order rejection (signal not in the endpoint table, a cross-node
 * window, or over the lane cap); on success *prep_out holds the prepare
 * payload with lanes assigned by z ascending, ties in array order.
 * Defensive clipping drops windows fully outside the node canvas. */
static bool translate_platform_layout(json_object* body, json_object** prep_out,
                                      std::string& err_out) {
    *prep_out = nullptr;
    if (!body || !json_object_is_type(body, json_type_object)) {
        err_out = "INVALID_ARGUMENT:empty body";
        return false;
    }
    json_object* wins = nullptr;
    json_object_object_get_ex(body, "windows", &wins);
    int wn = (wins && json_object_is_type(wins, json_type_array))
                 ? (int)json_object_array_length(wins) : 0;
    json_object* geo = nullptr;
    json_object_object_get_ex(body, "geometry", &geo);
    int canvas_w = 0, canvas_h = 0;
    if (geo) {
        json_object* v = nullptr;
        if (json_object_object_get_ex(geo, "w", &v) && v) canvas_w = json_object_get_int(v);
        if (json_object_object_get_ex(geo, "h", &v) && v) canvas_h = json_object_get_int(v);
    }
    /* Endpoint lookup: sig -> (address, port). A signal without a table
     * entry leaves the agent no way to name its stream — reject the order. */
    std::map<std::string, std::pair<std::string, int>> endpoints;
    json_object* eps = nullptr;
    if (json_object_object_get_ex(body, "endpoints", &eps) &&
        json_object_is_type(eps, json_type_array)) {
        int en = (int)json_object_array_length(eps);
        for (int i = 0; i < en; i++) {
            json_object* e = json_object_array_get_idx(eps, i);
            if (!e || !json_object_is_type(e, json_type_object)) continue;
            json_object* v = nullptr;
            std::string sig, addr;
            int port = 0;
            if (json_object_object_get_ex(e, "sig", &v) && v)
                sig = json_object_get_string(v);
            if (json_object_object_get_ex(e, "address", &v) && v)
                addr = json_object_get_string(v);
            if (json_object_object_get_ex(e, "port", &v) && v)
                port = json_object_get_int(v);
            if (!sig.empty() && !addr.empty() && port > 0)
                endpoints[sig] = {addr, port};
        }
    }
    if (wn > 8) {  // MP4_PIP_MAX_LANES (display_layout.h)
        err_out = "INVALID_ARGUMENT:too many windows for engine lanes";
        return false;
    }
    /* Stable z-ascending order: same layer order falls back to array order,
     * so channel assignment is deterministic (设计 §4 准入与排序). */
    std::vector<int> order((size_t)wn);
    for (int i = 0; i < wn; i++) order[(size_t)i] = i;
    auto z_of = [&](int i) -> int {
        json_object* w = json_object_array_get_idx(wins, i);
        json_object* v = nullptr;
        return (w && json_object_object_get_ex(w, "z", &v) && v)
                   ? json_object_get_int(v) : 0;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return z_of(a) < z_of(b); });

    json_object* streams = json_object_new_array();
    for (int idx : order) {
        json_object* w = json_object_array_get_idx(wins, idx);
        if (!w || !json_object_is_type(w, json_type_object)) continue;
        json_object* v = nullptr;
        std::string sig, win_id;
        if (json_object_object_get_ex(w, "sig", &v) && v) sig = json_object_get_string(v);
        if (json_object_object_get_ex(w, "windowId", &v) && v) win_id = json_object_get_string(v);
        auto it = endpoints.find(sig);
        if (it == endpoints.end()) {
            json_object_put(streams);
            err_out = "INVALID_ARGUMENT:signal '" + sig + "' not in endpoint table";
            return false;
        }
        json_object* pv = nullptr;
        bool partial = false;
        if (json_object_object_get_ex(w, "partial", &pv) && pv)
            partial = json_object_get_boolean(pv);
        if (partial) {
            json_object_put(streams);
            /* 退化方案 (设计 §4 源裁剪分两步走): the compositor cannot crop
             * a source today, so a cross-node window would be stretched to
             * the full source and paint the wrong picture — reject it. */
            err_out = "CROSS_NODE_WINDOW:window '" + win_id +
                      "' spans multiple nodes; compositor cannot crop yet";
            return false;
        }
        int dx = 0, dy = 0, dw = 0, dh = 0;
        json_object* dst = nullptr;
        if (json_object_object_get_ex(w, "dst", &dst) && dst) {
            if (json_object_object_get_ex(dst, "x", &v) && v) dx = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "y", &v) && v) dy = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "w", &v) && v) dw = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "h", &v) && v) dh = json_object_get_int(v);
        }
        if (dw <= 0 || dh <= 0) continue;  // degenerate: silently dropped
        /* Defensive clip against the node canvas (代理保留防御性裁剪). */
        int cx1 = std::max(0, dx), cy1 = std::max(0, dy);
        int cx2 = std::min(canvas_w, dx + dw), cy2 = std::min(canvas_h, dy + dh);
        if (cx2 <= cx1 || cy2 <= cy1) {
            fprintf(stderr, "dvs_agent: window '%s' fully outside canvas, dropped\n",
                    win_id.c_str());
            continue;
        }
        int sw = dw, sh = dh;
        json_object* src = nullptr;
        if (json_object_object_get_ex(w, "src", &src) && src) {
            if (json_object_object_get_ex(src, "w", &v) && v) sw = json_object_get_int(v);
            if (json_object_object_get_ex(src, "h", &v) && v) sh = json_object_get_int(v);
        }
        int lane = (int)json_object_array_length(streams);
        json_object* s = json_object_new_object();
        json_object_object_add(s, "lane", json_object_new_int(lane));
        json_object_object_add(s, "main", json_object_new_boolean(lane == 0));
        json_object_object_add(s, "sip", json_object_new_string(it->second.first.c_str()));
        json_object_object_add(s, "udpPort", json_object_new_int(it->second.second));
        json_object_object_add(s, "width", json_object_new_int(sw));
        json_object_object_add(s, "height", json_object_new_int(sh));
        json_object_object_add(s, "x", json_object_new_int(cx1));
        json_object_object_add(s, "y", json_object_new_int(cy1));
        json_object_object_add(s, "w", json_object_new_int(cx2 - cx1));
        json_object_object_add(s, "h", json_object_new_int(cy2 - cy1));
        json_object_array_add(streams, s);
    }
    json_object* prep = json_object_new_object();
    json_object* opv = nullptr;
    if (json_object_object_get_ex(body, "operationId", &opv) && opv)
        json_object_object_add(prep, "operationId", json_object_get(opv));
    json_object* rv = nullptr;
    if (json_object_object_get_ex(body, "revision", &rv) && rv)
        json_object_object_add(prep, "revision", json_object_get(rv));
    json_object* av = nullptr;
    if (json_object_object_get_ex(body, "applyAtNs", &av) && av)
        json_object_object_add(prep, "applyAtNs", json_object_get(av));
    /* Empty array = explicit clear (keep backdrop); it must still be staged
     * so the commit applies an empty layout instead of keeping the picture. */
    json_object_object_add(prep, "streams", streams);
    json_object* aov = nullptr;
    if (json_object_object_get_ex(body, "audioOwner", &aov) && aov)
        json_object_object_add(prep, "audioOwner", json_object_get(aov));
    json_object* vol = nullptr;
    if (json_object_object_get_ex(body, "volume", &vol) && vol)
        json_object_object_add(prep, "audioVolume", json_object_get(vol));
    *prep_out = prep;
    return true;
}

/* Read-only verbs on the query channel; never gated. */
static std::string media_query(const std::string& type, json_object* payload,
                               int timeout_ms = 2000) {
    if (!g_media_q) return gated_envelope("MEDIA_ENGINE_UNCONFIGURED: no IPC client");
    return g_media_q->request(type, payload, timeout_ms);
}

/* Single shared判定 for config outcomes: a stale answer (a newer save
 * landed while the engine worked) is dropped, never overwriting fresh
 * state. Both the synchronous input path and the background output path
 * report through here. */
static int config_desired_revision(const std::string& scope,
                                   const std::string& node_id) {
    int want_rev = 0;
    json_object* cur = g_store->get_config(scope, node_id);
    json_object* drv = nullptr;
    if (cur && json_object_object_get_ex(cur, "desiredRevision", &drv) && drv)
        want_rev = json_object_get_int(drv);
    if (cur) json_object_put(cur);
    return want_rev;
}

static void apply_media_config_outcome(const std::string& scope,
                                       const std::string& node_id, int want_rev,
                                       bool ok, const std::string& code,
                                       const std::string& msg) {
    int cur_rev = config_desired_revision(scope, node_id);
    if (cur_rev != want_rev) {
        fprintf(stderr,
                "dvs-agent: media configure (%s) stale rev %d vs %d, skip\n",
                scope.c_str(), want_rev, cur_rev);
        return;
    }
    if (!ok) {
        g_store->mark_failed(scope, code.empty() ? "MEDIA_CONFIGURE_FAILED" : code.c_str(),
                             msg.empty() ? "media engine refused config" : msg.c_str());
        fprintf(stderr, "dvs-agent: media configure (%s) failed code=%s msg=%s\n",
                scope.c_str(), code.c_str(), msg.c_str());
    } else {
        g_store->mark_applied(scope, 0);
        fprintf(stderr, "dvs-agent: media configure (%s) ok\n", scope.c_str());
    }
}

/* Engine link derived from the query channel counters, no extra probes.
 * Transitions are audit-logged; the shape stays local to /version so the
 * platform heartbeat contract never shifts under this change. */
static std::string g_link_logged = "unknown";
static std::string engine_link_state() {
    if (!g_media_q) return "unknown";
    int streak = g_media_q->fail_streak();
    long long ok_age = -1;
    if (g_media_q->last_ok_ms() > 0) ok_age = agent_now_ms() - g_media_q->last_ok_ms();
    std::string st;
    if (streak >= 3) st = "offline";
    else if (streak > 0) st = "degraded";
    else if (ok_age >= 0) st = "online";
    else st = "unknown";
    if (st != g_link_logged && (st == "offline" || g_link_logged == "offline")) {
        fprintf(stderr, "dvs-agent: engine link %s -> %s (streak=%d)\n",
                g_link_logged.c_str(), st.c_str(), streak);
        g_link_logged = st;
    } else if (g_link_logged == "unknown" && st != "unknown") {
        g_link_logged = st;
    }
    return st;
}

static long long g_cap_ok_ms = 0;
static const long long kCapStaleMs = 30000;

/* Pull engine capability once and merge into node.capabilities. Engine
 * unreachable keeps the stored value; an engine that clearly reports an
 * empty port array propagates the empty array so the frontend can show the
 * honest empty state instead of a synthesized port. Log-only. */
static void refresh_capability_from_media() {
    if (!g_media_q || !g_store) return;
    std::string cr = g_media_q->request("capability", nullptr);
    json_object* co = json_tokener_parse(cr.c_str());
    if (!co) { fprintf(stderr, "dvs-agent: startup capability pull failed (unparsable)\n"); return; }
    json_object* okv = nullptr;
    json_object* data = nullptr;
    if (json_object_object_get_ex(co, "ok", &okv) && okv && json_object_get_boolean(okv) &&
        json_object_object_get_ex(co, "data", &data) && data) {
        json_object* patch = json_object_new_object();
        json_object* caps = nullptr;
        json_object_object_get_ex(g_store->node(), "capabilities", &caps);
        json_object* merged = caps ? json_object_get(caps) : json_object_new_object();
        json_object_object_foreach(data, k, v) {
            json_object_object_add(merged, k, json_object_get(v));
        }
        json_object_object_add(patch, "capabilities", merged);
        g_store->update_node(patch);
        json_object_put(patch);
        g_cap_ok_ms = agent_now_ms();
        fprintf(stderr, "dvs-agent: startup capability merged\n");
    } else {
        fprintf(stderr, "dvs-agent: startup capability pull returned error\n");
    }
    json_object_put(co);
}

static bool tcp_reachable(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo* ai = nullptr;
    if (getaddrinfo(host.c_str(), ps, &hints, &ai) != 0) return false;
    bool ok = false;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) { ok = true; close(fd); break; }
        if (errno == EINPROGRESS) {
            struct pollfd pfd{.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t n = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && err == 0) ok = true;
            }
        }
        close(fd);
        if (ok) break;
    }
    freeaddrinfo(ai);
    return ok;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string guess_ct(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.find(".css") != std::string::npos) return "text/css; charset=utf-8";
    if (path.find(".js") != std::string::npos) return "text/javascript; charset=utf-8";
    if (path.find(".json") != std::string::npos) return "application/json; charset=utf-8";
    if (path.find(".svg") != std::string::npos) return "image/svg+xml";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}

static std::string env_or(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d ? d : "");
}

/* Node capacity self-report (S5): CPU busy ratio (since boot), memory usage,
 * load average and uptime, read from /proc. No external deps. */
static json_object* node_capacity_json() {
    json_object* o = json_object_new_object();

    /* memory from /proc/meminfo */
    long long mem_total = 0, mem_avail = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            char key[64]; long long val = 0;
            if (sscanf(line.c_str(), "%63[^:]: %lld", key, &val) == 2) {
                if (strcmp(key, "MemTotal") == 0) mem_total = val;
                else if (strcmp(key, "MemAvailable") == 0) mem_avail = val;
            }
        }
    }

    /* load average + uptime */
    double l1 = 0, l5 = 0, l15 = 0, uptime = 0;
    {
        std::ifstream f("/proc/loadavg");
        std::string line;
        if (std::getline(f, line)) sscanf(line.c_str(), "%lf %lf %lf", &l1, &l5, &l15);
    }
    {
        std::ifstream f("/proc/uptime");
        double idle = 0;
        if (f >> uptime >> idle) { /* uptime is the first field */ }
    }

    /* cumulative CPU busy ratio (since boot) from /proc/stat cpu line */
    double cpu_pct = 0.0;
    {
        std::ifstream f("/proc/stat");
        std::string line;
        if (std::getline(f, line) && line.rfind("cpu ", 0) == 0) {
            long long u=0,n=0,s=0,idle=0,iow=0,irq=0,sirq=0,steal=0;
            sscanf(line.c_str() + 4, "%lld %lld %lld %lld %lld %lld %lld %lld",
                   &u, &n, &s, &idle, &iow, &irq, &sirq, &steal);
            long long idle_all = idle + iow;
            long long total = u + n + s + idle + iow + irq + sirq + steal;
            if (total > 0) cpu_pct = 100.0 * (double)(total - idle_all) / (double)total;
        }
    }

    json_object_object_add(o, "cpuBusyPercent", json_object_new_double(cpu_pct));
    json_object_object_add(o, "memTotalKB", json_object_new_int64(mem_total));
    json_object_object_add(o, "memAvailableKB", json_object_new_int64(mem_avail));
    int mem_used_pct = mem_total > 0
        ? (int)(100.0 * (double)(mem_total - mem_avail) / (double)mem_total) : 0;
    json_object_object_add(o, "memUsedPercent", json_object_new_int(mem_used_pct));
    json_object_object_add(o, "load1", json_object_new_double(l1));
    json_object_object_add(o, "load5", json_object_new_double(l5));
    json_object_object_add(o, "load15", json_object_new_double(l15));
    json_object_object_add(o, "uptimeSec", json_object_new_int64((long long)uptime));
    return o;
}

static json_object* envelope_ok(json_object* data, const char* msg) {
    return dvs::contract::make_ok(data, msg ? msg : "", "a");
}

static json_object* envelope_fail(const char* code, int status, const char* msg) {
    return dvs::contract::make_fail(code, status, msg ? msg : code, "a");
}

static void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

static std::string get_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return "";
}

/* Background reporter. After the agent forwards a platform sub-layout to the
 * media engine (prepare IPC), the engine emits a prepare-result event once
 * it has readied its RX sessions. This thread waits for the matching
 * operation on the command channel, then closes the local two-phase
 * operation (commit on ready, abort otherwise) before uplinking so the
 * barrier can collect this node's reply. Runs detached; never blocks the
 * HTTP handler. Only ready/blocked leave this node; anything else
 * (including timeouts) is reported as blocked. */
static void agent_report_commit(const std::string& opid, int rev,
                                int64_t applyAtNs, const std::string& nodeId,
                                const std::string& platform_url) {
    (void)applyAtNs;  // reserved for barrier diagnostics
    std::string status = "blocked";
    std::string lanes_json = "[]";
    bool done = false;
    if (g_media) {
        std::string ev_status, ev_lanes;
        /* Blocking wait with early match: no fixed sleep grid, and the
         * client lock is released while polling so other verbs interleave. */
        if (g_media->wait_prepare_result(opid, 5000, ev_status, ev_lanes)) {
            status = ev_status;
            lanes_json = ev_lanes;
            done = true;
        }
    }
    /* fallback: if the event was missed (e.g. drained elsewhere), probe state */
    if (!done) {
        std::string qs = media_query("query_state", nullptr);
        json_object* st = json_tokener_parse(qs.c_str());
        if (st) {
            json_object* d = nullptr;
            if (json_object_object_get_ex(st, "data", &d) && d) {
                std::string s = get_str(d, "state");
                status = (s == "prepared" || s == "idle") ? "ready" : "blocked";
                json_object* lj = nullptr;
                if (json_object_object_get_ex(d, "lanes", &lj) && lj) {
                    const char* ls = json_object_to_json_string(lj);
                    if (ls) lanes_json = ls;
                }
            } else {
                status = "blocked";
            }
            json_object_put(st);
        } else {
            status = "blocked";
        }
    }
    json_object* lanes = nullptr;
    if (lanes_json != "[]") lanes = json_tokener_parse(lanes_json.c_str());
    /* Close the local two-phase operation so the media engine returns to
     * idle; otherwise the next prepare is rejected as "another operation
     * in progress". Commit on ready, abort otherwise, before uplinking.
     * Abort always bypasses the version gate: it is the escape hatch. */
    {
        json_object* term = json_object_new_object();
        json_object_object_add(term, "operationId", json_object_new_string(opid.c_str()));
        json_object_object_add(term, "revision", json_object_new_int(rev));
        std::string tr = (status == "ready") ? media_cmd("commit", term)
                                               : g_media->request("abort", term);
        json_object_put(term);
        json_object* to = json_tokener_parse(tr.c_str());
        bool ok = false;
        if (to) {
            json_object* okv = nullptr;
            if (json_object_object_get_ex(to, "ok", &okv) && okv)
                ok = json_object_get_boolean(okv) != 0;
            json_object_put(to);
        }
        if (!ok && status == "ready") {
            fprintf(stderr, "dvs-agent: media commit (%s) refused, downgrade to blocked\n",
                    opid.c_str());
            status = "blocked";
        }
    }
    if (platform_url.empty()) {  // local-autonomous: local 2PC done, nothing to uplink
        if (lanes) json_object_put(lanes);
        return;
    }
    // 单次投递语义：上行 helper 内已含 WS 优先加短连接回退，此处不再二次补发。
    if (g_reg) g_reg->uplink_barrier_ack(opid, nodeId, status, lanes);
    if (lanes) json_object_put(lanes);
}

int main(int argc, char** argv) {
    std::string db_path = "/tmp/dvs_agent.db";
    std::string platform_url;
    std::string media_sock;
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version") {
            printf("dvs-agent bundle %s ipc %d\n", DVS_NODE_BUNDLE_VERSION,
                   DVS_MEDIA_IPC_VERSION);
            return 0;
        }
        if (a == "--db" && i + 1 < argc) db_path = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--platform-url" && i + 1 < argc) platform_url = argv[++i];
        else if (a == "--media-sock" && i + 1 < argc) media_sock = argv[++i];
        else if (a == "--web-root" && i + 1 < argc) g_web_root = argv[++i];
    }
    if (g_web_root.empty()) g_web_root = env_or("DVS_WEB_ROOT", "./web/src");
    if (media_sock.empty()) media_sock = env_or("DVS_MEDIA_SOCK", "");
    if (platform_url.empty()) platform_url = env_or("DVS_PLATFORM_URL", "");

    g_store = new NodeStore(db_path);
    g_media = new MediaIpcClient(media_sock);
    g_media_q = new MediaIpcClient(media_sock);
    g_maint = new Maintenance(g_store, g_media);

    // Bundle handshake: confirm the local media engine is a co-released
    // peer. Warn-only; the engine may simply start later than the agent.
    // The verdict seeds the continuous version gate below.
    {
        bool answered = false, match = false;
        std::string eb;
        int ei = -1;
        MediaIpcClient::parse_hello(media_query("hello", nullptr), answered, match, eb, ei);
        {
            std::lock_guard<std::mutex> lk(g_ver_mu);
            g_ver_at = agent_now_ms();
            if (answered) {
                g_ver_known = true;
                g_ver_match = match;
            }
        }
        if (match)
            printf("dvs-agent: media engine bundle %s ipc %d confirmed\n",
                   DVS_NODE_BUNDLE_VERSION, DVS_MEDIA_IPC_VERSION);
        else
            printf("dvs-agent: WARNING media engine handshake mismatch "
                   "(see /api/v1/node/version)\n");
        fflush(stdout);
    }

    // Derive this agent's externally reachable base URL. Operator may
    // override via --agent-base / DVS_AGENT_BASE; otherwise auto-build from the
    // node's network.ip and the listen port so the platform can fan out to it.
    std::string agent_base = env_or("DVS_AGENT_BASE", "");
    if (agent_base.empty()) {
        std::string ip = "127.0.0.1";
        json_object* nd = g_store->node();
        json_object* net = nullptr;
        if (nd && json_object_object_get_ex(nd, "network", &net) && net) {
            const char* nip = nullptr;
            json_object* n2 = nullptr;
            if (json_object_object_get_ex(net, "ip", &n2) && n2) nip = json_object_get_string(n2);
            if (nip) ip = nip;
        }
        agent_base = "http://" + ip + ":" + std::to_string(port);
    }
    g_reg = new Registration(g_store, g_media_q, platform_url, 3000, agent_base);
    // WS 下行分发：布局/配置/升级经长连接到达，与短连接同语义。
    g_reg->set_downlink_handler([&](const std::string& type, json_object* data,
                                    const std::string& msg_id) {
        (void)msg_id;
        namespace ct = dvs::contract;
        if (type == ct::kLayoutPrepare) {
            if (!ct::validate_layout_prepare(data).empty()) return;
            std::string opid = get_str(data, "operationId");
            int rev = 1;
            json_object* rv = nullptr;
            if (json_object_object_get_ex(data, "revision", &rv)) rev = json_object_get_int(rv);
            int64_t applyAtNs = 0;
            json_object* av = nullptr;
            if (json_object_object_get_ex(data, "applyAtNs", &av))
                applyAtNs = (int64_t)json_object_get_int64(av);
            if (opid.empty()) return;
            /* Translate the platform sub-layout into engine streams. A whole
             * order rejection (endpoint miss / cross-node window / lane cap)
             * skips the prepare AND the reporter: reporting would ack ready
             * off an idle engine while nothing was staged. The barrier then
             * times out and the platform rolls back — never a fake ready. */
            json_object* prep = nullptr;
            std::string terr;
            if (!translate_platform_layout(data, &prep, terr)) {
                fprintf(stderr, "dvs_agent: layout rejected (%s)\n", terr.c_str());
                return;
            }
            std::string r = media_cmd("prepare", prep);
            json_object_put(prep);
            (void)r;
            std::string nodeId = get_str(g_store->node(), "id");
            if (nodeId.empty()) nodeId = "local-node";
            std::string purl = platform_url;
            std::thread([opid, rev, applyAtNs, nodeId, purl]() {
                agent_report_commit(opid, rev, applyAtNs, nodeId, purl);
            }).detach();
        } else if (type == ct::kConfigUpdate) {
            if (!ct::validate_config_update(data).empty()) return;
            json_object* cfg = nullptr;
            json_object_object_get_ex(data, "config", &cfg);
            json_object* patch2 = json_object_new_object();
            json_object* role_val = nullptr;
            if (cfg && json_object_is_type(cfg, json_type_object)) {
                json_object_object_foreach(cfg, key, val) {
                    std::string k(key);
                    if (k == "geometry") json_object_object_add(patch2, "geometry", json_object_get(val));
                    else if (k == "role") role_val = val;
                }
            }
            json_object_object_add(patch2, "configSource", json_object_new_string("platform-ws"));
            g_store->update_node(patch2);
            json_object_put(patch2);
            if (role_val) {
                if (json_object_is_type(role_val, json_type_string)) {
                    std::string r = json_object_get_string(role_val);
                    json_object* rv2 = json_object_new_object();
                    json_object_object_add(rv2, "role", json_object_new_string(r.c_str()));
                    g_store->set_desired("role", rv2, nullptr);
                    json_object_put(rv2);
                } else {
                    g_store->set_desired("role", json_object_get(role_val), nullptr);
                }
            }
            std::string nodeId = get_str(g_store->node(), "id");
            if (nodeId.empty()) nodeId = "local-node";
            int applied = 0;
            json_object* nd = g_store->node();
            json_object* crr = nullptr;
            if (nd && json_object_object_get_ex(nd, "configRevision", &crr) && crr) {
                for (int i = 0; i < 6; i++) {
                    json_object* s = nullptr;
                    if (json_object_object_get_ex(crr, NodeStore::SCOPES[i], &s) && s) {
                        json_object* a = nullptr;
                        if (json_object_object_get_ex(s, "applied", &a) && a)
                            applied = std::max(applied, json_object_get_int(a));
                    }
                }
            }
            g_reg->uplink_config_ack(nodeId, applied);
        } else if (type == ct::kUpgradeTask) {
            if (!ct::validate_upgrade_task(data).empty()) return;
            std::string tid = get_str(data, "taskId");
            std::string fn = get_str(data, "filename");
            std::string sha = get_str(data, "sha256");
            std::string nodeId = get_str(g_store->node(), "id");
            if (nodeId.empty()) nodeId = "local-node";
            std::thread([tid, fn, sha, nodeId]() {
                g_reg->uplink_upgrade_progress(tid, nodeId, "running");
                json_object* r = g_maint->upgrade(fn, sha);
                bool okk = false;
                std::string stt = "failed";
                if (r) {
                    json_object* okv = nullptr;
                    if (json_object_object_get_ex(r, "ok", &okv) && okv)
                        okk = json_object_get_boolean(okv) != 0;
                    json_object* sv = nullptr;
                    if (json_object_object_get_ex(r, "status", &sv) && sv) stt = json_object_get_string(sv);
                    json_object_put(r);
                }
                (void)okk;
                g_reg->uplink_upgrade_progress(tid, nodeId, stt);
            }).detach();
        }
    });

    // Merge the live engine capability at startup so MTL and PTP stay fresh.
    refresh_capability_from_media();

    // sync configStates into node at startup
    json_object* cs = g_store->all_states();
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "configStates", cs);
    json_object* cr = g_store->config_revision();
    json_object_object_add(patch, "configRevision", cr);
    g_store->update_node(patch);
    json_object_put(patch);

    HttpServer::Handler handler = [&](const HttpRequest& req, HttpResponse& res) {
        // login
        if (req.method == "POST" && req.path == "/api/v1/node/login") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string u = get_str(body, "username");
            std::string p = get_str(body, "password");
            if (body) json_object_put(body);
            if (!g_limiter.allow(u)) {
                send_json(res, envelope_fail("TOO_MANY_REQUESTS", 429, "登录尝试过多,请稍后再试"));
                return;
            }
            if (g_store->verify_password(u, p)) {
                g_limiter.clear(u);
                std::string tok = g_sessions.issue();
                json_object* d = json_object_new_object();
                json_object_object_add(d, "session", json_object_new_string(tok.c_str()));
                json_object_object_add(d, "expiresIn", json_object_new_int(3600));
                res.headers["Set-Cookie"] = "session=" + tok + "; Path=/; HttpOnly";
                json_object* pc = json_object_new_object();
                json_object_object_add(pc, "passwordChangeRequired",
                                      json_object_new_boolean(g_store->password_change_required() ? 1 : 0));
                json_object_object_add(d, "requirement", pc);
                send_json(res, envelope_ok(d, "登录成功"));
            } else {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "账号或密码错误"));
            }
            return;
        }

        // password change
        if (req.method == "POST" && req.path == "/api/v1/node/password") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string user = get_str(body, "username");
            if (user.empty()) user = "admin";  // default account
            std::string oldp = get_str(body, "oldPassword");
            std::string newp = get_str(body, "newPassword");
            if (body) json_object_put(body);
            if (!g_store->verify_password(user, oldp)) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "原密码错误"));
                return;
            }
            if (newp.size() < 6) {
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "新密码至少 6 位"));
                return;
            }
            g_store->set_password(newp);
            send_json(res, envelope_ok(json_object_new_object(), "密码已修改"));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/node/logout") {
            g_sessions.invalidate(session_from_cookie(req.headers));
            res.headers["Set-Cookie"] = "session=; Path=/; Max-Age=0";
            send_json(res, envelope_ok(json_object_new_object(), "已退出"));
            return;
        }

        // auth check (cookie)
        bool authed = g_sessions.valid(session_from_cookie(req.headers));
        // Strict auth is opt-in via DVS_REQUIRE_AUTH=1 (default off) so the
        // node-admin prototype keeps working without a login wall. When enabled,
        // every /api request except login must carry a valid session cookie.
        static bool g_require_auth = (std::getenv("DVS_REQUIRE_AUTH") != nullptr);
        // Browser sessions use the cookie. Internal platform calls use a
        // bearer token so barrier delivery cannot be forged on a shared LAN.
        bool token_ok = dvs::contract::bearer_ok(
            dvs::contract::extract_bearer(req.headers), g_api_token);
        if (g_require_auth && req.path.rfind("/api/", 0) == 0 &&
            req.path != "/api/v1/node/login" && !authed && !token_ok) {
            send_json(res, envelope_fail("UNAUTHORIZED", 401, "未登录或缺少令牌"));
            return;
        }

        if (req.method == "GET" && req.path == "/api/v1/node/info") {
            send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/role") {
            json_object* n = g_store->node();
            json_object* d = json_object_new_object();
            json_object* role = nullptr; json_object_object_get_ex(n, "role", &role);
            json_object* pr = nullptr; json_object_object_get_ex(n, "pendingRole", &pr);
            json_object_object_add(d, "role", json_object_get(role));
            json_object_object_add(d, "pendingRole", json_object_get(pr));
            json_object* cr = nullptr; json_object_object_get_ex(n, "configRevision", &cr);
            json_object* rrev = nullptr;
            if (cr) json_object_object_get_ex(cr, "role", &rrev);
            int rev = 1;
            if (rrev) { json_object* dd=nullptr; if (json_object_object_get_ex(rrev,"desired",&dd)) rev=json_object_get_int(dd); }
            json_object_object_add(d, "revision", json_object_new_int(rev));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // config scope
        {
            // regex manual
            std::string prefix = "/api/v1/node/config/";
            if (req.path.rfind(prefix, 0) == 0) {
                std::string rest = req.path.substr(prefix.size());
                std::string scope = rest;
                std::string action;
                size_t slash = rest.find('/');
                if (slash != std::string::npos) { scope = rest.substr(0, slash); action = rest.substr(slash+1); }
                // validate scope
                bool valid = false;
                for (int i = 0; i < 6; i++) if (scope == NodeStore::SCOPES[i]) valid = true;
                if (!valid) { send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "不支持的配置范围")); return; }
                std::string node_id = req.query.count("nodeId") ? req.query.at("nodeId") : get_str(g_store->node(), "id");
                if (req.method == "GET" && action.empty()) {
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, ""));
                    return;
                }
                if (req.method == "POST" && action.empty()) {
                    json_object* body = json_tokener_parse(req.body.c_str());
                    json_object* dv = nullptr;
                    if (body) {
                        if (!json_object_object_get_ex(body, "desiredValue", &dv)) json_object_object_get_ex(body, "value", &dv);
                    }
                    json_object* val = dv ? json_object_get(dv) : json_object_new_object();
                    const char* opid = nullptr;
                    std::string opstr;
                    if (body) {
                        json_object* o = nullptr;
                        if (json_object_object_get_ex(body, "operationId", &o) && o) { opstr = json_object_get_string(o); opid = opstr.c_str(); }
                    }
                    std::string cur_role = get_str(g_store->node(), "role");
                    g_store->set_desired(scope, val, opid);
                    std::string fail_code;
                    std::string fail_reason;
                    std::string scope_ok_msg;  // non-empty overrides the generic ok text
                    json_object* node_patch = json_object_new_object();
                    bool forward_media = false;
                    json_object* fwd = nullptr;
                    // Role activation is deferred to reboot so the running
                    // compositor is not cleared mid-session; saving here only
                    // records pendingRole and the reboot path clears media.
                    if (scope == "role") {
                        json_object* r = nullptr;
                        std::string want = (val && json_object_object_get_ex(val, "role", &r) && r) ? json_object_get_string(r) : "";
                        if (want != "input" && want != "output") {
                            fail_code = "INVALID_ARGUMENT";
                            fail_reason = "角色取值非法";
                        } else if (want == cur_role) {
                            json_object_object_add(node_patch, "pendingRole", json_object_new_null());
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                        } else {
                            json_object_object_add(node_patch, "pendingRole", json_object_new_string(want.c_str()));
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                        }
                    } else if (scope == "input") {
                        json_object* pv = nullptr;
                        std::string port = (val && json_object_object_get_ex(val, "port", &pv) && pv) ? json_object_get_string(pv) : "";
                        bool ok = !port.empty() && port.size() < 32;
                        if (ok) {
                            json_object* caps = nullptr;
                            json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                            json_object* arr = nullptr;
                            if (caps && json_object_object_get_ex(caps, "inputPorts", &arr) && arr &&
                                json_object_is_type(arr, json_type_array) && json_object_array_length(arr) > 0) {
                                ok = false;
                                for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
                                    json_object* e = json_object_array_get_idx(arr, (int)i);
                                    json_object* idv = nullptr;
                                    if (e && json_object_object_get_ex(e, "id", &idv) && idv &&
                                        port == json_object_get_string(idv)) { ok = true; break; }
                                }
                                if (!ok) { fail_code = "INVALID_ARGUMENT"; fail_reason = "采集口超出本机能力"; }
                            }
                        } else { fail_code = "INVALID_ARGUMENT"; fail_reason = "采集口不能为空"; }
                        if (fail_code.empty()) {
                            json_object* wv = nullptr;
                            json_object* wsrc = nullptr;
                            if (val && (json_object_object_get_ex(val, "whitelist", &wv) ||
                                        json_object_object_get_ex(val, "sources", &wsrc)) && (wv || wsrc)) {
                                json_object* wcheck = wv ? wv : wsrc;
                                std::string bad;
                                if (!validate_whitelist_value(wcheck, bad)) {
                                    fail_code = "INVALID_ARGUMENT";
                                    fail_reason = std::string("白名单地址/端口非法: ") + bad;
                                }
                            }
                        }
                        if (fail_code.empty()) {
                            TxParams txp;
                            std::string tx_err;
                            if (!tx_parse(val, txp, tx_err)) {
                                fail_code = "INVALID_ARGUMENT";
                                fail_reason = tx_err;
                            } else {
                                json_object* caps = nullptr;
                                json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                                TxCaps txcaps = tx_caps_from_node(caps);
                                if (!tx_validate(txp, txcaps, tx_err)) {
                                    fail_code = "INVALID_ARGUMENT";
                                    fail_reason = tx_err;
                                }
                            }
                            if (fail_code.empty()) {
                                /* Store an explicit shape, not the raw request. */
                                json_object* in = json_object_new_object();
                                json_object_object_add(in, "port", json_object_new_string(txp.port.c_str()));
                                if (txp.hasEncode) {
                                    json_object* enc = json_object_new_object();
                                    json_object_object_add(enc, "codec", json_object_new_string(txp.enc.codec.c_str()));
                                    json_object_object_add(enc, "width", json_object_new_int(txp.enc.width));
                                    json_object_object_add(enc, "height", json_object_new_int(txp.enc.height));
                                    json_object_object_add(enc, "fps", json_object_new_int(txp.enc.fps));
                                    json_object_object_add(enc, "bitrateKbps", json_object_new_int(txp.enc.bitrate));
                                    json_object_object_add(in, "encode", enc);
                                }
                                if (txp.hasTarget) {
                                    json_object* tgt = json_object_new_object();
                                    json_object_object_add(tgt, "sip", json_object_new_string(txp.tgt.sip.c_str()));
                                    json_object_object_add(tgt, "udpPort", json_object_new_int(txp.tgt.udpPort));
                                    json_object_object_add(in, "target", tgt);
                                }
                                json_object_object_add(node_patch, "input", in);
                                json_object* wv = nullptr;
                                if (val && (json_object_object_get_ex(val, "whitelist", &wv) ||
                                            json_object_object_get_ex(val, "sources", &wv)) && wv) {
                                    /* Whitelist stays RX admission (output role);
                                     * harmless to forward in the input role. */
                                    fwd = json_object_new_object();
                                    json_object_object_add(fwd, "whitelist", json_object_get(wv));
                                    forward_media = true;
                                }
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                bool tx_requested = txp.hasEncode || txp.hasTarget;
                                if (!tx_requested) {
                                    g_store->mark_applied(scope, 0);
                                } else if (!txp.hasEncode || !txp.hasTarget) {
                                    fail_code = "INVALID_ARGUMENT";
                                    fail_reason = "发送需同时给出编码与目标参数";
                                } else {
                                    /* Sync the engine role first so mutual
                                     * exclusion (RX off in input role) holds
                                     * before TX starts. */
                                    json_object* rv = json_object_new_object();
                                    json_object_object_add(rv, "scope", json_object_new_string("role"));
                                    json_object* rval = json_object_new_object();
                                    json_object_object_add(rval, "role", json_object_new_string(cur_role.c_str()));
                                    json_object_object_add(rv, "value", rval);
                                    std::string rr = media_cmd("configure", rv);
                                    (void)rr;  // role sync is best-effort; tx_start is authoritative
                                    json_object_put(rv);
                                    json_object* payload = tx_start_payload(txp);
                                    std::string tr = media_cmd("tx_start", payload);
                                    json_object_put(payload);
                                    json_object* to = json_tokener_parse(tr.c_str());
                                    bool ok = false;
                                    std::string code, msg, state;
                                    if (to) {
                                        json_object* okv = nullptr;
                                        if (json_object_object_get_ex(to, "ok", &okv) && okv)
                                            ok = json_object_get_boolean(okv) != 0;
                                        json_object* cv = nullptr;
                                        if (json_object_object_get_ex(to, "code", &cv) && cv)
                                            code = json_object_get_string(cv);
                                        json_object* mv2 = nullptr;
                                        if (json_object_object_get_ex(to, "msg", &mv2) && mv2)
                                            msg = json_object_get_string(mv2);
                                        json_object* dd = nullptr;
                                        if (json_object_object_get_ex(to, "data", &dd) && dd)
                                            state = get_str(dd, "state");
                                        json_object_put(to);
                                    } else {
                                        code = "TX_START_UNPARSABLE";
                                        msg = "media engine response unparsable";
                                    }
                                    if (!ok) {
                                        fail_code = code.empty() ? "TX_START_FAILED" : code;
                                        fail_reason = msg.empty() ? "media engine refused tx start" : msg;
                                        g_store->mark_failed(scope, fail_code.c_str(), fail_reason.c_str());
                                    } else if (state == "running") {
                                        g_store->mark_applied(scope, 0);
                                        scope_ok_msg = "输入采集发送已启动";
                                    } else {
                                        g_store->mark_applied(scope, 0);
                                        scope_ok_msg = "参数已下发，数据面待单板联调（TX_BACKEND_PENDING）";
                                    }
                                }
                            }
                        }
                    } else if (scope == "output") {
                        json_object* wv = nullptr; json_object* hv = nullptr; json_object* zv = nullptr;
                        json_object* mv = nullptr; json_object* cv = nullptr;
                        int w = (val && json_object_object_get_ex(val, "width", &wv) && wv) ? json_object_get_int(wv) : 0;
                        int h = (val && json_object_object_get_ex(val, "height", &hv) && hv) ? json_object_get_int(hv) : 0;
                        int z = (val && json_object_object_get_ex(val, "refreshHz", &zv) && zv) ? json_object_get_int(zv) : 0;
                        std::string mode = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "force";
                        std::string conn = (val && json_object_object_get_ex(val, "connector", &cv) && cv) ? json_object_get_string(cv) : "";
                        int lim_w = 7680, lim_h = 4320, lim_z = 60;
                        bool conn_known = false;
                        json_object* caps = nullptr;
                        json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                        json_object* arr = nullptr;
                        if (caps && json_object_object_get_ex(caps, "outputPorts", &arr) && arr &&
                            json_object_is_type(arr, json_type_array)) {
                            for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
                                json_object* e = json_object_array_get_idx(arr, (int)i);
                                json_object* idv = nullptr;
                                if (!e || !json_object_object_get_ex(e, "id", &idv) || !idv) continue;
                                if (!conn.empty() && conn != json_object_get_string(idv)) continue;
                                conn_known = true;
                                json_object* q = nullptr;
                                if (json_object_object_get_ex(e, "maxWidth", &q) && q) lim_w = json_object_get_int(q);
                                if (json_object_object_get_ex(e, "maxHeight", &q) && q) lim_h = json_object_get_int(q);
                                if (json_object_object_get_ex(e, "maxRefreshHz", &q) && q) lim_z = json_object_get_int(q);
                                break;
                            }
                            if (!conn.empty() && !conn_known) { fail_code = "INVALID_ARGUMENT"; fail_reason = "输出口不存在"; }
                        }
                        if (fail_code.empty()) {
                            if (w < 640 || w > lim_w || h < 480 || h > lim_h || z < 24 || z > lim_z) {
                                fail_code = "INVALID_ARGUMENT";
                                fail_reason = "输出参数超出本机能力范围";
                            } else if (mode != "force" && mode != "hotplug") {
                                fail_code = "INVALID_ARGUMENT";
                                fail_reason = "输出模式取值非法";
                            }
                        }
                        if (fail_code.empty()) {
                            json_object_object_add(node_patch, "output", json_object_get(val));
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            fwd = json_object_new_object();
                            json_object_object_add(fwd, "scope", json_object_new_string("output"));
                            json_object_object_add(fwd, "value", json_object_get(val));
                            forward_media = true;
                            // Stay in pending_apply here; the background thread
                            // flips the state after the engine answers.
                        }
                    } else if (scope == "network") {
                        json_object* mv = nullptr;
                        std::string nmode = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "dhcp";
                        json_object* iv = nullptr; json_object* kv = nullptr;
                        json_object* gv = nullptr; json_object* dvv = nullptr; json_object* sv = nullptr;
                        json_object* ifv = nullptr; json_object* cfv = nullptr;
                        std::string ip = (val && json_object_object_get_ex(val, "ip", &iv) && iv) ? json_object_get_string(iv) : "";
                        std::string mask = (val && json_object_object_get_ex(val, "mask", &kv) && kv) ? json_object_get_string(kv) : "";
                        std::string gw = (val && json_object_object_get_ex(val, "gateway", &gv) && gv) ? json_object_get_string(gv) : "";
                        std::string dns = (val && json_object_object_get_ex(val, "dns", &dvv) && dvv) ? json_object_get_string(dvv) : "";
                        std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                        std::string iface = (val && json_object_object_get_ex(val, "iface", &ifv) && ifv) ? json_object_get_string(ifv) : "eth0";
                        bool confirm = val && json_object_object_get_ex(val, "confirm", &cfv) && cfv && json_object_get_boolean(cfv);
                        std::string srv_host;
                        int srv_port = 0;
                        std::string srv_err;
                        if (nmode != "dhcp" && nmode != "static") { fail_code = "INVALID_ARGUMENT"; fail_reason = "网络方式非法"; }
                        else if (nmode == "static" && (!valid_ip4(ip) || !valid_ip4(mask) || !valid_ip4(gw) || !valid_ip4(dns))) {
                            fail_code = "INVALID_ARGUMENT"; fail_reason = "静态地址格式不正确";
                        } else if (srv.empty()) { fail_code = "INVALID_ARGUMENT"; fail_reason = "缺少注册服务器地址"; }
                        else if (!net_parse_server(srv, srv_host, srv_port, srv_err)) { fail_code = "INVALID_ARGUMENT"; fail_reason = std::string("注册服务器地址非法: ") + srv_err; }
                        else if (!net_valid_iface(iface)) { fail_code = "INVALID_ARGUMENT"; fail_reason = "网卡名非法"; }
                        if (fail_code.empty()) {
                            /* Snapshot before mutating so a failed system apply can
                             * roll back the node object to the previous values. */
                            json_object* net_old = nullptr;
                            json_object_object_get_ex(g_store->node(), "network", &net_old);
                            json_object* net_old_copy = net_old ? json_object_get(net_old) : json_object_new_object();
                            std::string ip_old = get_str(g_store->node(), "ip");
                            NetExecParams nep;
                            nep.mode = nmode; nep.ip = ip; nep.mask = mask;
                            nep.gw = gw; nep.dns = dns; nep.server = srv;
                            nep.iface = iface; nep.opid = opstr;
                            bool executed = false;
                            bool sys_ok = false;
                            bool sys_rb = false;
                            std::string sys_err;
                            std::string method = "systemd-networkd";
                            if (net_execution_requested(confirm, opstr)) {
                                NetExecResult nr = net_apply_real(nep);
                                executed = nr.executed; sys_ok = nr.ok;
                                sys_rb = nr.rolledBack; sys_err = nr.error;
                                method = nr.method;
                            } else {
                                std::string intent = std::string("{\"mode\":\"") + nmode +
                                    "\",\"iface\":\"" + iface + "\",\"server\":\"" + srv + "\"}";
                                sys_ok = intent_log_network(intent, sys_err);
                            }
                            json_object* nsys = json_object_new_object();
                            json_object_object_add(nsys, "executed", json_object_new_boolean(executed ? 1 : 0));
                            json_object_object_add(nsys, "method", json_object_new_string(method.c_str()));
                            json_object_object_add(nsys, "at", json_object_new_string(iso_now().c_str()));
                            if (!sys_err.empty())
                                json_object_object_add(nsys, "error", json_object_new_string(sys_err.c_str()));
                            json_object_object_add(nsys, "rolledBack", json_object_new_boolean(sys_rb ? 1 : 0));
                            if (!sys_ok) {
                                json_object* rb = json_object_new_object();
                                json_object_object_add(rb, "network", net_old_copy);
                                json_object_object_add(rb, "ip", json_object_new_string(ip_old.c_str()));
                                json_object_object_add(rb, "networkSystem", nsys);
                                g_store->update_node(rb);
                                json_object_put(rb);
                                fail_code = "NET_APPLY_FAILED";
                                fail_reason = executed ? (std::string("网络系统应用失败，已回滚: ") + sys_err)
                                                     : (std::string("网络意图落库失败: ") + sys_err);
                                json_object_put(net_old_copy);
                            } else {
                                json_object_put(net_old_copy);
                                json_object* net = json_object_new_object();
                                json_object_object_add(net, "mode", json_object_new_string(nmode.c_str()));
                                if (nmode == "static") {
                                    json_object_object_add(net, "mask", json_object_new_string(mask.c_str()));
                                    json_object_object_add(net, "gateway", json_object_new_string(gw.c_str()));
                                    json_object_object_add(net, "dns", json_object_new_string(dns.c_str()));
                                    json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                                    json_object_object_add(net, "iface", json_object_new_string(iface.c_str()));
                                    json_object_object_add(node_patch, "ip", json_object_new_string(ip.c_str()));
                                    json_object_object_add(net, "ip", json_object_new_string(ip.c_str()));
                                } else {
                                    json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                                    json_object_object_add(net, "iface", json_object_new_string(iface.c_str()));
                                }
                                json_object_object_add(node_patch, "network", net);
                                json_object_object_add(node_patch, "networkSystem", nsys);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                g_store->mark_applied(scope, 0);
                                scope_ok_msg = executed ? "系统网络已切换并通过连通检查"
                                                      : "意图已保存，系统网络未切换（需隔离环境确认执行）";
                            }
                        }
                    } else if (scope == "registration") {
                        json_object* mv = nullptr; json_object* sv = nullptr;
                        std::string rm = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "";
                        std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                        if ((rm != "auto" && rm != "manual") || srv.empty()) {
                            fail_code = "INVALID_ARGUMENT"; fail_reason = "缺少注册服务器地址";
                        } else {
                            json_object_object_add(node_patch, "registerMode", json_object_new_string(rm.c_str()));
                            json_object* net0 = nullptr;
                            json_object_object_get_ex(g_store->node(), "network", &net0);
                            json_object* net = net0 ? json_object_get(net0) : json_object_new_object();
                            if (!json_object_is_type(net, json_type_object)) { json_object_put(net); net = json_object_new_object(); }
                            json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                            json_object_object_add(node_patch, "network", net);
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                            if (rm == "auto" && g_reg) g_reg->trigger_async();
                        }
                    } else if (scope == "sip") {
                        json_object* ev = nullptr;
                        bool enabled = val && json_object_object_get_ex(val, "enabled", &ev) && ev && json_object_get_boolean(ev);
                        if (!enabled) {
                            json_object* sip = json_object_new_object();
                            json_object_object_add(sip, "enabled", json_object_new_boolean(0));
                            json_object_object_add(sip, "state", json_object_new_string("disabled"));
                            json_object_object_add(node_patch, "sip", sip);
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                        } else {
                            json_object* sv = nullptr; json_object* pv = nullptr;
                            json_object* dvv = nullptr; json_object* uv = nullptr;
                            std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                            int port = (val && json_object_object_get_ex(val, "port", &pv) && pv) ? json_object_get_int(pv) : 0;
                            std::string dev = (val && json_object_object_get_ex(val, "deviceId", &dvv) && dvv) ? json_object_get_string(dvv) : "";
                            std::string usr = (val && json_object_object_get_ex(val, "user", &uv) && uv) ? json_object_get_string(uv) : "";
                            if (srv.empty() || port < 1 || port > 65535 || dev.empty() || usr.empty()) {
                                fail_code = "INVALID_ARGUMENT"; fail_reason = "SIP 参数不完整";
                            } else {
                                /* Publish the probing window so polling can see
                                 * it; the result below overwrites it. This is
                                 * a connectivity probe, not a SIP session. */
                                json_object* probing = json_object_new_object();
                                json_object* sip_probe = json_object_new_object();
                                json_object_object_add(sip_probe, "enabled", json_object_new_boolean(1));
                                json_object_object_add(sip_probe, "state", json_object_new_string("probing"));
                                json_object_object_add(sip_probe, "server", json_object_new_string(srv.c_str()));
                                json_object_object_add(sip_probe, "port", json_object_new_int(port));
                                json_object_object_add(probing, "sip", sip_probe);
                                g_store->update_node(probing);
                                json_object_put(probing);
                                if (!tcp_reachable(srv, port, 3000)) {
                                json_object* sip = json_object_new_object();
                                json_object_object_add(sip, "enabled", json_object_new_boolean(1));
                                json_object_object_add(sip, "state", json_object_new_string("failed"));
                                json_object_object_add(sip, "server", json_object_new_string(srv.c_str()));
                                json_object_object_add(sip, "port", json_object_new_int(port));
                                json_object_object_add(node_patch, "sip", sip);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                fail_code = "SIP_UNREACHABLE"; fail_reason = "SIP 服务不可达";
                            } else {
                                json_object* sip = json_object_new_object();
                                json_object_object_add(sip, "enabled", json_object_new_boolean(1));
                                json_object_object_add(sip, "state", json_object_new_string("connected"));
                                json_object_object_add(sip, "server", json_object_new_string(srv.c_str()));
                                json_object_object_add(sip, "port", json_object_new_int(port));
                                json_object_object_add(node_patch, "sip", sip);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                g_store->mark_applied(scope, 0);
                            }
                            }
                        }
                    }
                    if (!fail_code.empty()) g_store->mark_failed(scope, fail_code.c_str(), fail_reason.c_str());
                    // Media forward decides the final config state. Input is
                    // synchronous so an engine refusal is visible at once.
                    // Output stays pending and a background thread flips the
                    // state after the engine answers; a revision check keeps a
                    // stale answer from overwriting a newer save.
                    if (forward_media && fwd) {
                        if (scope == "output") {
                            int want_rev = config_desired_revision(scope, node_id);
                            json_object* fwd_bg = json_object_get(fwd);
                            std::string sc = scope;
                            std::string nid = node_id;
                            std::thread([fwd_bg, sc, nid, want_rev]() {
                                std::string fr = media_cmd("configure", fwd_bg);
                                json_object* fo = json_tokener_parse(fr.c_str());
                                bool ok = false;
                                std::string code, msg;
                                if (fo) {
                                    json_object* okv = nullptr;
                                    json_object* codev = nullptr;
                                    json_object* msgv = nullptr;
                                    if (json_object_object_get_ex(fo, "ok", &okv) && okv)
                                        ok = json_object_get_boolean(okv) != 0;
                                    if (json_object_object_get_ex(fo, "code", &codev) && codev)
                                        code = json_object_get_string(codev);
                                    if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
                                        msg = json_object_get_string(msgv);
                                    json_object_put(fo);
                                } else {
                                    code = "MEDIA_CONFIGURE_UNPARSABLE";
                                    msg = "media engine response unparsable";
                                }
                                apply_media_config_outcome(sc, nid, want_rev, ok, code, msg);
                                json_object_put(fwd_bg);
                            }).detach();
                            json_object_put(fwd);
                        } else {
                            std::string fr = media_cmd("configure", fwd);
                            json_object* fo = json_tokener_parse(fr.c_str());
                            if (fo) {
                                json_object* okv = nullptr;
                                bool ok = false;
                                std::string code, msg;
                                if (json_object_object_get_ex(fo, "ok", &okv) && okv)
                                    ok = json_object_get_boolean(okv) != 0;
                                json_object* codev = nullptr;
                                if (json_object_object_get_ex(fo, "code", &codev) && codev)
                                    code = json_object_get_string(codev);
                                json_object* msgv = nullptr;
                                if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
                                    msg = json_object_get_string(msgv);
                                int want_rev = config_desired_revision(scope, node_id);
                                apply_media_config_outcome(scope, node_id, want_rev, ok,
                                                         code, msg);
                                json_object_put(fo);
                            } else {
                                int want_rev = config_desired_revision(scope, node_id);
                                apply_media_config_outcome(scope, node_id, want_rev, false,
                                                         "MEDIA_CONFIGURE_UNPARSABLE",
                                                         "media engine response unparsable");
                            }
                            json_object_put(fwd);
                        }
                    } else if (fwd) { json_object_put(fwd); }
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    json_object_object_add(patch, "configRevision", g_store->config_revision());
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object_put(node_patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    if (body) json_object_put(body);
                    const char* ok_msg = (scope == "output" && forward_media) ? "配置已保存，等待媒体生效" : "配置已提交";
                    const char* ok_text = scope_ok_msg.empty() ? ok_msg : scope_ok_msg.c_str();
                    send_json(res, envelope_ok(cfg, fail_code.empty() ? ok_text : "配置失败"));
                    return;
                }
                if (req.method == "POST" && action == "retry") {
                    g_store->retry(scope);
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, "配置已重新提交，等待应用"));
                    return;
                }
                send_json(res, envelope_fail("NOT_FOUND", 404, "未定义的节点配置操作"));
                return;
            }
        }

        if (req.method == "GET" && req.path == "/api/v1/nodes") {
            json_object* arr = json_object_new_array();
            json_object_array_add(arr, json_object_get(g_store->node()));
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", arr);
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(1));
            json_object_object_add(d, "total", json_object_new_int(1));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* patch = nullptr;
            if (body) {
                if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
            }
            // update lastHeartbeat
            json_object* hp = json_object_new_object();
            json_object_object_add(hp, "lastHeartbeat", json_object_new_string(iso_now().c_str()));
            if (patch) {
                json_object* m = nullptr;
                if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
            }
            g_store->update_node(hp);
            json_object_put(hp);
            if (body) json_object_put(body);
            send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* ids = nullptr;
            if (body) json_object_object_get_ex(body, "targetNodeIds", &ids);
            if (!ids && body) json_object_object_get_ex(body, "nodeIds", &ids);
            json_object* d = json_object_new_object();
            json_object* arr = json_object_new_array();
            if (ids) {
                // assume array
                int len = json_object_array_length(ids);
                for (int i = 0; i < len; i++) json_object_array_add(arr, json_object_get(json_object_array_get_idx(ids, i)));
            }
            json_object_object_add(d, "nodeIds", arr);
            json_object_object_add(d, "status", json_object_new_string("applied"));
            if (body) json_object_put(body);
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // node by id routes
        if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
            std::string id = rest;
            std::string action;
            size_t slash = rest.find('/');
            if (slash != std::string::npos) { id = rest.substr(0, slash); action = rest.substr(slash+1); }
            if (req.method == "GET" && action.empty()) {
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "PUT" && action.empty()) {
                json_object* body = json_tokener_parse(req.body.c_str());
                std::string name = get_str(body, "name");
                if (body) json_object_put(body);
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "name", json_object_new_string(name.c_str()));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "retire") {
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "lifecycle", json_object_new_string("retired"));
                json_object_object_add(patch, "online", json_object_new_boolean(0));
                json_object_object_add(patch, "bind", json_object_new_string("—"));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "heartbeat") {
                json_object* body = json_tokener_parse(req.body.c_str());
                json_object* patch = nullptr;
                if (body) {
                    if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
                }
                json_object* hp = json_object_new_object();
                json_object_object_add(hp, "lastHeartbeat", json_object_new_string(iso_now().c_str()));
                if (patch) {
                    json_object* m = nullptr;
                    if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
                }
                g_store->update_node(hp);
                json_object_put(hp);
                if (body) json_object_put(body);
                send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
                return;
            }
        }

        // media endpoints
        if (req.method == "GET" && req.path == "/api/v1/node/media/status") {
            // pass through the engine response (or structured error envelope)
            std::string r = media_query("query_state", nullptr);
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o); // already a full envelope from the media engine
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/events") {
            /* Both channels hear broadcasts; merge without losing either. */
            json_object* merged = json_object_new_array();
            json_object* a = g_media ? g_media->drain_events() : nullptr;
            json_object* b = g_media_q ? g_media_q->drain_events() : nullptr;
            for (json_object* src : {a, b}) {
                if (!src) continue;
                int n = json_object_array_length(src);
                for (int i = 0; i < n; i++)
                    json_object_array_add(merged, json_object_get(json_object_array_get_idx(src, i)));
                json_object_put(src);
            }
            send_json(res, envelope_ok(merged, ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/media/prepare") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string r = media_cmd("prepare", body);
            if (body) json_object_put(body);
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            return;
        }
        // Manual two-phase close: passthrough commit/abort so a prepared
        // operation can be finished by hand when the barrier path is down.
        if (req.method == "POST" && req.path == "/api/v1/node/media/commit") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string r = media_cmd("commit", body);
            if (body) json_object_put(body);
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/media/abort") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string r = g_media->request("abort", body);
            if (body) json_object_put(body);
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            return;
        }
        // Receive a platform-computed per-node sub-layout, forward it to the
        // media engine as a prepare, and report the result back to the
        // platform barrier. The node never re-fragments.
        if (req.method == "POST" && req.path == "/api/v1/node/media/layout") {
            if (!g_api_token.empty() && !token_ok) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "missing or bad token"));
                return;
            }
            json_object* body = json_tokener_parse(req.body.c_str());
            if (!body || dvs::contract::validate_layout_prepare(body).empty() == false) {
                std::string reason = body ? dvs::contract::validate_layout_prepare(body) : "INVALID_ARGUMENT:empty body";
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, reason.c_str()));
                return;
            }
            std::string opid = get_str(body, "operationId");
            int rev = 1;
            json_object* rv = nullptr;
            if (body && json_object_object_get_ex(body, "revision", &rv)) rev = json_object_get_int(rv);
            int64_t applyAtNs = 0;
            json_object* av = nullptr;
            if (body && json_object_object_get_ex(body, "applyAtNs", &av))
                applyAtNs = (int64_t)json_object_get_int64(av);
            if (opid.empty()) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "missing operationId"));
                return;
            }
            json_object* wins0 = nullptr;
            if (body) json_object_object_get_ex(body, "windows", &wins0);
            if (wins0 && !json_object_is_type(wins0, json_type_array)) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "windows must be an array"));
                return;
            }
            /* Translate the platform sub-layout into engine streams before
             * staging; a whole-order rejection answers 400 with the reason. */
            json_object* prep = nullptr;
            std::string terr;
            if (!translate_platform_layout(body, &prep, terr)) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, terr.c_str()));
                return;
            }
            std::string r = media_cmd("prepare", prep);
            json_object_put(prep);
            std::string nodeId = get_str(g_store->node(), "id");
            if (nodeId.empty()) nodeId = "local-node";
            // detach reporter; platform_url captured by value for the thread.
            std::thread([opid, rev, applyAtNs, nodeId, platform_url]() {
                agent_report_commit(opid, rev, applyAtNs, nodeId, platform_url);
            }).detach();
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            if (body) json_object_put(body);
            return;
        }
        // Expose per-lane RX readiness from the engine for barrier checks.
        if (req.method == "GET" && req.path == "/api/v1/node/media/lanes") {
            std::string r = media_query("query_state", nullptr);
            json_object* o = json_tokener_parse(r.c_str());
            json_object* d = nullptr;
            json_object* lanes = nullptr;
            if (o && json_object_object_get_ex(o, "data", &d) && d)
                json_object_object_get_ex(d, "lanes", &lanes);
            send_json(res, envelope_ok(lanes ? json_object_get(lanes)
                                             : json_object_new_array(), ""));
            if (o) json_object_put(o);
            return;
        }
        // TX session snapshot (input role): state is idle/configured/
        // running/failed. An unreachable engine yields its error envelope
        // instead of an empty object, so unknown is never misread as idle.
        if (req.method == "GET" && req.path == "/api/v1/node/media/tx") {
            std::string r = media_query("query_state", nullptr);
            json_object* o = json_tokener_parse(r.c_str());
            json_object* okv = nullptr;
            bool ok = o && json_object_object_get_ex(o, "ok", &okv) && okv &&
                      json_object_get_boolean(okv);
            json_object* d = nullptr;
            json_object* tx = nullptr;
            if (ok && json_object_object_get_ex(o, "data", &d) && d)
                json_object_object_get_ex(d, "tx", &tx);
            if (!ok) {
                send_json(res, o ? o : json_object_new_object());
                return;
            }
            send_json(res, envelope_ok(tx ? json_object_get(tx)
                                           : json_object_new_object(), ""));
            if (o) json_object_put(o);
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/sources") {
            send_json(res, envelope_ok(json_object_new_object(), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/logs") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            send_json(res, envelope_ok(d, ""));
            return;
        }
        // Accept the platform-owned config subset. Only geometry and role
        // apply; every other key is reported as ignored and never stored.
        if (req.method == "POST" && req.path == "/api/v1/node/config/sync") {
            if (!g_api_token.empty() && !token_ok) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "missing or bad token"));
                return;
            }
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string verr = body ? dvs::contract::validate_config_update(body) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            json_object* cfg = nullptr;
            if (body) json_object_object_get_ex(body, "config", &cfg);
            json_object* patch = json_object_new_object();
            json_object* ignored = json_object_new_array();
            if (cfg && json_object_is_type(cfg, json_type_object)) {
                json_object_object_foreach(cfg, key, val) {
                    std::string k(key);
                    if (k == "geometry") {
                        json_object_object_add(patch, "geometry", json_object_get(val));
                    } else if (k == "role") {
                        g_store->set_desired("role", json_object_get(val), nullptr);
                        json_object* rv = nullptr;
                        if (json_object_is_type(val, json_type_object) &&
                            json_object_object_get_ex(val, "role", &rv) && rv) {
                            json_object* pr = json_object_new_string(json_object_get_string(rv));
                            json_object* pp = json_object_new_object();
                            json_object_object_add(pp, "pendingRole", pr);
                            g_store->update_node(pp);
                            json_object_put(pp);
                        } else if (json_object_is_type(val, json_type_string)) {
                            json_object* pr = json_object_new_string(json_object_get_string(val));
                            json_object* pp = json_object_new_object();
                            json_object_object_add(pp, "pendingRole", pr);
                            g_store->update_node(pp);
                            json_object_put(pp);
                        }
                    } else {
                        json_object_array_add(ignored, json_object_new_string(k.c_str()));
                    }
                }
            }
            json_object_object_add(patch, "configSource", json_object_new_string("platform"));
            g_store->update_node(patch);
            json_object_put(patch);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "applied", json_object_new_boolean(1));
            json_object_object_add(d, "source", json_object_new_string("platform"));
            json_object_object_add(d, "ignored", ignored);
            if (body) json_object_put(body);
            send_json(res, envelope_ok(d, "config synced"));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/capability") {
            std::string cr = media_query("capability", nullptr);
            json_object* co = json_tokener_parse(cr.c_str());
            if (co) {
                json_object* okv = nullptr;
                json_object* data = nullptr;
                if (json_object_object_get_ex(co, "ok", &okv) && okv && json_object_get_boolean(okv) &&
                    json_object_object_get_ex(co, "data", &data) && data) {
                    json_object* patch = json_object_new_object();
                    json_object* caps = nullptr;
                    json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                    json_object* merged = caps ? json_object_get(caps) : json_object_new_object();
                    json_object_object_foreach(data, k, v) {
                        json_object_object_add(merged, k, json_object_get(v));
                    }
                    json_object_object_add(patch, "capabilities", merged);
                    g_store->update_node(patch);
                    json_object_put(patch);
                    g_cap_ok_ms = agent_now_ms();
                }
                json_object_put(co);
            }
            json_object* n = g_store->node();
            json_object* cap = nullptr;
            json_object_object_get_ex(n, "capabilities", &cap);
            json_object* cap_out = cap ? json_object_get(cap) : json_object_new_object();
            /* Freshness is served, never stored: an engine that went silent
             * leaves an explicitly expired snapshot instead of a fresh look. */
            long long age = (g_cap_ok_ms > 0) ? agent_now_ms() - g_cap_ok_ms : -1;
            bool stale = (age < 0) || (age > kCapStaleMs);
            json_object_object_add(cap_out, "capStale", json_object_new_boolean(stale ? 1 : 0));
            json_object_object_add(cap_out, "capAgeMs", json_object_new_int64(age));
            send_json(res, envelope_ok(cap_out, ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/register") {
            bool ok = g_reg ? g_reg->trigger_once() : false;
            json_object* d = json_object_new_object();
            json_object_object_add(d, "registerState", json_object_new_string(ok ? "registered" : "failed"));
            json_object_object_add(d, "online", json_object_new_boolean(g_reg && g_reg->online() ? 1 : 0));
            send_json(res, envelope_ok(d, ok ? "设备注册成功" : "注册失败，可修改地址后重试"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/upgrade/upload") {
            std::string fn = req.query.count("filename") ? req.query.at("filename") : "fw_upload.bin";
            auto hit = req.headers.find("X-Filename");
            if (hit != req.headers.end() && !hit->second.empty()) fn = hit->second;
            std::string base = fn;
            size_t slash = base.find_last_of("/\\");
            if (slash != std::string::npos) base = base.substr(slash + 1);
            if (base.empty() || base.size() > 128) {
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "文件名非法"));
                return;
            }
            std::string dest = "/tmp/" + base;
            std::ofstream out(dest, std::ios::binary);
            if (!out) {
                send_json(res, envelope_fail("INTERNAL", 500, "无法写入暂存"));
                return;
            }
            out.write(req.body.data(), (std::streamsize)req.body.size());
            out.close();
            json_object* d = json_object_new_object();
            json_object_object_add(d, "path", json_object_new_string(dest.c_str()));
            json_object_object_add(d, "size", json_object_new_int64((int64_t)req.body.size()));
            send_json(res, envelope_ok(d, "固件已上传"));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/capacity") {
            send_json(res, envelope_ok(node_capacity_json(), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/version") {
            // Bundle contract report: this agent's versions plus the live
            // media engine hello, so ops can confirm a co-released pair.
            // The gate verdict rides along: layout verbs are refused while
            // a reachable engine disagrees, queries stay open.
            std::string hr = media_query("hello", nullptr);
            bool answered = false, match = false;
            std::string eb;
            int ei = -1;
            MediaIpcClient::parse_hello(hr, answered, match, eb, ei);
            {
                std::lock_guard<std::mutex> lk(g_ver_mu);
                g_ver_at = agent_now_ms();
                if (answered) {
                    g_ver_known = true;
                    g_ver_match = match;
                }
            }
            std::string gate_reason;
            bool allowed = media_layout_allowed(gate_reason);
            json_object* ho = json_tokener_parse(hr.c_str());
            json_object* d = json_object_new_object();
            json_object_object_add(d, "bundleVersion",
                                  json_object_new_string(DVS_NODE_BUNDLE_VERSION));
            json_object_object_add(d, "ipcVersion",
                                  json_object_new_int(DVS_MEDIA_IPC_VERSION));
            json_object_object_add(d, "media",
                                  ho ? ho : json_object_new_object());
            json_object_object_add(d, "match", json_object_new_boolean(match ? 1 : 0));
            json_object_object_add(d, "gateBlocked", json_object_new_boolean(allowed ? 0 : 1));
            json_object_object_add(d, "gateReason", json_object_new_string(gate_reason.c_str()));
            json_object* link = json_object_new_object();
            json_object_object_add(link, "state",
                                  json_object_new_string(engine_link_state().c_str()));
            json_object_object_add(link, "failStreak",
                                  json_object_new_int(g_media_q ? g_media_q->fail_streak() : 0));
            long long ok_age = -1;
            if (g_media_q && g_media_q->last_ok_ms() > 0)
                ok_age = agent_now_ms() - g_media_q->last_ok_ms();
            json_object_object_add(link, "lastOkAgeMs", json_object_new_int64(ok_age));
            json_object_object_add(d, "engineLink", link);
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // maintenance
        if (req.method == "POST" && req.path == "/api/v1/node/reboot") {
            send_json(res, envelope_ok(g_maint->reboot(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/factory-reset") {
            send_json(res, envelope_ok(g_maint->factory_reset(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string fn = body ? get_str(body, "filename") : std::string("fw.bin");
            if (fn.empty()) fn = "fw.bin";
            std::string sha = body ? get_str(body, "sha256") : std::string();
            if (body) json_object_put(body);
            send_json(res, envelope_ok(g_maint->upgrade(fn, sha), ""));
            return;
        }

        if (req.method == "GET" && req.path == "/api/v1/audit-events") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(100));
            json_object_object_add(d, "total", json_object_new_int(0));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // static files
        if (req.method == "GET") {
            std::string path = req.path;
            if (path == "/" || path.empty()) path = "/index.html";
            std::string full = g_web_root + path;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::string content = read_file(full);
                res.status = 200;
                res.body = content;
                res.content_type = guess_ct(full);
                res.headers["Cache-Control"] = "no-cache";
                res.headers["X-Content-Type-Options"] = "nosniff";
                return;
            }
        }

        send_json(res, envelope_fail("NOT_FOUND", 404, ("未定义的 API：" + req.method + " " + req.path).c_str()));
    };

    HttpServer server(port, handler);
    server.start();
    printf("dvs-agent listening on 0.0.0.0:%d\n", port);
    fflush(stdout);

    // registration + heartbeat loop (no-op / local-autonomous when platform URL
    // is empty; never disturbs media output on transport failure)
    g_reg->start();

    while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
    return 0;
}
