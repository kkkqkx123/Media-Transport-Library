#include "registration.h"

#include "dvs_contract.h"
#include "http_server.h"
#include "node_store.h"
#include "media_ipc.h"
#include "http_client.h"

#include <json-c/json.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include "dvs_http_json.h"

static std::string str_of(json_object* o, const char* key, const char* def = "") {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return def;
}

static int int_of(json_object* o, const char* key, int def = 0) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_int(v);
    return def;
}

static std::string control_token();

static std::string iso_now() {
    char buf[32];
    time_t t = time(nullptr);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    return buf;
}

/* Read a single integer from a sysfs/proc file. Returns def on any failure. */
static long read_long_file(const char* path, long def) {
    std::ifstream f(path);
    if (!f) return def;
    long v = def;
    f >> v;
    return v;
}

Registration::Registration(NodeStore* store, MediaIpcClient* media,
                           const std::string& platform_url, int interval_ms,
                           const std::string& agent_base)
    : store_(store), media_(media), platform_url_(platform_url),
      agent_base_(agent_base), interval_ms_(interval_ms),
      register_state_("unregistered"),
      start_time_(time(nullptr)) {}

Registration::~Registration() { stop(); }

void Registration::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&Registration::loop, this);
}

void Registration::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Registration::set_downlink_handler(DownlinkHandler h) {
    std::lock_guard<std::mutex> lk(dl_mu_);
    downlink_ = h;
}

std::string Registration::register_state() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return register_state_;
}

std::string Registration::transport() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return transport_;
}

std::string Registration::ws_channel_url() const {
    std::string base = platform_url_;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/api/v1/channel/ws";
}

void Registration::loop() {
    if (platform_url_.empty()) {
        register_state_ = "unregistered";
        online_ = true;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            json_object* patch = json_object_new_object();
            json_object_object_add(patch, "lastHeartbeat", json_object_new_string(iso_now().c_str()));
            store_->update_node(patch);
            json_object_put(patch);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        register_state_ = "registering";
    }
    int backoff = 2;
    std::random_device rd;
    std::mt19937 rng(rd());
    while (running_) {
        namespace ct = dvs::contract;
        bool want_ws = ct::use_ws();
        bool ws_only = ct::ws_only();
        if (want_ws) {
            bool ok = ws_loop_once();
            if (ok) { backoff = 2; continue; }
            // WS 断开：auto 模式回退一次短连接心跳，ws 模式直接退避重拨。
            if (!ws_only) {
                std::string rs;
                { std::lock_guard<std::mutex> lk(state_mu_); rs = register_state_; }
                if (rs != "registered") register_once();
                else heartbeat_once();
            }
        } else {
            std::string rs;
            { std::lock_guard<std::mutex> lk(state_mu_); rs = register_state_; }
            if (rs != "registered") {
                if (register_once()) {
                    std::lock_guard<std::mutex> lk(state_mu_);
                    register_state_ = "registered";
                    backoff = 2;
                }
                else {
                    std::this_thread::sleep_for(std::chrono::seconds(backoff));
                    if (backoff < 30) backoff *= 2;
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
            if (!heartbeat_once()) {
                fail_streak_++;
                if (fail_streak_ >= 3) {
                    online_ = false;
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "online", json_object_new_boolean(0));
                    store_->update_node(patch);
                    json_object_put(patch);
                }
            } else {
                if (!online_) {
                    online_ = true;
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "online", json_object_new_boolean(1));
                    store_->update_node(patch);
                    json_object_put(patch);
                }
                fail_streak_ = 0;
            }
            continue;
        }
        int jitter = (int)(rng() % 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff * 1000 + jitter));
        if (backoff < 30) backoff *= 2;
    }
}

bool Registration::ws_send_wrapped(const std::string& type, json_object* data,
                                   std::string* reply_out) {
    namespace ct = dvs::contract;
    (void)reply_out;
    json_object* msg = ct::wrap(ct::next_msg_id("a"), type, data, ct::next_request_id("a"));
    const char* s = json_object_to_json_string(msg);
    std::string frame = s ? s : "{}";
    json_object_put(msg);
    std::lock_guard<std::mutex> lk(ws_mu_);
    if (!ws_.is_open()) return false;
    return ws_.send_text(frame);
}

