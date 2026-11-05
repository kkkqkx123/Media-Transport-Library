// dvs_platform main: node gateway + docs CRUD + op barrier + serial upgrade.
#include <json-c/json.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
static std::string g_web_root;

#include "dvs_contract.h"
#include "http_client.h"
#include "http_server.h"
#include "layout.h"
#include "ops.h"
#include "platform_store.h"

static PlatformStore* g_store = nullptr;
static OpCoordinator g_ops;
static std::mutex g_ops_mu;
static std::mutex g_idem_mu;
static std::map<std::string, std::string> g_idem_cache;

struct ChannelRegistry {
    std::mutex mu;
    std::map<std::string, int> node_fd;
    std::map<int, std::string> fd_node;
    std::map<std::string, std::string> transport;
    std::map<std::string, std::string> idem;
    std::map<std::string, std::string> upgrade_progress;
    std::mutex send_mu;
    void register_conn(const std::string& node, int fd) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = node_fd.find(node);
        if (it != node_fd.end() && it->second != fd) {
            int old = it->second;
            fd_node.erase(old);
            ::shutdown(old, SHUT_RDWR);
        }
        node_fd[node] = fd;
        fd_node[fd] = node;
        transport[node] = "ws";
    }
    void unregister_fd(int fd) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = fd_node.find(fd);
        if (it == fd_node.end()) return;
        std::string node = it->second;
        fd_node.erase(it);
        auto nit = node_fd.find(node);
        if (nit != node_fd.end() && nit->second == fd) node_fd.erase(nit);
    }
    std::string node_of(int fd) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = fd_node.find(fd);
        return it == fd_node.end() ? "" : it->second;
    }
    int fd_of(const std::string& node) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = node_fd.find(node);
        return it == node_fd.end() ? -1 : it->second;
    }
    void set_transport(const std::string& node, const std::string& t) {
        std::lock_guard<std::mutex> lk(mu);
        transport[node] = t;
    }
    bool idem_hit(const std::string& k, std::string& out) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = idem.find(k);
        if (it == idem.end()) return false;
        out = it->second;
        return true;
    }
    void idem_store(const std::string& k, const std::string& v) {
        std::lock_guard<std::mutex> lk(mu);
        if (idem.size() > 4096) idem.clear();
        idem[k] = v;
    }
};
static ChannelRegistry g_chan;

struct UpgradeJob {
    std::string task_id;
    std::vector<std::string> nodes;
    std::string filename;
    std::string sha256;
};
static std::mutex g_up_mu;
static std::condition_variable g_up_cv;
static std::queue<UpgradeJob> g_up_q;
// Shared control-plane bearer token. Empty means no check so a local
// prototype keeps working; when set, agent-to-platform calls must carry it.
static std::string g_api_token =
    (std::getenv("DVS_API_TOKEN") ? std::getenv("DVS_API_TOKEN") : "");

