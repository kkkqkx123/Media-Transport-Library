// dvs_platform main: node gateway + docs CRUD + op barrier + serial upgrade.
#include <json-c/json.h>

#include <atomic>
#include <algorithm>
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
#include <vector>
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
#include "docs_routes.h"
#include "http_json.h"
#include "layout.h"
#include "ops.h"
#include "platform_store.h"

static PlatformStore* g_store = nullptr;
static OpCoordinator g_ops;
static std::mutex g_ops_mu;

// Layout admission limits (DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md §4):
// window size shares the 64~7680 per-cell pixel floor so degenerate rects
// never reach a node; the per-node window budget (layout.h,
// max_windows_per_node) is enforced per member canvas at prepare, never on
// the wall-global count — a 16-window wall over 16 nodes needs one plane per
// node. Violations reject the whole order — the same-frame invariant
// forbids partial walls.
static const int kMinWindowSize = 64;

// Escape a string for embedding inside a JSON string literal. Audit events
// are built by string concatenation over untrusted ids; without this, an id
// containing '"' can forge or break audit records (log injection).
struct ChannelRegistry {
    std::mutex mu;
    std::map<std::string, int> node_fd;
    std::map<int, std::string> fd_node;
    std::map<std::string, std::string> transport;
    std::map<std::string, std::string> idem;
    std::map<std::string, std::string> upgrade_progress;
    std::mutex send_mu;
    void register_conn(const std::string& node, int fd) {
        int old = -1;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = node_fd.find(node);
            if (it != node_fd.end() && it->second != fd) {
                old = it->second;
                fd_node.erase(old);
            }
            node_fd[node] = fd;
            fd_node[fd] = node;
            transport[node] = "ws";
        }
        if (old >= 0) ::shutdown(old, SHUT_RDWR);
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
    // 查表与发送原子化：同时持有两把锁，避免查到旧句柄后连接被替换。
    bool send_to(const std::string& node, const std::string& frame) {
        std::unique_lock<std::mutex> l1(mu, std::defer_lock);
        std::unique_lock<std::mutex> l2(send_mu, std::defer_lock);
        std::lock(l1, l2);
        auto it = node_fd.find(node);
        if (it == node_fd.end()) return false;
        return HttpServer::ws_send_text(it->second, frame, false);
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
// Wall/binding/playback pure-JSON accessors live in layout.h (shared by the
// docs routes and the barrier paths); store-reading scans stay local below.
// Derive fanout members from the binding table. Pending-offline bindings are
// excluded; callers report them separately.
static std::set<std::string> derive_wall_members(const std::string& wall_id,
                                                  std::set<std::string>* pending_out) {
    std::set<std::string> out;
    for (auto& bid : g_store->list_ids("bindings")) {
        json_object* b = g_store->get_doc("bindings", bid);
        if (!b) continue;
        if (binding_wall_of(b) != wall_id) { json_object_put(b); continue; }
        std::string node = binding_node_of(b);
        std::string st = body_str(b, "status");
        if (node.empty()) { json_object_put(b); continue; }
        if (st == "pending_offline" || st == "pending") {
            if (pending_out) pending_out->insert(node);
        } else {
            out.insert(node);
        }
        json_object_put(b);
    }
    return out;
}
static void mark_wall_committed(const std::string& wall_id,
                                const std::string& scene_id, int revision,
                                const std::string& hash) {
    if (wall_id.empty()) return;
    json_object* w = g_store->get_doc("walls", wall_id);
    if (!w) return;
    if (!scene_id.empty()) {
        json_object_object_del(w, "currentSceneId");
        json_object_object_add(w, "currentSceneId",
                               json_object_new_string(scene_id.c_str()));
    }
    json_object_object_del(w, "appliedRevision");
    json_object_object_add(w, "appliedRevision", json_object_new_int(revision));
    if (!hash.empty()) {
        json_object_object_del(w, "layoutHash");
        json_object_object_add(w, "layoutHash",
                               json_object_new_string(hash.c_str()));
    }
    g_store->put_doc("walls", wall_id, w);
    json_object_put(w);
}
// Role intent carried by register/heartbeat: mirrored verbatim so the
// platform can tell whether a node sits in a role-switch transition.
// Only overwrites when the field is present; absent means "unknown, keep".
static void store_role_intent(json_object* cur, json_object* data) {
    if (!cur || !data) return;
    json_object* v = nullptr;
    if (json_object_object_get_ex(data, "pendingRole", &v)) {
        json_object_object_del(cur, "pendingRole");
        json_object_object_add(cur, "pendingRole",
                               v ? json_object_get(v) : json_object_new_null());
    }
    if (json_object_object_get_ex(data, "roleCleanup", &v)) {
        json_object_object_del(cur, "roleCleanup");
        json_object_object_add(cur, "roleCleanup",
                               v ? json_object_get(v) : json_object_new_null());
    }
}
// Offline bindings wait as pending and hold no port. When the node comes
// back, re-run the write-path checks (wall alive, signal whitelisted,
// output idle) and promote what still passes; conflicts stay pending.
static void promote_pending_bindings(const std::string& node_id) {
    if (node_id.empty() || !g_store) return;
    for (auto& bid : g_store->list_ids("bindings")) {
        json_object* b = g_store->get_doc("bindings", bid);
        if (!b) continue;
        if (binding_node_of(b) != node_id) { json_object_put(b); continue; }
        std::string st = body_str(b, "status");
        if (st != "pending" && st != "pending_offline") {
            json_object_put(b);
            continue;
        }
        std::string w = binding_wall_of(b);
        std::string outp = binding_output_of(b);
        std::string sig = binding_sig_of(b);
        std::string why;
        json_object* wall = g_store->get_doc("walls", w);
        if (!wall || wall_is_deleted(wall)) why = "wall unknown or deleted";
        if (wall) json_object_put(wall);
        if (why.empty() && !sig.empty()) {
            bool found = false;
            for (auto& wid : g_store->list_ids("whitelist")) {
                json_object* o = g_store->get_doc("whitelist", wid);
                if (!o) continue;
                bool hit = (body_str(o, "sig") == sig);
                json_object_put(o);
                if (hit) { found = true; break; }
            }
            if (!found) why = "binding signal not in whitelist";
        }
        if (why.empty() && !outp.empty()) {
            for (auto& obid : g_store->list_ids("bindings")) {
                if (obid == bid) continue;
                json_object* o = g_store->get_doc("bindings", obid);
                if (!o) continue;
                std::string ost = body_str(o, "status");
                bool busy = (binding_node_of(o) == node_id &&
                             binding_output_of(o) == outp &&
                             ost != "pending" && ost != "pending_offline");
                json_object_put(o);
                if (busy) { why = "node output port already bound"; break; }
            }
        }
        if (why.empty()) {
            json_object_object_del(b, "status");
            json_object_object_add(b, "status",
                                   json_object_new_string("active"));
            g_store->put_doc("bindings", bid, b);
            json_object_put(b);
            g_store->audit("{\"binding-promoted\":\"" + json_escape(bid) + "\"}");
        } else {
            json_object_put(b);
            g_store->audit("{\"binding-still-pending\":\"" + json_escape(bid) +
                           "\",\"reason\":\"" + json_escape(why) + "\"}");
        }
    }
}

static void store_ptp_playback(json_object* cur, json_object* data) {
    if (!cur || !data) return;
    json_object* p = nullptr;
    if (json_object_object_get_ex(data, "ptp", &p) && p) {
        json_object_object_del(cur, "ptp");
        json_object_object_add(cur, "ptp", json_object_get(p));
    }
    json_object* pb = nullptr;
    if (json_object_object_get_ex(data, "currentPlayback", &pb) && pb) {
        json_object_object_del(cur, "currentPlayback");
        json_object_object_add(cur, "currentPlayback", json_object_get(pb));
    }
}

void apply_hello_doc(json_object* data) {
    std::string id = body_str(data, "id");
    if (id.empty()) return;
    json_object* merged = json_object_get(data);
    json_object_object_add(merged, "online", json_object_new_boolean(1));
    json_object_object_add(merged, "lastHeartbeat",
                           json_object_new_string(now_iso().c_str()));
    json_object_object_add(merged, "transport", json_object_new_string("ws"));
    g_store->put_doc("node", id, merged);
    json_object_put(merged);
    g_chan.set_transport(id, "ws");
    promote_pending_bindings(id);
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
    store_ptp_playback(cur, data);
    store_role_intent(cur, data);
    g_store->put_doc("node", id, cur);
    json_object_put(cur);
    g_chan.set_transport(id, "ws");
    promote_pending_bindings(id);
}

std::string handle_barrier_ack(const std::string& opid, const std::string& node,
                              std::string st, json_object* lanes,
                              std::set<std::string>& missing_out,
                              json_object* ack) {
    if (st.empty()) st = "ready";
    if (st != "ready") st = "blocked";
    if (!barrier_lanes_ready(lanes)) st = "blocked";
    {
        int64_t seq = 0;
        if (!barrier_lanes_frame_consistent(lanes, &seq)) st = "blocked";
    }
    {
        json_object* nd = g_store ? g_store->get_doc("node", node) : nullptr;
        std::string reason;
        bool ok = node_clock_ok(nd, &reason);
        if (nd) json_object_put(nd);
        if (!ok) {
            st = "blocked";
            if (g_store)
                g_store->audit("{\"commit-clock-blocked\":\"" + json_escape(node) +
                               "\",\"reason\":\"" + json_escape(reason) + "\"}");
        }
    }
    // Frame-anchored verification: when the op carries a frame base and the
    // ack carries a frame report, they must agree. A node that committed a
    // different frame or missed its window fails the same-frame invariant
    // and the wall rolls back instead of showing a mixed scene.
    if (ack) {
        int64_t want_frame = -2;
        uint64_t want_epoch = 0;
        {
            std::lock_guard<std::mutex> lk(g_ops_mu);
            const Operation* op = g_ops.find(opid);
            if (op && op->epoch_ns > 0) {
                want_epoch = op->epoch_ns;
                want_frame = op->frame_index;
            }
        }
        json_object* fv = nullptr;
        if (want_epoch > 0 && want_frame >= 0 &&
            json_object_object_get_ex(ack, "frameIndex", &fv) && fv &&
            json_object_is_type(fv, json_type_int)) {
            int64_t got = json_object_get_int64(fv);
            if (got != want_frame) {
                st = "blocked";
                if (g_store)
                    g_store->audit("{\"commit-frame-mismatch\":\"" +
                                   json_escape(node) + "\",\"want\":" +
                                   std::to_string(want_frame) + ",\"got\":" +
                                   std::to_string(got) + "}");
            }
        }
        json_object* late = nullptr;
        if (json_object_object_get_ex(ack, "late", &late) && late) {
            bool is_late = json_object_is_type(late, json_type_boolean)
                                ? (json_object_get_boolean(late) != 0)
                                : (json_object_get_int(late) != 0);
            if (is_late) {
                st = "blocked";
                if (g_store)
                    g_store->audit("{\"commit-frame-late\":\"" +
                                   json_escape(node) + "\"}");
            }
        }
        std::string why = body_str(ack, "reason");
        if (!why.empty() && g_store)
            g_store->audit("{\"commit-reason\":\"" + json_escape(node) +
                           "\",\"reason\":\"" + json_escape(why) + "\"}");
    }
    std::string state;
    std::string commit_wall, commit_scene, commit_hash;
    int commit_rev = 0;
    {
        std::lock_guard<std::mutex> lk(g_ops_mu);
        state = g_ops.on_reply(opid, node, st != "blocked", st);
        missing_out = g_ops.missing(opid);
        if (state == "committed") {
            const Operation* op = g_ops.find(opid);
            if (op) {
                commit_wall = op->wall_id;
                commit_scene = op->scene_id;
                commit_rev = op->revision;
                commit_hash = op->layout_hash;
            }
        }
    }
    if (state == "committed" && !commit_wall.empty())
        mark_wall_committed(commit_wall, commit_scene, commit_rev, commit_hash);
    g_store->audit("{\"commit\":\"" + json_escape(node) + "\",\"state\":\"" + json_escape(st) + "\"}");
    return state;
}

bool ws_push(const std::string& node, const std::string& type, json_object* data) {
    json_object* msg = ct::wrap(ct::next_msg_id("p"), type, data,
                               ct::next_request_id("p"));
    const char* s = json_object_to_json_string(msg);
    std::string frame = s ? s : "{}";
    json_object_put(msg);
    return g_chan.send_to(node, frame);
}

// Outcome of one per-node layout delivery attempt.
struct LayoutDelivery {
    bool sent = false;       // payload handed to a transport (WS or HTTP)
    bool via_ws = false;
    int http_status = 0;     // meaningful when sent && !via_ws
    bool skipped = false;
    std::string skip_reason; // "no node doc" / "no geometry" / "no channel"
};

// Deliver one per-node layout.prepare payload to a single node: WS first,
// short HTTP to the agent base as fallback. Shared by the prepare fanout and
// the rollback reissue, which previously duplicated this loop inline. Audits
// stay with the callers because the two paths use different audit schemas.
// volume < 0 omits the scene-volume key (rollback reissue keeps the engine
// volume untouched); audioOwner derives from the windows themselves (the
// latest un-muted window speaks, matching the frontend behavior).
static LayoutDelivery deliver_layout_prepare(const std::string& node_id,
                                             const std::string& opid,
                                             int revision, int64_t apply_at_ns,
                                             json_object* wins, int volume,
                                             const std::string& token,
                                             const std::string& wall_id = "",
                                             const std::string& scene_id = "",
                                             const std::string& layout_hash = "",
                                             int64_t epoch_ns = 0,
                                             double fps = 60.0,
                                             int64_t frame_index = -1,
                                             int wall_w = 0, int wall_h = 0,
                                             const std::string& base_json = "",
                                             const std::string& adjust_json = "") {
    LayoutDelivery d;
    json_object* nd = g_store->get_doc("node", node_id);
    if (!nd) { d.skipped = true; d.skip_reason = "no node doc"; return d; }
    std::string agent_base = body_str(nd, "agentBase");
    json_object* geo = nullptr;
    json_object_object_get_ex(nd, "geometry", &geo);
    bool geo_ok = geo && rect_int(geo, "w") > 0 && rect_int(geo, "h") > 0;
    if (!geo_ok) {
        d.skipped = true; d.skip_reason = "no geometry";
        json_object_put(nd);
        return d;
    }
    Rect node{rect_int(geo, "x"), rect_int(geo, "y"),
              rect_int(geo, "w"), rect_int(geo, "h")};
    json_object* frag = fragment_for_node(wins, node);
    // Endpoint table: only the signals this node's windows reference. Entries
    // come from the whitelist docs; the port field is authoritative (added per
    // DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md), defaulting to the requirement
    // example's port range when an entry omits it. fps rides the same
    // table (req 06 §1.2: fps is a whitelist encoding param; the engine
    // rejects streams without one): whitelist fps when present, else the
    // platform-wide 60 default shared with the commit guard computation.
    // audioPort is optional (0/absent = no subscribable audio on this signal).
    std::set<std::string> sigs;
    int fn = json_object_array_length(frag);
    for (int i = 0; i < fn; i++)
        sigs.insert(body_str(json_object_array_get_idx(frag, i), "sig"));
    json_object* endpoints = json_object_new_array();
    if (!sigs.empty()) {
        for (auto& wid : g_store->list_ids("whitelist")) {
            json_object* wl = g_store->get_doc("whitelist", wid);
            if (!wl) continue;
            std::string wsig = body_str(wl, "sig");
            if (!sigs.count(wsig)) { json_object_put(wl); continue; }
            json_object* ep = json_object_new_object();
            json_object_object_add(ep, "sig", json_object_new_string(wsig.c_str()));
            json_object_object_add(ep, "address",
                                   json_object_new_string(body_str(wl, "address").c_str()));
            json_object* pv = nullptr;
            int port = 0;
            if (json_object_object_get_ex(wl, "port", &pv)) port = json_object_get_int(pv);
            if (port <= 0) port = 50010;
            json_object_object_add(ep, "port", json_object_new_int(port));
            json_object* fv = nullptr;
            int fps = 0;
            if (json_object_object_get_ex(wl, "fps", &fv) && fv)
                fps = json_object_get_int(fv);
            if (fps <= 0) fps = 60;
            json_object_object_add(ep, "fps", json_object_new_int(fps));
            json_object* apv = nullptr;
            int aport = 0;
            if (json_object_object_get_ex(wl, "audioPort", &apv) && apv)
                aport = json_object_get_int(apv);
            if (aport > 0)
                json_object_object_add(ep, "audioPort",
                                       json_object_new_int(aport));
            std::string mode = body_str(wl, "mode");
            json_object_object_add(ep, "mode",
                                   json_object_new_string(mode.empty() ? "multicast" : mode.c_str()));
            std::string ssrc = body_str(wl, "sourceIp");
            if (!ssrc.empty())
                json_object_object_add(ep, "sourceIp",
                                       json_object_new_string(ssrc.c_str()));
            json_object_array_add(endpoints, ep);
            json_object_put(wl);
        }
    }
    // Audio owner: latest un-muted window in fanout order (the frontend mutes
    // the others when a new window opens, so the newest un-muted one speaks).
    std::string audio_owner;
    if (wins) {
        int wn = json_object_array_length(wins);
        for (int i = wn - 1; i >= 0; i--) {
            json_object* w = json_object_array_get_idx(wins, i);
            json_object* mv = nullptr;
            bool muted = false;
            if (json_object_object_get_ex(w, "muted", &mv))
                muted = json_object_get_boolean(mv);
            std::string wid = body_str(w, "id");
            if (!wid.empty() && !muted) { audio_owner = wid; break; }
        }
    }
    json_object* payload = json_object_new_object();
    json_object_object_add(payload, "operationId", json_object_new_string(opid.c_str()));
    json_object_object_add(payload, "revision", json_object_new_int(revision));
    json_object_object_add(payload, "applyAtNs", json_object_new_int64(apply_at_ns));
    if (epoch_ns > 0) {
        json_object_object_add(payload, "epochTaiNs",
                               json_object_new_int64(epoch_ns));
        if (fps > 0.0)
            json_object_object_add(payload, "fps", json_object_new_double(fps));
        if (frame_index >= 0)
            json_object_object_add(payload, "frameIndex",
                                   json_object_new_int64(frame_index));
    }
    // Wall still for this member: the platform names the image and the
    // node-local sub-rectangle; the node fetches, crops and paints it.
    if (!base_json.empty()) {
        json_object* bi = json_tokener_parse(base_json.c_str());
        if (bi && json_object_is_type(bi, json_type_object)) {
            Rect sub;
            if (base_subrect(wall_w, wall_h, node, &sub)) {
                json_object* subo = json_object_new_object();
                json_object_object_add(subo, "x", json_object_new_int(sub.x));
                json_object_object_add(subo, "y", json_object_new_int(sub.y));
                json_object_object_add(subo, "w", json_object_new_int(sub.w));
                json_object_object_add(subo, "h", json_object_new_int(sub.h));
                json_object_object_add(bi, "sub", subo);
            }
            json_object_object_add(payload, "baseImage", bi);
        } else if (bi) {
            json_object_put(bi);
        }
    }
    if (!adjust_json.empty()) {
        json_object* ia = json_tokener_parse(adjust_json.c_str());
        if (ia && json_object_is_type(ia, json_type_object))
            json_object_object_add(payload, "imageAdjust", ia);
        else if (ia)
            json_object_put(ia);
    }
    json_object_object_add(payload, "geometry", json_object_get(geo));
    json_object_object_add(payload, "windows", frag);
    json_object_object_add(payload, "endpoints", endpoints);
    if (!audio_owner.empty())
        json_object_object_add(payload, "audioOwner",
                               json_object_new_string(audio_owner.c_str()));
    if (volume >= 0)
        json_object_object_add(payload, "volume", json_object_new_int(volume));
    if (!wall_id.empty())
        json_object_object_add(payload, "wallId",
                               json_object_new_string(wall_id.c_str()));
    if (!scene_id.empty())
        json_object_object_add(payload, "sceneId",
                               json_object_new_string(scene_id.c_str()));
    if (!layout_hash.empty())
        json_object_object_add(payload, "layoutHash",
                               json_object_new_string(layout_hash.c_str()));
    bool via_ws = false;
    if (ct::validate_layout_prepare(payload).empty())
        via_ws = ws_push(node_id, ct::kLayoutPrepare, json_object_get(payload));
    if (via_ws) {
        d.sent = true; d.via_ws = true;
    } else if (!agent_base.empty()) {
        const char* pb = json_object_to_json_string(payload);
        std::string resp;
        d.http_status = HttpClient().post_json(
            agent_base + "/api/v1/node/media/layout",
            std::string(pb ? pb : "{}"), resp, 5000, 2, token);
        d.sent = true;
    } else {
        d.skipped = true; d.skip_reason = "no channel";
    }
    json_object_put(payload);
    json_object_put(nd);
    return d;
}

// Sharded fanout shared by prepare and rollback: each worker parses its own
// snapshot copy and delivers a strided subset so large walls stay within the
// barrier timeout. Audits keep the caller schema via is_rollback.
static void fanout_layout_parallel(const std::set<std::string>& members,
                                   const std::string& opid, int rev, int64_t apply_at,
                                   const std::string& windows_json, int volume,
                                   const std::string& token, const std::string& wall_id,
                                   const std::string& scene_id, const std::string& hash,
                                   int64_t epoch_ns, double fps, int64_t frame_index,
                                   int wall_w, int wall_h,
                                   const std::string& base_json,
                                   const std::string& adjust_json,
                                   bool is_rollback) {
    if (members.empty()) return;
    std::vector<std::string> ids(members.begin(), members.end());
    int shards = fanout_parallelism();
    if (shards < 1) shards = 1;
    if (shards > (int)ids.size()) shards = (int)ids.size();
    std::vector<std::thread> workers;
    for (int s = 0; s < shards; s++) {
        workers.emplace_back([s, shards, ids, opid, rev, apply_at, windows_json,
                              volume, token, wall_id, scene_id, hash, epoch_ns,
                              fps, frame_index, wall_w, wall_h, base_json,
                              adjust_json, is_rollback]() {
            json_object* wins = json_tokener_parse(windows_json.c_str());
            for (size_t i = (size_t)s; i < ids.size(); i += (size_t)shards) {
                const std::string& mid = ids[i];
                LayoutDelivery d = deliver_layout_prepare(
                    mid, opid, rev, apply_at, wins, volume, token, wall_id,
                    scene_id, hash, epoch_ns, fps, frame_index, wall_w, wall_h,
                    base_json, adjust_json);
                if (is_rollback) {
                    if (d.skipped && d.skip_reason == "no geometry") continue;
                    if (d.via_ws) {
                        g_store->audit("{\"rollback\":\"" + json_escape(opid) +
                                       "\",\"node\":\"" + json_escape(mid) +
                                       "\",\"via\":\"ws\",\"executed\":1}");
                    } else if (d.sent) {
                        g_store->audit("{\"rollback\":\"" + json_escape(opid) +
                                       "\",\"node\":\"" + json_escape(mid) +
                                       "\",\"executed\":" +
                                       std::to_string(d.http_status == 200 ? 1 : 0) + "}");
                    } else {
                        g_store->audit("{\"rollback\":\"" + json_escape(opid) +
                                       "\",\"node\":\"" + json_escape(mid) +
                                       "\",\"executed\":0,\"reason\":\"" +
                                       json_escape(d.skip_reason) + "\"}");
                    }
                } else {
                    if (d.via_ws) {
                        g_store->audit("{\"fanout\":\"" + json_escape(mid) +
                                       "\",\"via\":\"ws\"}");
                        g_chan.set_transport(mid, "ws");
                    } else if (d.sent) {
                        g_store->audit("{\"fanout\":\"" + json_escape(mid) +
                                       "\",\"status\":" +
                                       std::to_string(d.http_status) + "}");
                        g_chan.set_transport(mid, "short");
                    } else {
                        g_store->audit("{\"fanout-skip\":\"" + json_escape(mid) +
                                       "\",\"reason\":\"" + json_escape(d.skip_reason) + "\"}");
                    }
                }
            }
            if (wins) json_object_put(wins);
        });
    }
    for (auto& t : workers) t.join();
}

// Actually execute a rollback: find the settled op whose revision matches
// to_revision, take its window snapshot, and re-issue that layout to every
// member with the rollback op id and applyAtNs=0 (immediate apply). Delivery
// mirrors the prepare fanout: WS first, short HTTP as fallback. When no
// snapshot exists the rollback is only audited — never faked as executed.
// Callers must NOT hold g_ops_mu (execute_rollback takes it itself).
static void execute_rollback(const std::string& rollback_op_id, int to_revision,
                             const std::set<std::string>& members) {
    const Operation* snap = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ops_mu);
        snap = g_ops.find_committed(to_revision);
    }
    if (!snap) {
        g_store->audit("{\"rollback\":\"" + json_escape(rollback_op_id) +
                       "\",\"executed\":0,\"reason\":\"no snapshot\"}");
        return;
    }
    std::string opid_c = rollback_op_id;
    int rev_c = to_revision;
    std::string windows_json = snap->windows_json;
    int vol_c = snap->volume;
    std::string wall_c = snap->wall_id;
    std::string scene_c = snap->scene_id;
    std::string hash_c = snap->layout_hash;
    int wall_w_c = snap->wall_w;
    int wall_h_c = snap->wall_h;
    int64_t epoch_c = (int64_t)snap->epoch_ns;
    double fps_c = snap->fps;
    std::string token_c = g_api_token;
    {
        std::lock_guard<std::mutex> lk(g_ops_mu);
        uint64_t deadline = now_ns() + (uint64_t)ct::barrier_timeout_ms() * 1000000ULL;
        Operation& rb = g_ops.begin_with_deadline(opid_c, rev_c, members, 0, deadline);
        rb.windows_json = windows_json;
        rb.volume = vol_c;
        rb.wall_id = wall_c;
        rb.scene_id = scene_c;
        rb.layout_hash = hash_c;
        rb.wall_w = wall_w_c;
        rb.wall_h = wall_h_c;
        rb.epoch_ns = snap->epoch_ns;
        rb.fps = snap->fps;
        rb.frame_index = -1;
        rb.created_ns = now_ns();
        g_ops.persist(opid_c);
    }
    std::thread([opid_c, rev_c, windows_json, members, token_c, vol_c, wall_c,
                 scene_c, hash_c, epoch_c, fps_c, wall_w_c, wall_h_c]() {
        fanout_layout_parallel(members, opid_c, rev_c, 0, windows_json, vol_c,
                               token_c, wall_c, scene_c, hash_c, epoch_c, fps_c,
                               -1, wall_w_c, wall_h_c, "", "", true);
    }).detach();
}