bool Registration::ws_loop_once() {
    namespace ct = dvs::contract;
    std::string token = control_token();
    {
        std::lock_guard<std::mutex> lk(ws_mu_);
        ws_.close();
        if (!ws_.connect_ws(ws_channel_url(), token, 5000)) return false;
    }
    // 首包 hello：问候携带与短连接注册体相同的档案信息。
    json_object* reg = build_register();
    json_object* node = store_->node();
    json_object* nidv = nullptr;
    std::string nid = "local-node";
    if (node) {
        json_object* iv = nullptr;
        if (json_object_object_get_ex(node, "id", &iv) && iv) nid = json_object_get_string(iv);
    }
    (void)nidv;
    json_object_object_add(reg, "id", json_object_new_string(nid.c_str()));
    if (!token.empty())
        json_object_object_add(reg, "token", json_object_new_string(token.c_str()));
    json_object* hello = ct::wrap(ct::next_msg_id("a"), ct::kHello, reg, ct::next_request_id("a"));
    const char* hs = json_object_to_json_string(hello);
    std::string hframe = hs ? hs : "{}";
    json_object_put(hello);
    {
        std::lock_guard<std::mutex> lk(ws_mu_);
        if (!ws_.send_text(hframe)) return false;
    }
    std::string wframe;
    // 握手等待 welcome：接收不持锁，避免阻塞回执发送。
    // 用临时锁外接收，避免长时间持锁阻塞 uplink。
    if (!ws_.recv_text(wframe, 5000)) { ws_.close(); return false; }
    json_object* wm = json_tokener_parse(wframe.c_str());
    std::string mid, tp, rid;
    json_object* dd = nullptr;
    bool ok = wm && ct::unwrap(wm, mid, tp, dd, rid) && tp == ct::kWelcome;
    if (ok && dd) {
        json_object* iv = nullptr;
        if (json_object_object_get_ex(dd, "heartbeatIntervalMs", &iv) && iv) {
            int v = json_object_get_int(iv);
            if (v > 0) interval_ms_ = v;
        }
    }
    if (dd) json_object_put(dd);
    if (wm) json_object_put(wm);
    if (!ok) { ws_.close(); return false; }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        register_state_ = "registered";
        transport_ = "ws";
    }
    online_ = true;
    fail_streak_ = 0;
    {
        json_object* patch = json_object_new_object();
        json_object_object_add(patch, "registerState", json_object_new_string("registered"));
        json_object_object_add(patch, "online", json_object_new_boolean(1));
        store_->update_node(patch);
        json_object_put(patch);
    }
    auto last_hb = std::chrono::steady_clock::now();
    while (running_ && ws_.is_open()) {
        std::string frame;
        auto t0 = std::chrono::steady_clock::now();
        // 以心跳周期为接收超时：超时即补一次 WS 心跳。
        bool got = ws_.recv_text(frame, interval_ms_ + 1000);
        auto now = std::chrono::steady_clock::now();
        auto since_t0 =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        if (!got) {
            // 耗时不足半个周期即返回的失败视为传输错误，直接断开；
            // 达到超时的失败则补心跳，成功继续，失败断开。
            if (since_t0 < (interval_ms_ / 2)) break;
            json_object* hb = build_heartbeat();
            if (!ws_send_wrapped(ct::kHeartbeat, hb)) break;
            last_hb = now;
            continue;
        }
        json_object* m = json_tokener_parse(frame.c_str());
        std::string in_mid, in_tp, in_rid;
        json_object* in_dd = nullptr;
        if (!m || !ct::unwrap(m, in_mid, in_tp, in_dd, in_rid)) {
            if (m) json_object_put(m);
            if (in_dd) json_object_put(in_dd);
            continue;
        }
        if (in_tp == ct::kAck || in_tp == ct::kError || in_tp == ct::kWelcome) {
            // 服务端确认，无需处理。
        } else if (in_tp == ct::kLayoutPrepare || in_tp == ct::kConfigUpdate ||
                   in_tp == ct::kUpgradeTask) {
            DownlinkHandler h;
            { std::lock_guard<std::mutex> lk(dl_mu_); h = downlink_; }
            if (h) h(in_tp, in_dd, in_mid);
        }
        if (in_dd) json_object_put(in_dd);
        if (m) json_object_put(m);
        auto since_hb =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb).count();
        if (since_hb >= interval_ms_) {
            json_object* hb = build_heartbeat();
            // 心跳 data 复用短连接载荷，WS 侧由平台按契约校验。
            ws_send_wrapped(ct::kHeartbeat, hb);
            last_hb = now;
        }
    }
    ws_.close();
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (transport_ == "ws") transport_ = "short";
    }
    online_ = false;
    return register_state() == "registered";
}