namespace {
namespace ct = dvs::contract;
std::string idempotency_key(const HttpRequest& req) {
    for (auto& h : req.headers) {
        std::string k = h.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "idempotency-key" && !h.second.empty()) return h.second;
    }
    return "";
}
std::string now_iso() {
    char b[32];
    time_t t = time(nullptr);
    strftime(b, sizeof(b), "%FT%TZ", gmtime(&t));
    return b;
}
int64_t now_sec() { return (int64_t)time(nullptr); }
uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
json_object* ok_envelope(json_object* data, const std::string& msg) {
    return ct::make_ok(data, msg, "p");
}
json_object* fail_envelope(const char* code, int status, const std::string& msg) {
    return ct::make_fail(code, status, msg, "p");
}
void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}
std::string body_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}
bool bearer_ok(const HttpRequest& req) {
    return ct::bearer_ok(ct::extract_bearer(req.headers), g_api_token);
}
bool require_bearer(const HttpRequest& req, HttpResponse& res) {
    if (bearer_ok(req)) return true;
    send_json(res, fail_envelope("UNAUTHORIZED", 401, "missing or bad token"));
    return false;
}
// Read an integer field from a rect-style JSON object (default 0).
int rect_int(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int(v);
    return 0;
}
// Intersect the global window set against one node's canvas rect and
// produce the node-local window list (with source crop). Caller owns the
// returned array. Pure pixel math; node never rounds (platform owns rounding).
json_object* fragment_for_node(json_object* windows, const Rect& node,
                               int wall_w, int wall_h) {
    json_object* out = json_object_new_array();
    if (!windows || !json_object_is_type(windows, json_type_array)) return out;
    int n = json_object_array_length(windows);
    for (int i = 0; i < n; i++) {
        json_object* w = json_object_array_get_idx(windows, i);
        json_object* win = nullptr, * src = nullptr, * stream = nullptr;
        json_object_object_get_ex(w, "win", &win);
        json_object_object_get_ex(w, "src", &src);
        json_object_object_get_ex(w, "stream", &stream);
        Rect gw{rect_int(win, "x"), rect_int(win, "y"),
                rect_int(win, "w"), rect_int(win, "h")};
        Rect gs{0, 0, rect_int(src, "w"), rect_int(src, "h")};
        Rect dst{}, ls{};
        if (layout_window_fragment(gw, node, gs.w, gs.h, &dst, &ls)) {
            json_object* nw = json_object_new_object();
            json_object* nd = json_object_new_object();
            json_object_object_add(nd, "x", json_object_new_int(dst.x));
            json_object_object_add(nd, "y", json_object_new_int(dst.y));
            json_object_object_add(nd, "w", json_object_new_int(dst.w));
            json_object_object_add(nd, "h", json_object_new_int(dst.h));
            json_object* ns = json_object_new_object();
            json_object_object_add(ns, "x", json_object_new_int(ls.x));
            json_object_object_add(ns, "y", json_object_new_int(ls.y));
            json_object_object_add(ns, "w", json_object_new_int(ls.w));
            json_object_object_add(ns, "h", json_object_new_int(ls.h));
            json_object_object_add(nw, "win", nd);
            json_object_object_add(nw, "src", ns);
            if (stream) json_object_object_add(nw, "stream", json_object_get(stream));
            json_object_array_add(out, nw);
        }
    }
    (void)wall_w; (void)wall_h; // reserved for future global-coord validation
    return out;
}
// Generic docs CRUD mounted at /api/v1/<kind>[/<id>].
bool route_docs(const std::string& kind, const HttpRequest& req, HttpResponse& res) {
    std::string base = "/api/v1/" + kind;
    if (req.path != base && req.path.rfind(base + "/", 0) != 0) return false;
    std::string id = (req.path.size() > base.size()) ? req.path.substr(base.size() + 1) : "";
    if (req.method == "GET" && id.empty()) {
        json_object* arr = json_object_new_array();
        for (auto& i : g_store->list_ids(kind)) {
            json_object* d = g_store->get_doc(kind, i);
            if (d) json_object_array_add(arr, d);
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "items", arr);
        send_json(res, ok_envelope(d, ""));
        return true;
    }
    if (req.method == "GET") {
        json_object* d = g_store->get_doc(kind, id);
        if (!d) { send_json(res, fail_envelope("NOT_FOUND", 404, "not found")); return true; }
        send_json(res, ok_envelope(d, ""));
        return true;
    }
    if (req.method == "POST" || req.method == "PUT") {
        std::string ikey = idempotency_key(req);
        std::string cache_key;
        if (!ikey.empty()) {
            cache_key = req.method + " " + req.path + "#" + ikey;
            std::lock_guard<std::mutex> lk(g_idem_mu);
            auto it = g_idem_cache.find(cache_key);
            if (it != g_idem_cache.end()) {
                res.body = it->second;
                res.content_type = "application/json; charset=utf-8";
                return true;
            }
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        if (!b) b = json_object_new_object();
        if (id.empty()) {
            id = body_str(b, "id");
            if (id.empty()) {
                char nb[32];
                snprintf(nb, sizeof(nb), "%s-%ld", kind.c_str(), (long)now_sec());
                id = nb;
                json_object_object_add(b, "id", json_object_new_string(id.c_str()));
            }
        }
        g_store->put_doc(kind, id, b);
        g_store->audit("{\"kind\":\"" + kind + "\",\"id\":\"" + id + "\"}");
        json_object* d = g_store->get_doc(kind, id);
        json_object_put(b);
        send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
        if (!cache_key.empty()) {
            std::lock_guard<std::mutex> lk(g_idem_mu);
            if (g_idem_cache.size() < 1024) g_idem_cache[cache_key] = res.body;
        }
        return true;
    }
    if (req.method == "DELETE") {
        g_store->del_doc(kind, id);
        send_json(res, ok_envelope(json_object_new_object(), ""));
        return true;
    }
    return false;
}

void apply_hello_doc(json_object* data) {
    std::string id = body_str(data, "id");
    if (id.empty()) id = body_str(data, "sn");
    if (id.empty()) return;
    json_object* merged = json_object_get(data);
    json_object_object_add(merged, "online", json_object_new_boolean(1));
    json_object_object_add(merged, "lastHeartbeat",
                           json_object_new_string(now_iso().c_str()));
    json_object_object_add(merged, "transport", json_object_new_string("ws"));
    g_store->put_doc("node", id, merged);
    json_object_put(merged);
    g_chan.set_transport(id, "ws");
}

void apply_heartbeat_doc(const std::string& id, json_object* data) {
    json_object* cur = g_store->get_doc("node", id);
    if (!cur) return;
    json_object_object_add(cur, "online", json_object_new_boolean(1));
    json_object_object_add(cur, "lastHeartbeat",
                           json_object_new_string(now_iso().c_str()));
    json_object_object_add(cur, "transport", json_object_new_string("ws"));
    json_object* ld = nullptr;
    if (data && json_object_object_get_ex(data, "lanesDigest", &ld) && ld) {
        json_object* curm = nullptr;
        if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
            curm = json_object_new_object();
            json_object_object_add(cur, "metrics", curm);
        }
        json_object_object_del(curm, "lanesDigest");
        json_object_object_add(curm, "lanesDigest", json_object_get(ld));
    }
    json_object* txd = nullptr;
    if (data && json_object_object_get_ex(data, "txDigest", &txd) && txd) {
        json_object* curm = nullptr;
        if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
            curm = json_object_new_object();
            json_object_object_add(cur, "metrics", curm);
        }
        json_object_object_del(curm, "txDigest");
        json_object_object_add(curm, "txDigest", json_object_get(txd));
    }
    json_object* sipd = nullptr;
    if (data && json_object_object_get_ex(data, "sip", &sipd) && sipd) {
        json_object_object_del(cur, "sipDigest");
        json_object_object_add(cur, "sipDigest", json_object_get(sipd));
    }
    g_store->put_doc("node", id, cur);
    json_object_put(cur);
    g_chan.set_transport(id, "ws");
}