static std::string ws_idem_key(const std::string& msg_id, const std::string& tp, json_object* data) {
    std::string node = body_str(data, "id");
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
    g_store->audit("{\"ws-hello\":\"" + json_escape(node_id) + "\"}");
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "heartbeatIntervalMs",
                               json_object_new_int(ct::heartbeat_interval_ms()));
        json_object_object_add(w, "barrierTimeoutMs",
                               json_object_new_int(ct::barrier_timeout_ms()));
        json_object_object_add(w, "id", json_object_new_string(node_id.c_str()));
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
                    (lanes && json_object_is_type(lanes, json_type_array)) ? lanes : nullptr, miss, dd);
                const Operation* op = nullptr;
                { std::lock_guard<std::mutex> lk(g_ops_mu); op = g_ops.find(opid); }
                json_object* ack = json_object_new_object();
                if (!op) {
                    json_object_object_add(ack, "state", json_object_new_string("unknown"));
                } else if (state == "blocked") {
                    int to_rev = 0;
                    std::string rb;
                    std::set<std::string> rb_members;
                    {
                        std::lock_guard<std::mutex> lk(g_ops_mu);
                        rb = g_ops.rollback_op(opid, &to_rev);
                        const Operation* op2 = g_ops.find(opid);
                        if (op2) rb_members = op2->members;
                    }
                    if (!rb.empty()) execute_rollback(rb, to_rev, rb_members);
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
                g_store->audit("{\"upgrade-progress\":\"" + json_escape(node) + "\",\"task\":\"" + json_escape(tid) + "\",\"status\":\"" + json_escape(st) + "\"}");
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
                g_store->audit("{\"config-ack\":\"" + json_escape(body_str(dd, "node")) + "\"}");
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
        g_store->audit("{\"ws-offline\":\"" + json_escape(gone) + "\"}");
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
    g_ops.set_store(g_store);

    // Crash recovery: reload persisted operations. Preparing ops keep their
    // deadline and are expired/rolled back by the sweeper; committed ops are
    // restored as rollback snapshots (see DVS_OP_STATE_PERSISTENCE_DESIGN.md).
    // Fanout is deliberately not re-issued on recovery.
    for (const auto& id : g_store->list_ids("op")) {
        json_object* d = g_store->get_doc("op", id);
        if (!d) continue;
        Operation op;
        if (operation_from_json(d, &op)) {
            std::lock_guard<std::mutex> lk(g_ops_mu);
            g_ops.restore(op);
        } else {
            g_store->del_doc("op", id);  // unparseable record, drop it
        }
        json_object_put(d);
    }

    HttpServer::Handler h = [](const HttpRequest& req, HttpResponse& res) {
        // Unified write gate: when a control-plane token is configured,
        // every mutating API call must carry it; reads stay open so local
        // consoles keep working. Empty token means no check (prototype).
        if (req.path.rfind("/api/", 0) == 0 && req.method != "GET" &&
            !bearer_ok(req)) {
            send_json(res, fail_envelope("UNAUTHORIZED", 401,
                                          "missing or bad token"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/nodes/register") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string verr = b ? ct::validate_hello(b) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            std::string id = body_str(b, "id");
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
            g_store->audit("{\"register\":\"" + json_escape(id) + "\"}");
            promote_pending_bindings(id);
            if (!has_base || !has_geo)
                g_store->audit("{\"incomplete-register\":\"" + json_escape(id) + "\"}");
            json_object* d = g_store->get_doc("node", id);
            json_object_put(b);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string verr = b ? ct::validate_heartbeat(b) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            std::string id = body_str(b, "id");
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
            store_ptp_playback(cur, b);
            store_role_intent(cur, b);
            json_object_object_add(cur, "transport", json_object_new_string("short"));
            g_store->put_doc("node", id, cur);
            g_chan.set_transport(id, "short");
            json_object_put(b);
            promote_pending_bindings(id);
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
                g_store->audit("{\"rename\":\"" + json_escape(id) + "\"}");
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
                store_ptp_playback(cur, body2);
                store_role_intent(cur, body2);
                g_store->put_doc("node", id, cur);
                if (body2) json_object_put(body2);
                json_object_put(cur);
                promote_pending_bindings(id);
                json_object* d = g_store->get_doc("node", id);
                send_json(res, ok_envelope(d ? d : json_object_new_object(), "heartbeat received"));
                return;
            }
            if (req.method == "GET" && act == "current-task") {
                // Projection from playback-task docs: the node's active task,
                // falling back to the node-reported currentPlayback digest.
                json_object* proj = nullptr;
                for (auto& tid : g_store->list_ids("playback-tasks")) {
                    json_object* t = g_store->get_doc("playback-tasks", tid);
                    if (!t) continue;
                    std::string tn = body_str(t, "targetNodeId");
                    bool active = (tn == id && playback_active_status(playback_status_of(t)));
                    if (active) { proj = t; break; }
                    json_object_put(t);
                }
                json_object* d = json_object_new_object();
                if (proj) {
                    json_object_object_add(d, "task", proj);
                } else {
                    json_object* nd = g_store->get_doc("node", id);
                    json_object* pb = nullptr;
                    if (nd) json_object_object_get_ex(nd, "currentPlayback", &pb);
                    json_object_object_add(d, "task",
                        pb ? json_object_get(pb) : json_object_new_null());
                    if (nd) json_object_put(nd);
                }
                send_json(res, ok_envelope(d, ""));
                return;
            }
            if (req.method == "POST" && act == "retire") {
                // Reference-checked retire: output nodes must be unbound and
                // input signals must be unlisted first. Offline nodes leave a
                // retire-pending marker instead of being deleted silently.
                std::vector<std::string> refs;
                for (auto& bid : g_store->list_ids("bindings")) {
                    json_object* b = g_store->get_doc("bindings", bid);
                    if (!b) continue;
                    bool hit = (binding_node_of(b) == id);
                    json_object_put(b);
                    if (hit) refs.push_back("binding:" + bid);
                }
                for (auto& wid : g_store->list_ids("whitelist")) {
                    json_object* w = g_store->get_doc("whitelist", wid);
                    if (!w) continue;
                    bool hit = (body_str(w, "nodeId") == id);
                    json_object_put(w);
                    if (hit) refs.push_back("whitelist:" + wid);
                }
                if (!refs.empty()) {
                    json_object* d = json_object_new_object();
                    json_object* ra = json_object_new_array();
                    for (auto& r : refs)
                        json_object_array_add(ra, json_object_new_string(r.c_str()));
                    json_object_object_add(d, "refs", ra);
                    g_store->audit("{\"retire-rejected\":\"" + json_escape(id) + "\"}");
                    send_json(res, fail_envelope("RESOURCE_IN_USE", 409,
                        "node still referenced; unbind/unlist first"));
                    return;
                }
                json_object* nd = g_store->get_doc("node", id);
                if (!nd) {
                    send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown"));
                    return;
                }
                bool online = false;
                {
                    json_object* v = nullptr;
                    if (json_object_object_get_ex(nd, "online", &v) && v)
                        online = json_object_get_boolean(v) != 0;
                }
                json_object_put(nd);
                if (!online) {
                    json_object* q = json_object_new_object();
                    json_object_object_add(q, "id", json_object_new_string(id.c_str()));
                    json_object_object_add(q, "nodeId", json_object_new_string(id.c_str()));
                    json_object_object_add(q, "status",
                                           json_object_new_string("retire_pending"));
                    g_store->put_doc("retire-queue", id, q);
                    json_object_put(q);
                    g_store->audit("{\"retire-pending\":\"" + json_escape(id) + "\"}");
                    json_object* d = json_object_new_object();
                    json_object_object_add(d, "status",
                                           json_object_new_string("retire_pending"));
                    send_json(res, ok_envelope(d, "retire pending, offline"));
                    return;
                }
                g_store->tombstone_add(id, now_sec());
                g_store->del_doc("node", id);
                g_store->del_doc("retire-queue", id);
                g_store->audit("{\"retire\":\"" + json_escape(id) + "\"}");
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
                            g_store->audit("{\"config-push-ignored\":\"" + json_escape(k) + "\"}");
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
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string opid = body_str(b, "operationId");
            int rev = 1;
            json_object* v = nullptr;
            if (b && json_object_object_get_ex(b, "revision", &v)) rev = json_object_get_int(v);
            std::string wall_id = body_str(b, "wallId");
            if (wall_id.empty()) wall_id = body_str(b, "wall");
            std::string scene_id = body_str(b, "sceneId");
            if (scene_id.empty()) scene_id = body_str(b, "scene");
            std::set<std::string> members;
            json_object* arr = nullptr;
            if (b && json_object_object_get_ex(b, "members", &arr) &&
                json_object_is_type(arr, json_type_array)) {
                for (size_t i = 0; i < json_object_array_length(arr); i++)
                    members.insert(json_object_get_string(json_object_array_get_idx(arr, i)));
            }
            if (opid.empty() || (members.empty() && wall_id.empty())) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "need operationId+members"));
                return;
            }
            // Wall-anchored path: derive members from bindings, verify the
            // caller list when present, and take canvas/fps from the wall doc.
            int wall_w = 0, wall_h = 0;
            double fps = 60.0;
            int wall_applied = 0;
            int64_t wall_epoch = 0;
            int wall_min_side = kMinWindowSize;
            std::string wall_base_json;
            std::string wall_adjust_json;
            std::set<std::string> pending;
            if (!wall_id.empty()) {
                json_object* wdoc = g_store->get_doc("walls", wall_id);
                if (!wdoc || wall_is_deleted(wdoc)) {
                    if (wdoc) json_object_put(wdoc);
                    if (b) json_object_put(b);
                    send_json(res, fail_envelope("NOT_FOUND", 404, "wall unknown or deleted"));
                    return;
                }
                wall_px_of(wdoc, &wall_w, &wall_h);
                fps = wall_fps_of(wdoc);
                wall_min_side = wall_min_window_side(wdoc, wall_w, wall_h);
                {
                    json_object* ev = nullptr;
                    if (json_object_object_get_ex(wdoc, "epochTaiNs", &ev) && ev &&
                        json_object_is_type(ev, json_type_int))
                        wall_epoch = json_object_get_int64(ev);
                    if (wall_epoch <= 0) {
                        // Self-heal for walls created before the frame base
                        // existed: stamp once, then freeze like any epoch.
                        wall_epoch = (int64_t)now_ns();
                        json_object_object_del(wdoc, "epochTaiNs");
                        json_object_object_add(wdoc, "epochTaiNs",
                                               json_object_new_int64(wall_epoch));
                        g_store->put_doc("walls", wall_id, wdoc);
                    }
                    json_object* biv = nullptr;
                    if (json_object_object_get_ex(wdoc, "baseImage", &biv) && biv &&
                        json_object_is_type(biv, json_type_object) &&
                        validate_base_image(biv).empty()) {
                        const char* bs = json_object_to_json_string(biv);
                        if (bs) wall_base_json = bs;
                    }
                    json_object* iav = nullptr;
                    if (json_object_object_get_ex(wdoc, "imageAdjust", &iav) && iav &&
                        json_object_is_type(iav, json_type_object) &&
                        validate_image_adjust(iav).empty()) {
                        const char* as = json_object_to_json_string(iav);
                        if (as) wall_adjust_json = as;
                    }
                }
                json_object* av = nullptr;
                if (json_object_object_get_ex(wdoc, "appliedRevision", &av) && av)
                    wall_applied = json_object_get_int(av);
                json_object_put(wdoc);
                std::set<std::string> derived = derive_wall_members(wall_id, &pending);
                if (!members.empty() && members != derived) {
                    if (b) json_object_put(b);
                    send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                        "members do not match wall bindings"));
                    return;
                }
                members = derived;
                if (members.empty()) {
                    if (b) json_object_put(b);
                    send_json(res, fail_envelope("RESOURCE_IN_USE", 409,
                        "wall has no active bindings"));
                    return;
                }
                if (!scene_id.empty()) {
                    json_object* sc = g_store->get_doc("scenes", scene_id);
                    std::string sw;
                    if (sc) {
                        sw = body_str(sc, "wallId");
                        if (sw.empty()) sw = body_str(sc, "wall");
                        json_object_put(sc);
                    }
                    if (!sc || sw != wall_id) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                            "scene does not belong to wall"));
                        return;
                    }
                }
                if (rev <= wall_applied) {
                    if (b) json_object_put(b);
                    send_json(res, fail_envelope("EXPIRED", 410, "revision not newer"));
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    if (g_ops.wall_has_preparing(wall_id, opid)) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope("CONFLICT", 409,
                            "wall already has an operation in flight"));
                        return;
                    }
                    int committed = g_ops.wall_committed_revision(wall_id);
                    if (rev <= committed) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope("EXPIRED", 410, "revision not newer"));
                        return;
                    }
                }
                // Clock gate before anything is registered.
                for (const auto& mid : members) {
                    json_object* nd = g_store->get_doc("node", mid);
                    std::string reason;
                    bool ok = node_clock_ok(nd, &reason);
                    if (nd) json_object_put(nd);
                    if (!ok) {
                        if (b) json_object_put(b);
                        g_store->audit("{\"clock-blocked\":\"" + json_escape(mid) +
                                       "\",\"reason\":\"" + json_escape(reason) + "\"}");
                        send_json(res, fail_envelope("CLOCK_UNSYNCED", 409,
                            std::string("node ") + mid + ": " + reason));
                        return;
                    }
                }
            }
            json_object* wall = nullptr;
            if (b) json_object_object_get_ex(b, "wall", &wall);
            if (wall_id.empty() && wall) {
                wall_w = rect_int(wall, "w");
                wall_h = rect_int(wall, "h");
                double rf = 0;
                if (b && json_object_object_get_ex(b, "fps", &v)) rf = json_object_get_double(v);
                if (rf > 0.0) fps = rf;
            }
            json_object* windows = nullptr;
            if (b) json_object_object_get_ex(b, "windows", &windows);
            // Admission before anything is registered or fanned out: the
            // whole order is rejected on any violation (the same-frame
            // invariant forbids partial walls, see
            // DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md §4 准入与排序).
            {
                int win_n = (windows && json_object_is_type(windows, json_type_array))
                                ? (int)json_object_array_length(windows) : 0;
                // Per-node plane budget (never wall-global): the overlay limit
                // is per member canvas. Windows spread over many nodes are
                // fine; only one node intersecting more than the budget rejects
                // the whole order. Members without stored geometry stay
                // missing-report at fanout and are skipped here.
                {
                    std::vector<std::pair<std::string, Rect>> known;
                    for (const auto& mid : members) {
                        json_object* nd = g_store->get_doc("node", mid);
                        json_object* geo = nullptr;
                        if (nd) json_object_object_get_ex(nd, "geometry", &geo);
                        bool ok = geo && rect_int(geo, "w") > 0 && rect_int(geo, "h") > 0;
                        if (ok)
                            known.emplace_back(mid, Rect{rect_int(geo, "x"), rect_int(geo, "y"),
                                                         rect_int(geo, "w"), rect_int(geo, "h")});
                        if (nd) json_object_put(nd);
                    }
                    std::string budget_err = validate_per_node_budget(windows, known);
                    if (!budget_err.empty()) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope("INVALID_ARGUMENT", 400, budget_err));
                        return;
                    }
                    // Decode budget: no member may be asked for more distinct
                    // signals than its reported decode ceiling. Unknown
                    // budgets never gate (capability not yet reported).
                    for (const auto& kv : known) {
                        json_object* nd = g_store->get_doc("node", kv.first);
                        int budget = node_decode_budget(nd);
                        if (nd) json_object_put(nd);
                        if (budget <= 0) continue;
                        int sigs = count_node_signals(windows, kv.second);
                        if (sigs > budget) {
                            if (b) json_object_put(b);
                            send_json(res, fail_envelope(ct::media_err::kCapabilityExceeded,
                                422, std::string("node ") + kv.first + " needs " +
                                std::to_string(sigs) + " decodes, budget " +
                                std::to_string(budget)));
                            return;
                        }
                    }
                }
                bool need_wall = win_n > 0;
                for (int i = 0; i < win_n; i++) {
                    json_object* w = json_object_array_get_idx(windows, i);
                    int wx = rect_int(w, "x"), wy = rect_int(w, "y");
                    int ww = rect_int(w, "w"), wh = rect_int(w, "h");
                    // Minimal size: wall floor (64px, or 1/16 of the smallest
                    // cell by area) so a degenerate window can never reach a
                    // node.
                    if (ww < wall_min_side || wh < wall_min_side) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope(
                                           "INVALID_ARGUMENT", 400,
                                           "window below minimum size"));
                        return;
                    }
                    // Canvas sanity: windows are already wall pixels (the
                    // frontend converts on save), so anything outside the
                    // declared wall means a conversion bug — reject the order.
                    if (need_wall && (wall_w <= 0 || wall_h <= 0 || wx < 0 || wy < 0 ||
                                      wx + ww > wall_w || wy + wh > wall_h)) {
                        if (b) json_object_put(b);
                        send_json(res, fail_envelope(
                                           "INVALID_ARGUMENT", 400,
                                           "window outside wall canvas"));
                        return;
                    }
                }
            }
            int volume = 80;
            if (b && json_object_object_get_ex(b, "volume", &v) && v)
                volume = json_object_get_int(v);
            if (!validate_volume(volume).empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                    "volume must be 0..100"));
                return;
            }
            if ((int)members.size() > max_wall_members()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                    "wall membership exceeds limit"));
                return;
            }
            uint64_t apply_ns = layout_apply_at_ns(now_ns(), fps);
            uint64_t deadline = now_ns() + (uint64_t)ct::barrier_timeout_ms() * 1000000ULL;
            // Frame anchor: every member maps the same applyAtNs to the same
            // target frame through the shared wall epoch. Rollback reissues
            // use immediate commits (frame_index -1, node takes current).
            int64_t frame_index = -1;
            if (!wall_id.empty() && wall_epoch > 0)
                frame_index = ct::frame_index_for_apply_at((uint64_t)wall_epoch,
                                                            fps, apply_ns);
            std::string op_id_copy; uint64_t apply_copy = 0;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                Operation& op = g_ops.begin_with_deadline(opid, rev, members, apply_ns, deadline);
                op_id_copy = op.op_id; apply_copy = op.apply_at_ns;
            }

            const char* win_s = (windows && json_object_is_type(windows, json_type_array))
                ? json_object_to_json_string(windows) : "[]";
            std::string windows_json = win_s ? win_s : "[]";
            uint64_t apply_at = apply_copy;
            std::string opid_c = op_id_copy;
            std::string token_c = g_api_token;
            int volume_c = volume;
            std::string wall_c = wall_id;
            std::string scene_c = scene_id;
            std::string hash_c;
            {
                json_object* wtmp = json_tokener_parse(windows_json.c_str());
                hash_c = layout_windows_hash(wtmp);
                if (wtmp) json_object_put(wtmp);
            }
            // Stamp the fanout snapshot + created_ns for persistence and
            // rollback reissue (execute_rollback reads it back).
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                if (Operation* op2 = g_ops.mutable_find(opid_c)) {
                    op2->windows_json = windows_json;
                    op2->wall_w = wall_w;
                    op2->wall_h = wall_h;
                    op2->created_ns = now_ns();
                    op2->wall_id = wall_c;
                    op2->scene_id = scene_c;
                    op2->layout_hash = hash_c;
                    op2->volume = volume_c;
                    op2->epoch_ns = (uint64_t)wall_epoch;
                    op2->fps = fps;
                    op2->frame_index = frame_index;
                }
                g_ops.persist(opid_c);
            }
            if (members.size() > 32)
                g_store->audit("{\"fanout-scale\":\"" + json_escape(opid_c) + "\",\"members\":" +
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
                    g_store->audit("{\"fanout-skip\":\"" + json_escape(mid) +
                                   "\",\"reason\":\"no channel/geometry\"}");
                }
                if (nd) json_object_put(nd);
            }
            std::thread([opid_c, rev, apply_at, members, windows_json, volume_c,
                           token_c, wall_c, scene_c, hash_c, wall_epoch, fps,
                           frame_index, wall_w, wall_h, wall_base_json,
                           wall_adjust_json]() {
                fanout_layout_parallel(members, opid_c, rev, (int64_t)apply_at,
                                       windows_json, volume_c, token_c,
                                       wall_c, scene_c, hash_c, wall_epoch, fps,
                                       frame_index, wall_w, wall_h, wall_base_json,
                                       wall_adjust_json, false);
            }).detach();

            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "operationId", json_object_new_string(opid_c.c_str()));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64((int64_t)apply_at));
            json_object_object_add(d, "state", json_object_new_string("preparing"));
            json_object_object_add(d, "missing", missing);
            json_object_object_add(d, "layoutHash",
                                   json_object_new_string(hash_c.c_str()));
            if (frame_index >= 0)
                json_object_object_add(d, "frameIndex",
                                       json_object_new_int64(frame_index));
            if (!pending.empty()) {
                json_object* pa = json_object_new_array();
                for (auto& p : pending)
                    json_object_array_add(pa, json_object_new_string(p.c_str()));
                json_object_object_add(d, "pending", pa);
            }
            send_json(res, ok_envelope(d, "preparing"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/ops/commit") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string verr = b ? ct::validate_barrier_ack(b) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            std::string opid = body_str(b, "operationId");
            std::string node = body_str(b, "node");
            std::string st = body_str(b, "status");
            json_object* lanes = nullptr;
            if (b) json_object_object_get_ex(b, "lanes", &lanes);
            std::set<std::string> still_missing;
            std::string state = handle_barrier_ack(opid, node, st, lanes, still_missing, b);
            uint64_t apply_ns = 0;
            int64_t frame_index = -1;
            bool known = false;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                const Operation* op = g_ops.find(opid);
                if (op) {
                    apply_ns = op->apply_at_ns;
                    frame_index = op->frame_index;
                    known = true;
                }
            }
            if (b) json_object_put(b);
            if (!known) { send_json(res, fail_envelope("NOT_FOUND", 404, "op unknown")); return; }
            if (state == "blocked") {
                int to_rev = 0;
                std::string rb;
                std::set<std::string> rb_members;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    rb = g_ops.rollback_op(opid, &to_rev);
                    const Operation* op2 = g_ops.find(opid);
                    if (op2) rb_members = op2->members;
                }
                if (!rb.empty()) execute_rollback(rb, to_rev, rb_members);
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
            if (frame_index >= 0)
                json_object_object_add(d, "frameIndex",
                                       json_object_new_int64(frame_index));
            json_object* miss = json_object_new_array();
            for (const auto& m : still_missing)
                json_object_array_add(miss, json_object_new_string(m.c_str()));
            json_object_object_add(d, "missing", miss);
            send_json(res, ok_envelope(d, state));
            return;
        }
        // 短连接回退：配置确认与升级进度在无 WS 时走短连接，与 WS 同校验同审计。
        if (req.method == "POST" && req.path == "/api/v1/ops/config-ack") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string verr = b ? ct::validate_config_ack(b) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            g_store->audit("{\"config-ack\":\"" + json_escape(body_str(b, "node")) + "\"}");
            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "received", json_object_new_boolean(1));
            send_json(res, ok_envelope(d, ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/ops/upgrade-progress") {
            if (!require_bearer(req, res)) return;
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string verr = b ? ct::validate_upgrade_progress(b) : "INVALID_ARGUMENT:empty body";
            if (!verr.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            std::string tid = body_str(b, "taskId");
            std::string node = body_str(b, "node");
            std::string st = body_str(b, "status");
            { std::lock_guard<std::mutex> lk(g_chan.mu); g_chan.upgrade_progress[tid + "/" + node] = st; }
            g_store->audit("{\"upgrade-progress\":\"" + json_escape(node) + "\",\"task\":\"" + json_escape(tid) + "\",\"status\":\"" + json_escape(st) + "\"}");
            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "received", json_object_new_boolean(1));
            send_json(res, ok_envelope(d, ""));
            return;
        }
        // 升级改为任务队列驱动：handler 只入队，worker 串行经 WS/短连接投递。
        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            if (!require_bearer(req, res)) return;
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
            g_store->audit("{\"upgrade-enqueue\":\"" + json_escape(task_id) + "\"}");
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
        // Operation status query: state, applyAt, missing, wall/scene/hash.
        // Long-poll form (?wait=1&timeoutMs=N, N capped at 30s): the call
        // returns once the barrier leaves preparing or the budget expires,
        // so browsers replace fixed-interval polling with one hanging read.
        if (req.method == "GET" && req.path.rfind("/api/v1/ops/", 0) == 0) {
            std::string opid = req.path.substr(strlen("/api/v1/ops/"));
            if (!opid.empty() && opid.find('/') == std::string::npos) {
                bool wait = req.query.count("wait") && req.query.at("wait") == "1";
                long wait_ms = 10000;
                if (wait && req.query.count("timeoutMs")) {
                    try { wait_ms = std::stol(req.query.at("timeoutMs")); }
                    catch (...) { wait_ms = 10000; }
                    if (wait_ms < 0) wait_ms = 0;
                    if (wait_ms > 30000) wait_ms = 30000;
                }
                const Operation* op = nullptr;
                std::set<std::string> miss;
                if (wait) {
                    auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(wait_ms);
                    for (;;) {
                        std::string st;
                        {
                            std::lock_guard<std::mutex> lk(g_ops_mu);
                            op = g_ops.find(opid);
                            if (op) st = op->state;
                        }
                        if (!op || st != "preparing") break;
                        if (std::chrono::steady_clock::now() >= deadline) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    op = g_ops.find(opid);
                    if (op) miss = g_ops.missing(opid);
                }
                if (!op) {
                    send_json(res, fail_envelope("NOT_FOUND", 404, "op unknown"));
                    return;
                }
                json_object* d = json_object_new_object();
                json_object_object_add(d, "operationId",
                    json_object_new_string(op->op_id.c_str()));
                json_object_object_add(d, "state",
                    json_object_new_string(op->state.c_str()));
                json_object_object_add(d, "revision", json_object_new_int(op->revision));
                json_object_object_add(d, "applyAt",
                    json_object_new_int64((int64_t)op->apply_at_ns));
                json_object_object_add(d, "wallId",
                    json_object_new_string(op->wall_id.c_str()));
                json_object_object_add(d, "sceneId",
                    json_object_new_string(op->scene_id.c_str()));
                json_object_object_add(d, "layoutHash",
                    json_object_new_string(op->layout_hash.c_str()));
                json_object_object_add(d, "volume",
                    json_object_new_int(op->volume));
                json_object_object_add(d, "epochTaiNs",
                    json_object_new_int64((int64_t)op->epoch_ns));
                json_object_object_add(d, "fps",
                    json_object_new_double(op->fps));
                json_object_object_add(d, "frameIndex",
                    json_object_new_int64(op->frame_index));
                json_object* ma = json_object_new_array();
                for (auto& m : miss)
                    json_object_array_add(ma, json_object_new_string(m.c_str()));
                json_object_object_add(d, "missing", ma);
                send_json(res, ok_envelope(d, op->state));
                return;
            }
        }
        // Scene dirty compare (read-only): {sceneId, windows, volume}.
        if (req.method == "POST" && req.path == "/api/v1/scenes/compare") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string sid = body_str(b, "sceneId");
            if (sid.empty()) sid = body_str(b, "id");
            json_object* live_wins = nullptr;
            if (b) json_object_object_get_ex(b, "windows", &live_wins);
            int live_vol = 80;
            json_object* vv = nullptr;
            if (b && json_object_object_get_ex(b, "volume", &vv) && vv)
                live_vol = json_object_get_int(vv);
            json_object* sc = sid.empty() ? nullptr : g_store->get_doc("scenes", sid);
            if (!sc) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("NOT_FOUND", 404, "scene unknown"));
                return;
            }
            json_object* saved_wins = nullptr;
            json_object_object_get_ex(sc, "windows", &saved_wins);
            int saved_vol = 80;
            json_object* sv = nullptr;
            if (json_object_object_get_ex(sc, "volume", &sv) && sv)
                saved_vol = json_object_get_int(sv);
            json_object* d = layout_dirty_compare(saved_wins, saved_vol,
                                                  live_wins, live_vol);
            json_object_put(sc);
            if (b) json_object_put(b);
            send_json(res, ok_envelope(d, "compared"));
            return;
        }
        // Wall restore after soft delete (bindings stay cleared).
        if (req.method == "POST" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
            req.path.size() > strlen("/api/v1/walls/") + 8 &&
            req.path.rfind("/restore") == req.path.size() - 8) {
            std::string wid = req.path.substr(strlen("/api/v1/walls/"));
            wid = wid.substr(0, wid.size() - 8);
            json_object* w = g_store->get_doc("walls", wid);
            if (!w) {
                send_json(res, fail_envelope("NOT_FOUND", 404, "wall unknown"));
                return;
            }
            if (!wall_is_deleted(w)) {
                json_object_put(w);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "wall not deleted"));
                return;
            }
            json_object_object_del(w, "deleted");
            json_object_object_del(w, "deletedAt");
            g_store->put_doc("walls", wid, w);
            json_object_put(w);
            g_store->audit("{\"wall-restore\":\"" + json_escape(wid) + "\"}");
            json_object* d = g_store->get_doc("walls", wid);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), "restored"));
            return;
        }
        // Wallpaper sub-image descriptor: intersect wall canvas with node canvas.
        // Exact "/base-image" suffix so "/base-image-version" below is not
        // hijacked by this branch.
        if (req.method == "GET" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
            req.path.size() >= 11 &&
            req.path.rfind("/base-image") == req.path.size() - 11) {
            std::string mid = req.path.substr(strlen("/api/v1/walls/"));
            size_t sl = mid.find('/');
            std::string wid = (sl == std::string::npos) ? mid : mid.substr(0, sl);
            std::string node_id;
            auto qi = req.query.find("node");
            if (qi != req.query.end()) node_id = qi->second;
            json_object* w = g_store->get_doc("walls", wid);
            if (!w || wall_is_deleted(w)) {
                if (w) json_object_put(w);
                send_json(res, fail_envelope("NOT_FOUND", 404, "wall unknown or deleted"));
                return;
            }
            int ww = 0, hh = 0;
            wall_px_of(w, &ww, &hh);
            std::string bver = body_str(w, "baseImageRevision");
            if (bver.empty()) bver = body_str(w, "backdropRevision");
            json_object_put(w);
            if (node_id.empty()) {
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "need ?node="));
                return;
            }
            json_object* nd = g_store->get_doc("node", node_id);
            if (!nd) {
                send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown"));
                return;
            }
            json_object* geo = nullptr;
            json_object_object_get_ex(nd, "geometry", &geo);
            Rect node{rect_int(geo, "x"), rect_int(geo, "y"),
                      rect_int(geo, "w"), rect_int(geo, "h")};
            json_object_put(nd);
            Rect sub;
            if (!base_subrect(ww, hh, node, &sub)) {
                send_json(res, fail_envelope("NOT_FOUND", 404, "node outside wall canvas"));
                return;
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "wallId", json_object_new_string(wid.c_str()));
            json_object_object_add(d, "nodeId", json_object_new_string(node_id.c_str()));
            json_object_object_add(d, "baseImageRevision",
                                   json_object_new_string(bver.c_str()));
            json_object_object_add(d, "revision",
                                   json_object_new_string(bver.c_str()));
            std::string cache_key = wid + ":" + bver + ":" + node_id;
            json_object_object_add(d, "cacheKey",
                                   json_object_new_string(cache_key.c_str()));
            json_object_object_add(d, "wallW", json_object_new_int(ww));
            json_object_object_add(d, "wallH", json_object_new_int(hh));
            json_object* r = json_object_new_object();
            json_object_object_add(r, "x", json_object_new_int(sub.x));
            json_object_object_add(r, "y", json_object_new_int(sub.y));
            json_object_object_add(r, "w", json_object_new_int(sub.w));
            json_object_object_add(r, "h", json_object_new_int(sub.h));
            json_object_object_add(d, "subRect", r);
            send_json(res, ok_envelope(d, "base-image"));
            return;
        }
        // Upgrade progress aggregation per task.
        if (req.method == "GET" && req.path.rfind("/api/v1/nodes/upgrade/", 0) == 0) {
            std::string tid = req.path.substr(strlen("/api/v1/nodes/upgrade/"));
            json_object* d = json_object_new_object();
            json_object* items = json_object_new_array();
            int done = 0, total = 0;
            {
                std::lock_guard<std::mutex> lk(g_chan.mu);
                for (auto& kv : g_chan.upgrade_progress) {
                    const std::string& k = kv.first;
                    if (k.rfind(tid + "/", 0) != 0) continue;
                    std::string node = k.substr(tid.size() + 1);
                    json_object* e = json_object_new_object();
                    json_object_object_add(e, "node",
                        json_object_new_string(node.c_str()));
                    json_object_object_add(e, "status",
                        json_object_new_string(kv.second.c_str()));
                    json_object_array_add(items, e);
                    total++;
                    if (kv.second == "done" || kv.second == "succeeded") done++;
                }
            }
            json_object_object_add(d, "taskId", json_object_new_string(tid.c_str()));
            json_object_object_add(d, "items", items);
            json_object_object_add(d, "done", json_object_new_int(done));
            json_object_object_add(d, "total", json_object_new_int(total));
            send_json(res, ok_envelope(d, "upgrade-progress"));
            return;
        }
        // Playback task transitions: start/cancel/finish map to state machine.
        if (req.method == "POST" && req.path.rfind("/api/v1/playback-tasks/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/playback-tasks/"));
            size_t sl = rest.find('/');
            std::string tid = (sl == std::string::npos) ? rest : rest.substr(0, sl);
            std::string verb = (sl == std::string::npos) ? "" : rest.substr(sl + 1);
            if (!tid.empty() && (verb == "start" || verb == "cancel" || verb == "finish")) {
                json_object* t = g_store->get_doc("playback-tasks", tid);
                if (!t) {
                    send_json(res, fail_envelope("NOT_FOUND", 404, "task unknown"));
                    return;
                }
                std::string from = playback_status_of(t);
                std::string want;
                if (verb == "start") want = "executing";
                else if (verb == "cancel") want = "cancelled";
                else want = "succeeded";
                // Start validates the wall/scene/node triple before moving.
                if (verb == "start" && from == "pending_execution") {
                    std::string wall_id = body_str(t, "wallId");
                    std::string node = body_str(t, "targetNodeId");
                    json_object* w = wall_id.empty() ? nullptr
                                                     : g_store->get_doc("walls", wall_id);
                    json_object* nd = node.empty() ? nullptr
                                                  : g_store->get_doc("node", node);
                    bool ok = (w && !wall_is_deleted(w) && nd);
                    if (w) json_object_put(w);
                    if (nd) json_object_put(nd);
                    if (!ok) {
                        json_object_put(t);
                        send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                            "wall/node unavailable for start"));
                        return;
                    }
                }
                if (!playback_transition_ok(from, want)) {
                    json_object_put(t);
                    send_json(res, fail_envelope("CONFLICT", 409,
                        "illegal playback transition"));
                    return;
                }
                json_object_object_del(t, "status");
                json_object_object_add(t, "status", json_object_new_string(want.c_str()));
                g_store->put_doc("playback-tasks", tid, t);
                json_object_put(t);
                g_store->audit("{\"playback-" + verb + "\":\"" + json_escape(tid) + "\"}");
                json_object* d = g_store->get_doc("playback-tasks", tid);
                send_json(res, ok_envelope(d ? d : json_object_new_object(), verb));
                return;
            }
        }
        // Multicast pool allocation: caller supplies only the signal (and an
        // optional pinned port); the server picks the first free pool pair
        // and creates the whitelist entry. Idempotent by signal: a retry for
        // an already-allocated signal returns the existing entry.
        if (req.method == "POST" && req.path == "/api/v1/whitelist/allocate") {
            json_object* b = json_tokener_parse(req.body.c_str());
            if (!b) b = json_object_new_object();
            std::string sig = body_str(b, "sig");
            int want_port = 0;
            json_object* pv = nullptr;
            if (json_object_object_get_ex(b, "port", &pv) && pv)
                want_port = json_object_get_int(pv);
            std::string want_addr = body_str(b, "address");
            if (!sig.empty()) {
                json_object* hit = nullptr;
                for (auto& wid : g_store->list_ids("whitelist")) {
                    json_object* o = g_store->get_doc("whitelist", wid);
                    if (!o) continue;
                    if (body_str(o, "sig") == sig) { hit = o; break; }
                    json_object_put(o);
                }
                if (hit) {
                    json_object_put(b);
                    send_json(res, ok_envelope(hit, "already allocated"));
                    return;
                }
            }
            std::set<std::pair<std::string, int>> used;
            for (auto& wid : g_store->list_ids("whitelist")) {
                json_object* o = g_store->get_doc("whitelist", wid);
                if (!o) continue;
                std::string a;
                int p = 0;
                whitelist_addr_port(o, &a, &p);
                if (p <= 0) p = 50010;
                if (!a.empty()) used.insert({a, p});
                json_object_put(o);
            }
            std::string addr;
            int port = 0;
            if (!want_addr.empty()) {
                if (want_port <= 0) want_port = 50010;
                if (want_port < 1 || want_port > 65535) {
                    json_object_put(b);
                    send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                        "port must be 1..65535"));
                    return;
                }
                if (used.count({want_addr, want_port}) != 0) {
                    json_object_put(b);
                    send_json(res, fail_envelope("CONFLICT", 409,
                        "multicast address+port already used"));
                    return;
                }
                addr = want_addr;
                port = want_port;
            } else {
                if (!multicast_pool_next(used, want_port, &addr, &port)) {
                    json_object_put(b);
                    send_json(res, fail_envelope("RESOURCE_EXHAUSTED", 507,
                        "multicast pool exhausted"));
                    return;
                }
            }
            std::string id = sig;
            if (id.empty()) {
                char nb[48];
                snprintf(nb, sizeof(nb), "wl-%ld", (long)now_sec());
                id = nb;
                sig = id;
            }
            json_object* doc = json_object_new_object();
            json_object_object_add(doc, "id", json_object_new_string(id.c_str()));
            json_object_object_add(doc, "sig", json_object_new_string(sig.c_str()));
            json_object_object_add(doc, "address", json_object_new_string(addr.c_str()));
            json_object_object_add(doc, "port", json_object_new_int(port));
            for (const char* k : {"codec", "width", "height", "fps",
                                    "audioPort",
                                    "bitrateKbps", "bitrate", "keyFrameInterval",
                                    "gop", "mode"}) {
                json_object* kv = nullptr;
                if (json_object_object_get_ex(b, k, &kv) && kv)
                    json_object_object_add(doc, k, json_object_get(kv));
            }
            std::string verr = validate_whitelist_shape(doc);
            if (!verr.empty()) {
                json_object_put(doc);
                json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, verr.c_str()));
                return;
            }
            json_object_object_add(doc, "status", json_object_new_string("active"));
            json_object_put(b);
            g_store->put_doc("whitelist", id, doc);
            g_store->audit("{\"whitelist-allocate\":\"" + json_escape(id) + "\"}");
            json_object* d = g_store->get_doc("whitelist", id);
            json_object_put(doc);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), "allocated"));
            return;
        }
        // Wallpaper upload: store the base-image record and stamp the wall.
        if (req.method == "POST" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
            req.path.size() >= 11 &&
            req.path.rfind("/base-image") == req.path.size() - 11) {
            std::string wid = req.path.substr(strlen("/api/v1/walls/"));
            wid = wid.substr(0, wid.size() - 11);
            json_object* w = g_store->get_doc("walls", wid);
            if (!w || wall_is_deleted(w)) {
                if (w) json_object_put(w);
                send_json(res, fail_envelope("NOT_FOUND", 404, "wall unknown or deleted"));
                return;
            }
            json_object_put(w);
            json_object* b = json_tokener_parse(req.body.c_str());
            if (!b) b = json_object_new_object();
            std::string rev = body_str(b, "revision");
            if (rev.empty()) rev = body_str(b, "baseImageRevision");
            int bw = 0, bh = 0;
            json_object* v = nullptr;
            if (json_object_object_get_ex(b, "width", &v) && v) bw = json_object_get_int(v);
            if (json_object_object_get_ex(b, "height", &v) && v) bh = json_object_get_int(v);
            std::string url = body_str(b, "url");
            bool bad = rev.empty() || rev.size() > 64 ||
                         (bw != 0 && (bw < 64 || bw > 7680)) ||
                         (bh != 0 && (bh < 64 || bh > 4320)) || url.size() > 512;
            if (bad) {
                json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400,
                    "need revision[1..64] + optional width/height/url in range"));
                return;
            }
            json_object* doc = json_object_new_object();
            json_object_object_add(doc, "id", json_object_new_string(wid.c_str()));
            json_object_object_add(doc, "wallId", json_object_new_string(wid.c_str()));
            json_object_object_add(doc, "revision", json_object_new_string(rev.c_str()));
            if (bw > 0) json_object_object_add(doc, "width", json_object_new_int(bw));
            if (bh > 0) json_object_object_add(doc, "height", json_object_new_int(bh));
            if (!url.empty())
                json_object_object_add(doc, "url", json_object_new_string(url.c_str()));
            json_object_object_add(doc, "updatedAt",
                                   json_object_new_string(now_iso().c_str()));
            json_object_put(b);
            g_store->put_doc("base-images", wid, doc);
            json_object_put(doc);
            json_object* cur = g_store->get_doc("walls", wid);
            if (cur) {
                json_object_object_del(cur, "baseImageRevision");
                json_object_object_add(cur, "baseImageRevision",
                                       json_object_new_string(rev.c_str()));
                g_store->put_doc("walls", wid, cur);
                json_object_put(cur);
            }
            g_store->audit("{\"base-image\":\"" + json_escape(wid) + "\"}");
            json_object* d = g_store->get_doc("base-images", wid);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), "stored"));
            return;
        }
        // Wallpaper version query for node pull (startup / settings change).
        if (req.method == "GET" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
            req.path.size() >= 19 &&
            req.path.rfind("/base-image-version") == req.path.size() - 19) {
            std::string wid = req.path.substr(strlen("/api/v1/walls/"));
            wid = wid.substr(0, wid.size() - 19);
            json_object* w = g_store->get_doc("walls", wid);
            if (!w || wall_is_deleted(w)) {
                if (w) json_object_put(w);
                send_json(res, fail_envelope("NOT_FOUND", 404, "wall unknown or deleted"));
                return;
            }
            json_object_put(w);
            json_object* d = g_store->get_doc("base-images", wid);
            if (!d) {
                send_json(res, fail_envelope("NOT_FOUND", 404, "no base image"));
                return;
            }
            std::string rev = body_str(d, "revision");
            json_object_object_add(d, "wallId",
                                   json_object_new_string(wid.c_str()));
            json_object_object_add(d, "cacheKey",
                                   json_object_new_string((wid + ":" + rev).c_str()));
            send_json(res, ok_envelope(d, "base-image-version"));
            return;
        }
        const char* kinds[] = {"groups", "walls", "scenes", "bindings",
                               "whitelist", "playback-tasks"};
        DocsCtx docs_ctx{g_store, &g_ops, &g_ops_mu};
        for (auto k : kinds)
            if (route_docs(docs_ctx, k, req, res)) return;
        // static files share the common traversal-safe server
        if (req.method == "GET" &&
            HttpServer::serve_static_file(g_web_root, req.path, res))
            return;
        send_json(res, fail_envelope("NOT_FOUND", 404, "unknown api"));
    };

    if (!web_root_arg.empty()) g_web_root = web_root_arg;
    if (g_web_root.empty()) {
        const char* env = getenv("DVS_WEB_ROOT");
        if (env && *env) g_web_root = env;
    }
    if (g_web_root.empty()) g_web_root = "./web/src";
    if (g_api_token.empty())
        fprintf(stderr, "dvs-platform: WARNING DVS_API_TOKEN unset, "
                        "mutating APIs accept unauthenticated calls\n");
    HttpServer server(port, h);
    server.set_ws_handler("/api/v1/channel/ws", ws_channel_loop);
    // 屏障超时回收：超时未齐按阻塞处理并回滚，避免永久停留 preparing。
    std::thread([]() {
        time_t last_gc = 0;
        const uint64_t kOpTtlNs = 24ULL * 3600 * 1000000000ULL;
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
                std::set<std::string> rb_members;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    rb = g_ops.rollback_op(id, &to_rev);
                    const Operation* op2 = g_ops.find(id);
                    if (op2) rb_members = op2->members;
                }
                g_store->audit("{\"barrier-timeout\":\"" + json_escape(id) + "\"}");
                if (!rb.empty()) execute_rollback(rb, to_rev, rb_members);
            }
            // Replay rollbacks for blocked ops persisted without one — the
            // crash window between the blocked persist and execute_rollback.
            // rollback_op stamps __rolledback__, so replays are idempotent.
            std::vector<std::string> replay;
            {
                std::lock_guard<std::mutex> lk(g_ops_mu);
                replay = g_ops.pending_rollbacks();
            }
            for (auto& id : replay) {
                int to_rev = 0;
                std::string rb;
                std::set<std::string> rb_members;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    rb = g_ops.rollback_op(id, &to_rev);
                    const Operation* op2 = g_ops.find(id);
                    if (op2) rb_members = op2->members;
                }
                g_store->audit("{\"op-rollback-replay\":\"" + json_escape(id) +
                               "\",\"rollback\":\"" + json_escape(rb) + "\"}");
                if (!rb.empty()) execute_rollback(rb, to_rev, rb_members);
            }
            // Hourly: drop terminal-state ops older than 24h (memory + docs),
            // and hard-delete soft-deleted walls past 7 days.
            time_t now_s = time(nullptr);
            if (last_gc == 0 || now_s - last_gc >= 3600) {
                last_gc = now_s;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mu);
                    g_ops.gc(now_ns(), kOpTtlNs);
                }
                for (auto& wid : g_store->list_ids("walls")) {
                    json_object* w = g_store->get_doc("walls", wid);
                    if (!w) continue;
                    bool gone = false;
                    if (wall_is_deleted(w)) {
                        // deletedAt is ISO-8601; without a clock parse, use the
                        // doc as-is: sweeper only enforces the tombstone path
                        // via audit age. Hard delete is conservative here and
                        // runs only when the wall carries an explicit epoch.
                        json_object* v = nullptr;
                        if (json_object_object_get_ex(w, "deletedAtEpoch", &v) && v) {
                            int64_t at = json_object_get_int64(v);
                            if (at > 0 && now_s - (time_t)at > 7 * 24 * 3600) gone = true;
                        }
                    }
                    json_object_put(w);
                    if (gone) {
                        g_store->del_doc("walls", wid);
                        g_store->del_doc("base-images", wid);
                        g_store->audit("{\"wall-gc\":\"" + json_escape(wid) + "\"}");
                    }
                }
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
                    g_store->audit("{\"upgrade-push\":\"" + json_escape(nid) + "\",\"via\":\"ws\"}");
                    json_object_put(task);
                    continue;
                }
                json_object* nd = g_store->get_doc("node", nid);
                std::string base = nd ? body_str(nd, "agentBase") : "";
                if (nd) json_object_put(nd);
                if (base.empty()) {
                    g_store->audit("{\"upgrade-skip\":\"" + json_escape(nid) + "\"}");
                    json_object_put(task);
                    continue;
                }
                const char* tb = json_object_to_json_string(task);
                std::string body = tb ? tb : "{}";
                json_object_put(task);
                std::string resp;
                int sc = cli.post_json(base + "/api/v1/node/upgrade", body, resp, 10000, 2, g_api_token);
                g_store->audit("{\"upgrade-push\":\"" + json_escape(nid) + "\",\"status\":" + std::to_string(sc) + "}");
            }
        }
    }).detach();
    server.start();
    printf("dvs-platform listening on 0.0.0.0:%d\n", port);
    fflush(stdout);
    for (;;) pause();
    return 0;
}