static void barrier_frame_fill(json_object* o, const Registration::BarrierFrame& fr) {
    if (!o) return;
    if (fr.has_frame)
        json_object_object_add(o, "frameIndex",
                               json_object_new_int64(fr.frame_index));
    if (fr.has_applied_at)
        json_object_object_add(o, "appliedAtTaiNs",
                               json_object_new_int64(fr.applied_at_ns));
    if (fr.late)
        json_object_object_add(o, "late", json_object_new_boolean(1));
    if (!fr.reason.empty())
        json_object_object_add(o, "reason",
                               json_object_new_string(fr.reason.c_str()));
}

bool Registration::uplink_barrier_ack(const std::string& opid, const std::string& node,
                            const std::string& status, json_object* lanes,
                            const BarrierFrame& frame) {
    namespace ct = dvs::contract;
    json_object* d = json_object_new_object();
    json_object_object_add(d, "operationId", json_object_new_string(opid.c_str()));
    json_object_object_add(d, "node", json_object_new_string(node.c_str()));
    json_object_object_add(d, "status", json_object_new_string(status.c_str()));
    if (lanes) json_object_object_add(d, "lanes", json_object_get(lanes));
    barrier_frame_fill(d, frame);
    if (!ct::validate_barrier_ack(d).empty()) { json_object_put(d); return false; }
    if (ws_.is_open() && ws_send_wrapped(ct::kBarrierAck, d)) return true;
    json_object_put(d);
    if (platform_url_.empty()) return false;
    HttpClient cli;
    json_object* c = json_object_new_object();
    json_object_object_add(c, "operationId", json_object_new_string(opid.c_str()));
    json_object_object_add(c, "node", json_object_new_string(node.c_str()));
    json_object_object_add(c, "status", json_object_new_string(status.c_str()));
    if (lanes) json_object_object_add(c, "lanes", json_object_get(lanes));
    barrier_frame_fill(c, frame);
    const char* cb = json_object_to_json_string(c);
    std::string resp;
    int sc = cli.post_json(platform_url_ + "/api/v1/ops/commit", std::string(cb ? cb : "{}"),
                           resp, 5000, 2, control_token());
    json_object_put(c);
    return sc >= 200 && sc < 400;
}

bool Registration::uplink_config_ack(const std::string& node, int applied_revision) {
    namespace ct = dvs::contract;
    json_object* d = json_object_new_object();
    json_object_object_add(d, "node", json_object_new_string(node.c_str()));
    json_object_object_add(d, "appliedRevision", json_object_new_int(applied_revision));
    if (!ct::validate_config_ack(d).empty()) { json_object_put(d); return false; }
    if (ws_.is_open() && ws_send_wrapped(ct::kConfigAck, d)) return true;
    json_object_put(d);
    if (platform_url_.empty()) return false;
    HttpClient cli;
    json_object* c = json_object_new_object();
    json_object_object_add(c, "node", json_object_new_string(node.c_str()));
    json_object_object_add(c, "appliedRevision", json_object_new_int(applied_revision));
    const char* cb = json_object_to_json_string(c);
    std::string resp;
    int sc = cli.post_json(platform_url_ + "/api/v1/ops/config-ack", std::string(cb ? cb : "{}"),
                           resp, 5000, 2, control_token());
    json_object_put(c);
    return sc >= 200 && sc < 400;
}