std::string handle_barrier_ack(const std::string& opid, const std::string& node,
                              std::string st, json_object* lanes,
                              std::set<std::string>& missing_out) {
    if (st.empty()) st = "ready";
    if (st != "ready") st = "blocked";
    if (lanes && json_object_is_type(lanes, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(lanes); i++) {
            json_object* e = json_object_array_get_idx(lanes, i);
            json_object* rv = nullptr;
            bool ready = true;
            if (e && json_object_object_get_ex(e, "ready", &rv) && rv)
                ready = json_object_get_boolean(rv) != 0;
            if (!ready) { st = "blocked"; break; }
        }
    }
    std::string state;
    {
        std::lock_guard<std::mutex> lk(g_ops_mu);
        state = g_ops.on_reply(opid, node, st != "blocked", st);
        missing_out = g_ops.missing(opid);
    }
    g_store->audit("{\"commit\":\"" + node + "\",\"state\":\"" + st + "\"}");
    return state;
}

bool ws_push(const std::string& node, const std::string& type, json_object* data) {
    int fd = g_chan.fd_of(node);
    if (fd < 0) return false;
    json_object* msg = ct::wrap(ct::next_msg_id("p"), type, data,
                               ct::next_request_id("p"));
    const char* s = json_object_to_json_string(msg);
    std::string frame = s ? s : "{}";
    json_object_put(msg);
    std::lock_guard<std::mutex> lk(g_chan.send_mu);
    return HttpServer::ws_send_text(fd, frame, false);
}

static std::string ws_idem_key(const std::string& msg_id, const std::string& tp, json_object* data) {
    std::string node;
    if (data) {
        json_object* v = nullptr;
        if (json_object_object_get_ex(data, "node", &v) && v &&
            json_object_is_type(v, json_type_string)) node = json_object_get_string(v);
        else if (json_object_object_get_ex(data, "id", &v) && v &&
                 json_object_is_type(v, json_type_string)) node = json_object_get_string(v);
        else if (json_object_object_get_ex(data, "nodeId", &v) && v &&
                 json_object_is_type(v, json_type_string)) node = json_object_get_string(v);
        else if (json_object_object_get_ex(data, "sn", &v) && v &&
                 json_object_is_type(v, json_type_string)) node = json_object_get_string(v);
    }
    return msg_id + "#" + tp + "#" + node;
}