bool Registration::uplink_upgrade_progress(const std::string& task_id, const std::string& node,
                                 const std::string& status, const std::string& detail) {
    namespace ct = dvs::contract;
    json_object* d = json_object_new_object();
    json_object_object_add(d, "taskId", json_object_new_string(task_id.c_str()));
    json_object_object_add(d, "node", json_object_new_string(node.c_str()));
    json_object_object_add(d, "status", json_object_new_string(status.c_str()));
    if (!detail.empty())
        json_object_object_add(d, "detail", json_object_new_string(detail.c_str()));
    if (!ct::validate_upgrade_progress(d).empty()) { json_object_put(d); return false; }
    if (ws_.is_open() && ws_send_wrapped(ct::kUpgradeProgress, d)) return true;
    json_object_put(d);
    if (platform_url_.empty()) return false;
    HttpClient cli;
    json_object* c = json_object_new_object();
    json_object_object_add(c, "taskId", json_object_new_string(task_id.c_str()));
    json_object_object_add(c, "node", json_object_new_string(node.c_str()));
    json_object_object_add(c, "status", json_object_new_string(status.c_str()));
    if (!detail.empty())
        json_object_object_add(c, "detail", json_object_new_string(detail.c_str()));
    const char* cb = json_object_to_json_string(c);
    std::string resp;
    int sc = cli.post_json(platform_url_ + "/api/v1/ops/upgrade-progress",
                           std::string(cb ? cb : "{}"), resp, 5000, 2, control_token());
    json_object_put(c);
    return sc >= 200 && sc < 400;
}

static std::string control_token() {
    const char* v = std::getenv("DVS_API_TOKEN");
    return v ? std::string(v) : std::string();
}

bool Registration::register_once() {
    std::string token = control_token();
    HttpClient cli;
    json_object* payload = build_register();
    const char* body = json_object_to_json_string(payload);
    std::string resp;
    int status = cli.post_json(platform_url_ + "/api/v1/nodes/register", body, resp,
                               5000, 2, token);
    json_object_put(payload);
    if (status < 0 || status >= 400) return false;

    json_object* ack = json_tokener_parse(resp.c_str());
    if (!ack) return false;
    json_object* data = nullptr;
    if (json_object_object_get_ex(ack, "data", &data) && data) {
        json_object* nid = nullptr;
        if (json_object_object_get_ex(data, "id", &nid) && nid)
            store_->set_node_id(json_object_get_string(nid));
        json_object* iv = nullptr;
        if (json_object_object_get_ex(data, "heartbeatIntervalMs", &iv) && iv) {
            int ivv = json_object_get_int(iv);
            if (ivv > 0) interval_ms_ = ivv;
        }
    }
    json_object_put(ack);

    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "registerState", json_object_new_string("registered"));
    json_object_object_add(patch, "online", json_object_new_boolean(1));
    store_->update_node(patch);
    json_object_put(patch);
    return true;
}

bool Registration::trigger_once() {
    if (platform_url_.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        register_state_ = "registering";
    }
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "registerState", json_object_new_string("registering"));
    store_->update_node(patch);
    json_object_put(patch);
    bool ok = register_once();
    std::string rs = ok ? "registered" : "failed";
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        register_state_ = rs;
    }
    json_object* p2 = json_object_new_object();
    json_object_object_add(p2, "registerState", json_object_new_string(rs.c_str()));
    store_->update_node(p2);
    json_object_put(p2);
    return ok;
}

void Registration::trigger_async() {
    if (platform_url_.empty()) return;
    std::thread([this]() { trigger_once(); }).detach();
}

bool Registration::heartbeat_once() {
    HttpClient cli;
    json_object* payload = build_heartbeat();
    const char* body = json_object_to_json_string(payload);
    std::string resp;
    int status = cli.post_json(platform_url_ + "/api/v1/nodes/heartbeat", body, resp,
                               interval_ms_ + 1000, 2, control_token());
    json_object_put(payload);
    if (status < 0 || status >= 400) return false;

    /* Reflect appliedRevision from ack if present; keep it simple and just
     * update lastHeartbeat locally. */
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "lastHeartbeat", json_object_new_string(iso_now().c_str()));
    store_->update_node(patch);
    json_object_put(patch);
    return true;
}