void ws_channel_loop(int fd, HttpRequest hs_req) {
    std::string hs_bearer = ct::extract_bearer(hs_req.headers);
    auto send_wrapped = [&](const std::string& idem_key, const std::string& type,
                            json_object* data) -> std::string {
        std::string hit;
        if (!idem_key.empty() && g_chan.idem_hit(idem_key, hit)) {
            std::lock_guard<std::mutex> lk(g_chan.send_mu);
            HttpServer::ws_send_text(fd, hit, false);
            return hit;
        }
        json_object* msg = ct::wrap(ct::next_msg_id("p"), type, data,
                                   ct::next_request_id("p"));
        const char* s = json_object_to_json_string(msg);
        std::string frame = s ? s : "{}";
        json_object_put(msg);
        {
            std::lock_guard<std::mutex> lk(g_chan.send_mu);
            HttpServer::ws_send_text(fd, frame, false);
        }
        if (!idem_key.empty()) g_chan.idem_store(idem_key, frame);
        return frame;
    };
    std::string first;
    if (!HttpServer::ws_recv_text(fd, first, 5000, false)) { ::close(fd); return; }
    json_object* jm = json_tokener_parse(first.c_str());
    std::string msg_id, type, req_id;
    json_object* data = nullptr;
    if (!jm || !ct::unwrap(jm, msg_id, type, data, req_id) || type != ct::kHello) {
        if (jm) json_object_put(jm);
        if (data) json_object_put(data);
        ::close(fd);
        return;
    }
    json_object* tokv = nullptr;
    std::string hello_tok;
    if (json_object_object_get_ex(data, "token", &tokv) && tokv &&
        json_object_is_type(tokv, json_type_string))
        hello_tok = json_object_get_string(tokv);
    std::string provided = !hello_tok.empty() ? hello_tok : hs_bearer;
    std::string verr = ct::validate_hello(data);
    bool auth = ct::bearer_ok(provided, g_api_token);
    std::string node_id = body_str(data, "id");
    if (node_id.empty()) node_id = body_str(data, "sn");
    if (!verr.empty() || !auth || node_id.empty() ||
        g_store->tombstone_check(node_id, now_sec())) {
        json_object* d = json_object_new_object();
        json_object_object_add(d, "reason", json_object_new_string(
            !auth ? "missing or bad token" : (!verr.empty() ? verr.c_str() : "missing id")));
        send_wrapped(ws_idem_key(msg_id, type, data), ct::kError, d);
        if (data) json_object_put(data);
        json_object_put(jm);
        ::close(fd);
        return;
    }
    g_chan.register_conn(node_id, fd);
    apply_hello_doc(data);
    g_store->audit("{\"ws-hello\":\"" + node_id + "\"}");
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "heartbeatIntervalMs",
                               json_object_new_int(ct::heartbeat_interval_ms()));
        json_object_object_add(w, "barrierTimeoutMs",
                               json_object_new_int(ct::barrier_timeout_ms()));
        json_object_object_add(w, "nodeId", json_object_new_string(node_id.c_str()));
        send_wrapped(ws_idem_key(msg_id, type, data), ct::kWelcome, w);
    }
    if (data) json_object_put(data);
    json_object_put(jm);
    int hb_ms = ct::heartbeat_interval_ms();
    int idle_miss = 0;
    for (;;) {
        std::string frame;
        if (!HttpServer::ws_recv_text(fd, frame, hb_ms + 2000, false)) {
            if (++idle_miss >= 3) break;
            std::lock_guard<std::mutex> lk(g_chan.send_mu);
            if (!HttpServer::ws_send_ping(fd, "", false)) break;
            continue;
        }
        idle_miss = 0;
        json_object* m = json_tokener_parse(frame.c_str());
        std::string mid, tp, rid;
        json_object* dd = nullptr;
        if (!m || !ct::unwrap(m, mid, tp, dd, rid)) {
            if (m) json_object_put(m);
            if (dd) json_object_put(dd);
            continue;
        }
        if (tp == ct::kHeartbeat) {
            std::string e = ct::validate_heartbeat(dd);
            std::string hid = body_str(dd, "id");
            if (hid.empty()) hid = body_str(dd, "nodeId");
            if (!e.empty() || hid.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason", json_object_new_string(
                    e.empty() ? "bad heartbeat" : e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                apply_heartbeat_doc(hid, dd);
                json_object* ack = json_object_new_object();
                json_object_object_add(ack, "revision", json_object_new_int(1));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kBarrierAck) {
            std::string e = ct::validate_barrier_ack(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason", json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                std::string opid = body_str(dd, "operationId");
                std::string node = body_str(dd, "node");
                std::string st = body_str(dd, "status");
                json_object* lanes = nullptr;
                json_object_object_get_ex(dd, "lanes", &lanes);
                std::set<std::string> miss;
                std::string state = handle_barrier_ack(opid, node, st,
                    (lanes && json_object_is_type(lanes, json_type_array)) ? lanes : nullptr, miss);
                const Operation* op = nullptr;
                { std::lock_guard<std::mutex> lk(g_ops_mu); op = g_ops.find(opid); }
                json_object* ack = json_object_new_object();
                if (!op) {
                    json_object_object_add(ack, "state", json_object_new_string("unknown"));
                } else if (state == "blocked") {
                    int to_rev = 0;
                    std::string rb;
                    { std::lock_guard<std::mutex> lk(g_ops_mu); rb = g_ops.rollback_op(opid, &to_rev); }
                    json_object_object_add(ack, "rollback", json_object_new_string(rb.c_str()));
                    json_object_object_add(ack, "toRevision", json_object_new_int(to_rev));
                } else {
                    json_object_object_add(ack, "state", json_object_new_string(state.c_str()));
                    json_object* ma = json_object_new_array();
                    for (auto& mm : miss) json_object_array_add(ma, json_object_new_string(mm.c_str()));
                    json_object_object_add(ack, "missing", ma);
                }
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kUpgradeProgress) {
            std::string e = ct::validate_upgrade_progress(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason", json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                std::string tid = body_str(dd, "taskId");
                std::string node = body_str(dd, "node");
                std::string st = body_str(dd, "status");
                { std::lock_guard<std::mutex> lk(g_chan.mu); g_chan.upgrade_progress[tid + "/" + node] = st; }
                g_store->audit("{\"upgrade-progress\":\"" + node + "\",\"task\":\"" + tid + "\",\"status\":\"" + st + "\"}");
                json_object* ack = json_object_new_object();
                json_object_object_add(ack, "received", json_object_new_boolean(1));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kConfigAck) {
            std::string e = ct::validate_config_ack(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason", json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                g_store->audit("{\"config-ack\":\"" + body_str(dd, "node") + "\"}");
                json_object* ack = json_object_new_object();
                json_object_object_add(ack, "received", json_object_new_boolean(1));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "reason", json_object_new_string("unknown type"));
            send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
        }
        if (dd) json_object_put(dd);
        if (m) json_object_put(m);
    }
    std::string gone = g_chan.node_of(fd);
    g_chan.unregister_fd(fd);
    if (!gone.empty()) {
        json_object* cur = g_store->get_doc("node", gone);
        if (cur) {
            json_object_object_add(cur, "online", json_object_new_boolean(0));
            g_store->put_doc("node", gone, cur);
            json_object_put(cur);
        }
        g_store->audit("{\"ws-offline\":\"" + gone + "\"}");
    }
    ::close(fd);
}
} // namespace

int main(int argc, char** argv) {
    std::string db = "/tmp/dvs_platform.db";
    int port = 8081;
    std::string web_root_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--web-root" && i + 1 < argc) web_root_arg = argv[++i];
    }
    g_store = new PlatformStore(db);

    HttpServer::Handler h = [](const HttpRequest& req, HttpResponse& res) {
        if (req.method == "POST" && req.path == "/api/v1/nodes/register") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string id = body_str(b, "id");
            if (id.empty()) id = body_str(b, "sn");
            if (id.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "missing id"));
                return;
            }
            g_store->tombstone_gc(now_sec());
            if (g_store->tombstone_check(id, now_sec())) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("RETIRED", 410, "node retired"));
                return;
            }
            if (!b) b = json_object_new_object();
            bool has_base = !body_str(b, "agentBase").empty();
            json_object* geo0 = nullptr;
            bool has_geo = b && json_object_object_get_ex(b, "geometry", &geo0) && geo0;
            json_object_object_add(b, "online", json_object_new_boolean(1));
            json_object_object_add(b, "lastHeartbeat",
                                   json_object_new_string(now_iso().c_str()));
            json_object_object_add(b, "transport", json_object_new_string("short"));
            g_store->put_doc("node", id, b);
            g_chan.set_transport(id, "short");
            g_store->audit("{\"register\":\"" + id + "\"}");
            if (!has_base || !has_geo)
                g_store->audit("{\"incomplete-register\":\"" + id + "\"}");
            json_object* d = g_store->get_doc("node", id);
            json_object_put(b);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string id = body_str(b, "id");
            if (id.empty()) id = body_str(b, "nodeId");
            json_object* cur = id.empty() ? nullptr : g_store->get_doc("node", id);
            if (!cur) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown"));
                return;
            }
            json_object_object_add(cur, "online", json_object_new_boolean(1));
            json_object_object_add(cur, "lastHeartbeat",
                                   json_object_new_string(now_iso().c_str()));
            json_object* m = nullptr;
            if (b && json_object_object_get_ex(b, "metrics", &m) && m)
                json_object_object_add(cur, "metrics", json_object_get(m));
            json_object* ld = nullptr;
            if (b && json_object_object_get_ex(b, "lanesDigest", &ld) && ld) {
                json_object* curm = nullptr;
                if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                    curm = json_object_new_object();
                    json_object_object_add(cur, "metrics", curm);
                }
                json_object_object_del(curm, "lanesDigest");
                json_object_object_add(curm, "lanesDigest", json_object_get(ld));
            }
            /* Additive digests: TX session summary and the SIP probe
             * summary. Stored verbatim, never branched on. */
            json_object* txd = nullptr;
            if (b && json_object_object_get_ex(b, "txDigest", &txd) && txd) {
                json_object* curm = nullptr;
                if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                    curm = json_object_new_object();
                    json_object_object_add(cur, "metrics", curm);
                }
                json_object_object_del(curm, "txDigest");
                json_object_object_add(curm, "txDigest", json_object_get(txd));
            }
            json_object* sipd = nullptr;
            if (b && json_object_object_get_ex(b, "sip", &sipd) && sipd) {
                json_object_object_del(cur, "sipDigest");
                json_object_object_add(cur, "sipDigest", json_object_get(sipd));
            }
            json_object_object_add(cur, "transport", json_object_new_string("short"));
            g_store->put_doc("node", id, cur);
            g_chan.set_transport(id, "short");
            json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "revision", json_object_new_int(1));
            send_json(res, ok_envelope(d, ""));
            json_object_put(cur);
            return;
        }
        if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
            size_t sl = rest.find('/');
            std::string id = (sl == std::string::npos) ? rest : rest.substr(0, sl);
            std::string act = (sl == std::string::npos) ? "" : rest.substr(sl + 1);
            if (req.method == "GET" && act.empty()) {
                json_object* d = g_store->get_doc("node", id);
                if (!d) { send_json(res, fail_envelope("NOT_FOUND", 404, "not found")); return; }
                send_json(res, ok_envelope(d, ""));
                return;
            }
            if (req.method == "PUT" && act.empty()) {
                json_object* cur = g_store->get_doc("node", id);
                if (!cur) { send_json(res, fail_envelope("NOT_FOUND", 404, "not found")); return; }
                json_object* b = json_tokener_parse(req.body.c_str());
                std::string nm = body_str(b, "name");
                if (b) json_object_put(b);
                if (nm.empty()) { send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "missing name")); json_object_put(cur); return; }
                json_object_object_del(cur, "name");
                json_object_object_add(cur, "name", json_object_new_string(nm.c_str()));
                g_store->put_doc("node", id, cur);
                json_object_put(cur);
                g_store->audit("{\"rename\":\"" + id + "\"}");
                json_object* d = g_store->get_doc("node", id);
                send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
                return;
            }
            if (req.method == "POST" && act == "heartbeat") {
                json_object* cur = g_store->get_doc("node", id);
                if (!cur) { send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown")); return; }
                json_object* body2 = json_tokener_parse(req.body.c_str());
                json_object_object_del(cur, "online");
                json_object_object_add(cur, "online", json_object_new_boolean(1));
                json_object_object_del(cur, "lastHeartbeat");
                json_object_object_add(cur, "lastHeartbeat",
                                       json_object_new_string(now_iso().c_str()));
                json_object* m = nullptr;
                if (body2 && json_object_object_get_ex(body2, "metrics", &m) && m)
                    json_object_object_add(cur, "metrics", json_object_get(m));
                json_object* ld = nullptr;
                if (body2 && json_object_object_get_ex(body2, "lanesDigest", &ld) && ld) {
                    json_object* curm = nullptr;
                    if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                        curm = json_object_new_object();
                        json_object_object_add(cur, "metrics", curm);
                    }
                    json_object_object_del(curm, "lanesDigest");
                    json_object_object_add(curm, "lanesDigest", json_object_get(ld));
                }
                json_object* txd = nullptr;
                if (body2 && json_object_object_get_ex(body2, "txDigest", &txd) && txd) {
                    json_object* curm = nullptr;
                    if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                        curm = json_object_new_object();
                        json_object_object_add(cur, "metrics", curm);
                    }
                    json_object_object_del(curm, "txDigest");
                    json_object_object_add(curm, "txDigest", json_object_get(txd));
                }
                json_object* sipd = nullptr;
                if (body2 && json_object_object_get_ex(body2, "sip", &sipd) && sipd) {
                    json_object_object_del(cur, "sipDigest");
                    json_object_object_add(cur, "sipDigest", json_object_get(sipd));
                }
                g_store->put_doc("node", id, cur);
                if (body2) json_object_put(body2);
                json_object_put(cur);
                json_object* d = g_store->get_doc("node", id);
                send_json(res, ok_envelope(d ? d : json_object_new_object(), "heartbeat received"));
                return;
            }
            if (req.method == "GET" && act == "current-task") {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "task", json_object_new_null());
                send_json(res, ok_envelope(d, ""));
                return;
            }
            if (req.method == "POST" && act == "retire") {
                g_store->tombstone_add(id, now_sec());
                g_store->del_doc("node", id);
                g_store->audit("{\"retire\":\"" + id + "\"}");
                send_json(res, ok_envelope(json_object_new_object(), "retired"));
                return;
            }
            // Push the platform-owned config subset to the node. Only geometry
            // and role leave the platform; every other key is dropped and
            // audited so node-local scopes stay authoritative.
            if (req.method == "POST" && act == "config-push") {
                json_object* cur = g_store->get_doc("node", id);
                if (!cur) { send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown")); return; }
                std::string base = body_str(cur, "agentBase");
                json_object* body2 = json_tokener_parse(req.body.c_str());
                json_object* cfg = nullptr;
                if (body2) json_object_object_get_ex(body2, "config", &cfg);
                json_object* d = json_object_new_object();
                json_object* ignored = json_object_new_array();
                json_object* fwd = json_object_new_object();
                if (cfg && json_object_is_type(cfg, json_type_object)) {
                    json_object_object_foreach(cfg, key, val) {
                        std::string k(key);
                        if (k == "geometry" || k == "role")
                            json_object_object_add(fwd, k.c_str(), json_object_get(val));
                        else {
                            json_object_array_add(ignored, json_object_new_string(k.c_str()));
                            g_store->audit("{\"config-push-ignored\":\"" + k + "\"}");
                        }
                    }
                }
                json_object_object_add(d, "ignored", ignored);
                if (!json_object_object_length(fwd)) {
                    json_object_put(fwd);
                    json_object_object_add(d, "status", json_object_new_string("skipped"));
                } else {
                    json_object* sp = json_object_new_object();
                    json_object_object_add(sp, "config", json_object_get(fwd));
                    json_object_put(fwd);
                    std::string verr = ct::validate_config_update(sp);
                    bool via_ws = false;
                    if (verr.empty()) via_ws = ws_push(id, ct::kConfigUpdate, json_object_get(sp));
                    if (via_ws) {
                        json_object_object_add(d, "status", json_object_new_string("pushed"));
                        json_object_object_add(d, "via", json_object_new_string("ws"));
                        json_object_put(sp);
                    } else if (!base.empty()) {
                        HttpClient cli;
                        const char* spb = json_object_to_json_string(sp);
                        std::string resp;
                        int sc = cli.post_json(base + "/api/v1/node/config/sync",
                                               std::string(spb ? spb : "{}"), resp, 5000, 2, g_api_token);
                        json_object_object_add(d, "status", json_object_new_string(sc == 200 ? "pushed" : "failed"));
                        json_object_object_add(d, "via", json_object_new_string("short"));
                        json_object_put(sp);
                    } else {
                        json_object_object_add(d, "status", json_object_new_string("skipped"));
                        json_object_put(sp);
                    }
                }
                if (body2) json_object_put(body2);
                json_object_put(cur);
                send_json(res, ok_envelope(d, "config-push"));
                return;
            }
        }
        if (req.method == "GET" && req.path == "/api/v1/nodes") {
            json_object* arr = json_object_new_array();
            for (auto& i : g_store->list_ids("node")) {
                json_object* d = g_store->get_doc("node", i);
                if (d) json_object_array_add(arr, d);
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", arr);
            send_json(res, ok_envelope(d, ""));
            return;
        }
        // Prepare registers the barrier and returns at once; per-node
        // sub-layout delivery runs in a detached worker so the handler
        // never blocks on node calls. Geometry comes only from stored
        // node docs, never from the prepare request.
        if (req.method == "POST" && req.path == "/api/v1/ops/prepare") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string opid = body_str(b, "operationId");
            int rev = 1;
            json_object* v = nullptr;
            if (b && json_object_object_get_ex(b, "revision", &v)) rev = json_object_get_int(v);
            double fps = 0;
            if (b && json_object_object_get_ex(b, "fps", &v)) fps = json_object_get_double(v);
            std::set<std::string> members;
            json_object* arr = nullptr;
            if (b && json_object_object_get_ex(b, "members", &arr) &&
                json_object_is_type(arr, json_type_array)) {
                for (size_t i = 0; i < json_object_array_length(arr); i++)
                    members.insert(json_object_get_string(json_object_array_get_idx(arr, i)));
            }
            if (opid.empty() || members.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "need operationId+members"));
                return;
            }
            uint64_t apply_ns = layout_apply_at_ns(now_ns(), fps);
            uint64_t deadline = now_ns() + (uint64_t)ct::barrier_timeout_ms() * 1000000ULL;
            std::string op_id_copy; uint64_t apply_copy = 0;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                Operation& op = g_ops.begin_with_deadline(opid, rev, members, apply_ns, deadline);
                op_id_copy = op.op_id; apply_copy = op.apply_at_ns;
            }

            json_object* wall = nullptr;
            if (b) json_object_object_get_ex(b, "wall", &wall);
            json_object* windows = nullptr;
            if (b) json_object_object_get_ex(b, "windows", &windows);
            int wall_w = rect_int(wall, "w"), wall_h = rect_int(wall, "h");
            const char* win_s = (windows && json_object_is_type(windows, json_type_array))
                ? json_object_to_json_string(windows) : "[]";
            std::string windows_json = win_s ? win_s : "[]";
            uint64_t apply_at = apply_copy;
            std::string opid_c = op_id_copy;
            std::string token_c = g_api_token;
            if (members.size() > 32)
                g_store->audit("{\"fanout-scale\":\"" + opid_c + "\",\"members\":" +
                               std::to_string(members.size()) + "}");
            json_object* missing = json_object_new_array();
            for (const auto& mid : members) {
                json_object* nd = g_store->get_doc("node", mid);
                std::string ab = nd ? body_str(nd, "agentBase") : "";
                json_object* geo = nullptr;
                if (nd) json_object_object_get_ex(nd, "geometry", &geo);
                bool geo_ok = geo && rect_int(geo, "w") > 0 && rect_int(geo, "h") > 0;
                bool has_chan = (g_chan.fd_of(mid) >= 0) || !ab.empty();
                if (!nd || !geo_ok || !has_chan) {
                    json_object_array_add(missing, json_object_new_string(mid.c_str()));
                    g_store->audit("{\"fanout-skip\":\"" + mid +
                                   "\",\"reason\":\"no channel/geometry\"}");
                }
                if (nd) json_object_put(nd);
            }
            std::thread([opid_c, rev, apply_at, members, windows_json, wall_w, wall_h,
                           token_c]() {
                json_object* wins = json_tokener_parse(windows_json.c_str());
                HttpClient cli;
                for (const auto& mid : members) {
                    json_object* nd = g_store->get_doc("node", mid);
                    if (!nd) continue;
                    std::string agent_base = body_str(nd, "agentBase");
                    json_object* geo = nullptr;
                    json_object_object_get_ex(nd, "geometry", &geo);
                    bool geo_ok = geo && rect_int(geo, "w") > 0 && rect_int(geo, "h") > 0;
                    if (!geo_ok) { json_object_put(nd); continue; }
                    Rect node{rect_int(geo, "x"), rect_int(geo, "y"),
                              rect_int(geo, "w"), rect_int(geo, "h")};
                    json_object* frag = fragment_for_node(wins, node, wall_w, wall_h);
                    json_object* payload = json_object_new_object();
                    json_object_object_add(payload, "operationId",
                                           json_object_new_string(opid_c.c_str()));
                    json_object_object_add(payload, "revision", json_object_new_int(rev));
                    json_object_object_add(payload, "applyAtNs",
                                           json_object_new_int64((int64_t)apply_at));
                    json_object_object_add(payload, "geometry", json_object_get(geo));
                    json_object_object_add(payload, "windows", frag);
                    std::string verr = ct::validate_layout_prepare(payload);
                    bool via_ws = false;
                    if (verr.empty()) via_ws = ws_push(mid, ct::kLayoutPrepare, json_object_get(payload));
                    if (via_ws) {
                        g_store->audit("{\"fanout\":\"" + mid + "\",\"via\":\"ws\"}");
                        g_chan.set_transport(mid, "ws");
                        json_object_put(payload);
                    } else if (!agent_base.empty()) {
                        const char* pb = json_object_to_json_string(payload);
                        std::string resp;
                        int sc = cli.post_json(agent_base + "/api/v1/node/media/layout",
                                               std::string(pb ? pb : "{}"), resp, 5000, 2, token_c);
                        g_store->audit("{\"fanout\":\"" + mid + "\",\"status\":" + std::to_string(sc) + "}");
                        g_chan.set_transport(mid, "short");
                        json_object_put(payload);
                    } else {
                        g_store->audit("{\"fanout-skip\":\"" + mid + "\",\"reason\":\"no channel\"}");
                        json_object_put(payload);
                    }
                    json_object_put(nd);
                }
                if (wins) json_object_put(wins);
            }).detach();

            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "operationId", json_object_new_string(opid_c.c_str()));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64((int64_t)apply_at));
            json_object_object_add(d, "state", json_object_new_string("preparing"));
            json_object_object_add(d, "missing", missing);
            send_json(res, ok_envelope(d, "preparing"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/ops/commit") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string opid = body_str(b, "operationId");
            std::string node = body_str(b, "node");
            std::string st = body_str(b, "status");
            json_object* lanes = nullptr;
            if (b) json_object_object_get_ex(b, "lanes", &lanes);
            std::set<std::string> still_missing;
            std::string state = handle_barrier_ack(opid, node, st, lanes, still_missing);
            uint64_t apply_ns = 0;
            bool known = false;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                const Operation* op = g_ops.find(opid);
                if (op) { apply_ns = op->apply_at_ns; known = true; }
            }
            if (b) json_object_put(b);
            if (!known) { send_json(res, fail_envelope("NOT_FOUND", 404, "op unknown")); return; }
            if (state == "blocked") {
                int to_rev = 0;
                std::string rb;
                { std::lock_guard<std::mutex> lk(g_ops_mu); rb = g_ops.rollback_op(opid, &to_rev); }
                json_object* d = json_object_new_object();
                json_object_object_add(d, "rollback", json_object_new_string(rb.c_str()));
                json_object_object_add(d, "toRevision", json_object_new_int(to_rev));
                send_json(res, ok_envelope(d, "rolled back"));
                return;
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "state", json_object_new_string(state.c_str()));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64((int64_t)apply_ns));
            json_object* miss = json_object_new_array();
            for (const auto& m : still_missing)
                json_object_array_add(miss, json_object_new_string(m.c_str()));
            json_object_object_add(d, "missing", miss);
            send_json(res, ok_envelope(d, state));
            return;
        }
        // 升级改为任务队列驱动：handler 只入队，worker 串行经 WS/短连接投递。
        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            json_object* b = json_tokener_parse(req.body.c_str());
            json_object* arr = nullptr;
            if (b) json_object_object_get_ex(b, "targetNodeIds", &arr);
            std::string fn = body_str(b, "filename");
            if (fn.empty()) fn = "fw.bin";
            std::string sha = body_str(b, "sha256");
            std::vector<std::string> nodes;
            if (arr && json_object_is_type(arr, json_type_array)) {
                for (size_t i = 0; i < json_object_array_length(arr); i++)
                    nodes.push_back(json_object_get_string(json_object_array_get_idx(arr, i)));
            }
            if (b) json_object_put(b);
            std::string task_id = ct::next_msg_id("p");
            {
                std::lock_guard<std::mutex> lk(g_up_mu);
                g_up_q.push(UpgradeJob{task_id, nodes, fn, sha});
            }
            g_up_cv.notify_one();
            g_store->audit("{\"upgrade-enqueue\":\"" + task_id + "\"}");
            json_object* d = json_object_new_object();
            json_object_object_add(d, "taskId", json_object_new_string(task_id.c_str()));
            json_object_object_add(d, "queued", json_object_new_int((int)nodes.size()));
            send_json(res, ok_envelope(d, "queued"));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/audit-events") {
            send_json(res, ok_envelope(g_store->audit_list(100), ""));
            return;
        }
        const char* kinds[] = {"groups", "walls", "scenes", "bindings",
                               "whitelist", "playback-tasks"};
        for (auto k : kinds)
            if (route_docs(k, req, res)) return;
        // static files
        if (req.method == "GET") {
            std::string path = req.path;
            if (path == "/" || path.empty()) path = "/index.html";
            std::string full = g_web_root + path;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::ifstream f(full, std::ios::binary);
                std::ostringstream ss; ss << f.rdbuf();
                res.status = 200;
                res.body = ss.str();
                // guess content type
                if (full.find(".html") != std::string::npos) res.content_type = "text/html; charset=utf-8";
                else if (full.find(".css") != std::string::npos) res.content_type = "text/css; charset=utf-8";
                else if (full.find(".js") != std::string::npos) res.content_type = "text/javascript; charset=utf-8";
                else if (full.find(".svg") != std::string::npos) res.content_type = "image/svg+xml";
                else res.content_type = "application/octet-stream";
                return;
            }
        }
        send_json(res, fail_envelope("NOT_FOUND", 404, "unknown api"));
    };

    if (!web_root_arg.empty()) g_web_root = web_root_arg;
    if (g_web_root.empty()) {
        const char* env = getenv("DVS_WEB_ROOT");
        if (env && *env) g_web_root = env;
    }
    if (g_web_root.empty()) g_web_root = "./web/src";
    HttpServer server(port, h);
    server.set_ws_handler("/api/v1/channel/ws", ws_channel_loop);
    // 屏障超时回收：超时未齐按阻塞处理并回滚，避免永久停留 preparing。
    std::thread([]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::vector<std::string> expired;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                expired = g_ops.expire_sweep(now_ns());
            }
            for (auto& id : expired) {
                int to_rev = 0;
                std::string rb;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    rb = g_ops.rollback_op(id, &to_rev);
                }
                g_store->audit("{\"barrier-timeout\":\"" + id + "\"}");
            }
        }
    }).detach();
    // 升级任务队列 worker：逐节点串行，失败隔离到单节点。
    std::thread([]() {
        HttpClient cli;
        for (;;) {
            UpgradeJob job;
            {
                std::unique_lock<std::mutex> lk(g_up_mu);
                g_up_cv.wait(lk, []() { return !g_up_q.empty(); });
                job = g_up_q.front();
                g_up_q.pop();
            }
            for (auto& nid : job.nodes) {
                json_object* task = json_object_new_object();
                json_object_object_add(task, "taskId", json_object_new_string(job.task_id.c_str()));
                json_object_object_add(task, "filename", json_object_new_string(job.filename.c_str()));
                if (!job.sha256.empty())
                    json_object_object_add(task, "sha256", json_object_new_string(job.sha256.c_str()));
                bool via_ws = false;
                if (ct::validate_upgrade_task(task).empty())
                    via_ws = ws_push(nid, ct::kUpgradeTask, json_object_get(task));
                if (via_ws) {
                    g_store->audit("{\"upgrade-push\":\"" + nid + "\",\"via\":\"ws\"}");
                    json_object_put(task);
                    continue;
                }
                json_object* nd = g_store->get_doc("node", nid);
                std::string base = nd ? body_str(nd, "agentBase") : "";
                if (nd) json_object_put(nd);
                if (base.empty()) {
                    g_store->audit("{\"upgrade-skip\":\"" + nid + "\"}");
                    json_object_put(task);
                    continue;
                }
                const char* tb = json_object_to_json_string(task);
                std::string body = tb ? tb : "{}";
                json_object_put(task);
                std::string resp;
                int sc = cli.post_json(base + "/api/v1/node/upgrade", body, resp, 10000, 2, g_api_token);
                g_store->audit("{\"upgrade-push\":\"" + nid + "\",\"status\":" + std::to_string(sc) + "}");
            }
        }
    }).detach();
    server.start();
    printf("dvs-platform listening on 0.0.0.0:%d\n", port);
    fflush(stdout);
    for (;;) pause();
    return 0;
}