json_object* Registration::build_register() {
    json_object* node = store_->node();
    json_object* o = json_object_new_object();
    /* Identity is the operator-assigned node id; sn stays as hardware info
     * merged into the platform node doc, never as an identity fallback. */
    json_object_object_add(o, "id", json_object_new_string(str_of(node, "id", "local-node").c_str()));
    json_object_object_add(o, "sn", json_object_new_string(str_of(node, "sn", "RK3588-LOCAL").c_str()));
    json_object_object_add(o, "mac", json_object_new_string(str_of(node, "mac").c_str()));
    json_object_object_add(o, "model", json_object_new_string(str_of(node, "model", "DVS-NODE-R1").c_str()));
    json_object_object_add(o, "firmware", json_object_new_string(str_of(node, "fw", "V1.0.0").c_str()));
    json_object_object_add(o, "role", json_object_new_string(str_of(node, "role", "output").c_str()));
    /* Pending role lets the platform clean bindings/whitelist before reboot. */
    json_object* prv = nullptr;
    if (json_object_object_get_ex(node, "pendingRole", &prv) && prv &&
        !json_object_is_type(prv, json_type_null))
        json_object_object_add(o, "pendingRole", json_object_new_string(json_object_get_string(prv)));
    else
        json_object_object_add(o, "pendingRole", json_object_new_null());
    json_object* rcv = nullptr;
    if (json_object_object_get_ex(node, "roleCleanup", &rcv) && rcv &&
        !json_object_is_type(rcv, json_type_null))
        json_object_object_add(o, "roleCleanup", json_object_new_string(json_object_get_string(rcv)));
    else
        json_object_object_add(o, "roleCleanup", json_object_new_null());
    /* Advertise this agent's reachable base URL and its canvas geometry
     * in the global wall so the platform can fan out per-node sub-layouts. */
    if (!agent_base_.empty())
        json_object_object_add(o, "agentBase", json_object_new_string(agent_base_.c_str()));
    json_object* geo = nullptr;
    json_object_object_get_ex(node, "geometry", &geo);
    if (geo) {
        json_object_object_add(o, "geometry", json_object_get(geo));
    } else {
        json_object* g0 = json_object_new_object();
        json_object_object_add(g0, "x", json_object_new_int(0));
        json_object_object_add(g0, "y", json_object_new_int(0));
        json_object_object_add(g0, "w", json_object_new_int(0));
        json_object_object_add(g0, "h", json_object_new_int(0));
        json_object_object_add(o, "geometry", g0);
    }

    json_object* cap = nullptr;
    json_object_object_get_ex(node, "capabilities", &cap);
    json_object_object_add(o, "capability", cap ? json_object_get(cap) : json_object_new_object());

    json_object* net = nullptr;
    json_object_object_get_ex(node, "network", &net);
    json_object* net_out = net ? json_object_get(net) : json_object_new_object();
    if (net == nullptr) {
        json_object_object_add(net_out, "ip", json_object_new_string(str_of(node, "ip", "127.0.0.1").c_str()));
    }
    json_object_object_add(o, "network", net_out);

    json_object* ptp = json_object_new_object();
    json_object_object_add(ptp, "state", json_object_new_string("unknown"));
    json_object_object_add(ptp, "offsetUs", json_object_new_int(0));
    json_object_object_add(o, "ptp", ptp);
    return o;
}

json_object* Registration::build_heartbeat() {
    json_object* node = store_->node();
    json_object* o = json_object_new_object();
    std::string nid = str_of(node, "id", "local-node");
    json_object_object_add(o, "id", json_object_new_string(nid.c_str()));
    json_object_object_add(o, "uptimeSec", json_object_new_int((int)(time(nullptr) - start_time_)));
    json_object_object_add(o, "role", json_object_new_string(str_of(node, "role", "output").c_str()));
    json_object* phb = nullptr;
    if (json_object_object_get_ex(node, "pendingRole", &phb) && phb &&
        !json_object_is_type(phb, json_type_null))
        json_object_object_add(o, "pendingRole", json_object_new_string(json_object_get_string(phb)));
    else
        json_object_object_add(o, "pendingRole", json_object_new_null());
    json_object* rcb = nullptr;
    if (json_object_object_get_ex(node, "roleCleanup", &rcb) && rcb &&
        !json_object_is_type(rcb, json_type_null))
        json_object_object_add(o, "roleCleanup", json_object_new_string(json_object_get_string(rcb)));
    else
        json_object_object_add(o, "roleCleanup", json_object_new_null());
    json_object_object_add(o, "firmware", json_object_new_string(str_of(node, "fw", "V1.0.0").c_str()));

    /* appliedRevision: max applied across the six scopes */
    int applied = 0;
    json_object* cr = nullptr;
    if (json_object_object_get_ex(node, "configRevision", &cr) && cr) {
        for (int i = 0; i < 6; i++) {
            json_object* s = nullptr;
            if (json_object_object_get_ex(cr, NodeStore::SCOPES[i], &s) && s) {
                json_object* a = nullptr;
                if (json_object_object_get_ex(s, "applied", &a) && a)
                    applied = std::max(applied, json_object_get_int(a));
            }
        }
    }
    json_object_object_add(o, "appliedRevision", json_object_new_int(applied));

    /* media stats + ptp + lane digest from engine query_state (best effort).
     * Single query per heartbeat: the TX snapshot below reuses this copy. */
    json_object* media = json_object_new_object();
    json_object* ptp = json_object_new_object();
    int lane_ready = 0, lane_total = 0;
    long long lane_lost = 0;
    std::string tx_state = "unknown", tx_port = "", tx_sip = "";
    int tx_udp = 0;
    bool have_tx = false;
    std::string qs = media_->query_state();
    json_object* st = json_tokener_parse(qs.c_str());
    if (st) {
        json_object* d = nullptr;
        if (json_object_object_get_ex(st, "data", &d) && d) {
            json_object_object_add(media, "rxSessions", json_object_new_int(int_of(d, "rxSessions")));
            json_object_object_add(media, "decoding", json_object_new_int(int_of(d, "decoding")));
            json_object_object_add(media, "layers", json_object_new_int(int_of(d, "layers")));
            json_object* p = nullptr;
            if (json_object_object_get_ex(d, "ptp", &p) && p) {
                json_object_object_add(ptp, "state", json_object_new_string(str_of(p, "state", "unknown").c_str()));
                json_object_object_add(ptp, "offsetUs", json_object_new_int(int_of(p, "offsetUs")));
            }
            json_object* lanes = nullptr;
            if (json_object_object_get_ex(d, "lanes", &lanes) && lanes &&
                json_object_is_type(lanes, json_type_array)) {
                lane_total = (int)json_object_array_length(lanes);
                for (int i = 0; i < lane_total; i++) {
                    json_object* e = json_object_array_get_idx(lanes, i);
                    json_object* rv = nullptr;
                    bool ready = false;
                    if (e && json_object_object_get_ex(e, "ready", &rv) && rv)
                        ready = json_object_get_boolean(rv) != 0;
                    if (ready) lane_ready++;
                    json_object* lv = nullptr;
                    if (e && json_object_object_get_ex(e, "lostFrames", &lv) && lv)
                        lane_lost += json_object_get_int64(lv);
                }
            }
            json_object* tx = nullptr;
            if (json_object_object_get_ex(d, "tx", &tx) && tx) {
                have_tx = true;
                tx_state = str_of(tx, "state", "unknown");
                tx_port = str_of(tx, "port", "");
                json_object* tgt = nullptr;
                if (json_object_object_get_ex(tx, "target", &tgt) && tgt) {
                    tx_sip = str_of(tgt, "sip", "");
                    tx_udp = int_of(tgt, "udpPort");
                }
            }
            /* Fold the same snapshot into the stored playback projection
             * so /info reports playing without an extra status poll. */
            {
                json_object* pp = json_object_new_object();
                if (lane_ready > 0) {
                    json_object* cur = json_object_new_object();
                    json_object_object_add(cur, "status", json_object_new_string("playing"));
                    json_object_object_add(cur, "lanesReady", json_object_new_int(lane_ready));
                    json_object_object_add(cur, "lanesTotal", json_object_new_int(lane_total));
                    json_object_object_add(cur, "updatedAt", json_object_new_string(iso_now().c_str()));
                    json_object_object_add(pp, "currentPlayback", cur);
                } else {
                    json_object_object_add(pp, "currentPlayback", json_object_new_null());
                }
                store_->update_node(pp);
                json_object_put(pp);
            }
        }
        json_object_put(st);
    } else {
        json_object_object_add(media, "rxSessions", json_object_new_int(0));
        json_object_object_add(media, "decoding", json_object_new_int(0));
        json_object_object_add(media, "layers", json_object_new_int(0));
        json_object_object_add(ptp, "state", json_object_new_string("unknown"));
        json_object_object_add(ptp, "offsetUs", json_object_new_int(0));
    }
    json_object_object_add(o, "media", media);
    json_object_object_add(o, "ptp", ptp);
    json_object* digest = json_object_new_object();
    json_object_object_add(digest, "ready", json_object_new_int(lane_ready));
    json_object_object_add(digest, "total", json_object_new_int(lane_total));
    json_object_object_add(digest, "lostFrames", json_object_new_int64(lane_lost));
    json_object_object_add(o, "lanesDigest", digest);

    /* TX session digest (input role) reuses the snapshot above. Unknown when
     * the engine is silent; no second query so one heartbeat costs one IPC. */
    json_object* txd = json_object_new_object();
    if (have_tx) {
        json_object_object_add(txd, "state", json_object_new_string(tx_state.c_str()));
        json_object_object_add(txd, "port", json_object_new_string(tx_port.c_str()));
        if (!tx_sip.empty()) {
            json_object_object_add(txd, "targetSip", json_object_new_string(tx_sip.c_str()));
            json_object_object_add(txd, "targetPort", json_object_new_int(tx_udp));
        }
    } else {
        json_object_object_add(txd, "state", json_object_new_string("unknown"));
    }
    json_object_object_add(o, "txDigest", txd);

    /* SIP connectivity-probe summary (never touches media state). */
    {
        json_object* sip = nullptr;
        json_object_object_get_ex(node, "sip", &sip);
        bool en = false;
        if (sip) {
            json_object* ev = nullptr;
            if (json_object_object_get_ex(sip, "enabled", &ev) && ev)
                en = json_object_get_boolean(ev) != 0;
        }
        json_object* so = json_object_new_object();
        json_object_object_add(so, "enabled", json_object_new_boolean(en ? 1 : 0));
        json_object_object_add(so, "state",
                               json_object_new_string(sip ? str_of(sip, "state", "disabled").c_str() : "disabled"));
        json_object_object_add(so, "mode", json_object_new_string("connectivity-probe"));
        json_object_object_add(so, "server",
                               json_object_new_string(sip ? str_of(sip, "server", "").c_str() : ""));
        json_object_object_add(o, "sip", so);
    }

    /* resources (best effort): MemTotal parsed by key, not by position. */
    json_object* res = json_object_new_object();
    long mem_kb = 0;
    {
        std::ifstream mf("/proc/meminfo");
        std::string mline;
        while (std::getline(mf, mline)) {
            long v = 0;
            char key[64];
            if (sscanf(mline.c_str(), "%63[^:]: %ld", key, &v) == 2 &&
                strcmp(key, "MemTotal") == 0) {
                mem_kb = v;
                break;
            }
        }
    }
    json_object_object_add(res, "cpu", json_object_new_int(0));
    json_object_object_add(res, "memMb", json_object_new_int((int)(mem_kb / 1024)));
    long temp = read_long_file("/sys/class/thermal/thermal_zone0/temp", 0);
    json_object_object_add(res, "tempC", json_object_new_int((int)(temp / 1000)));
    json_object_object_add(o, "resources", res);

    return o;
}
