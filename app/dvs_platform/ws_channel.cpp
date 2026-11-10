#include "ws_channel.h"

#include <unistd.h>

#include <mutex>

#include "barrier.h"
#include "channels.h"
#include "dvs_contract.h"
#include "dvs_http_json.h"
#include "layout_fanout.h"
#include "node_state.h"
#include "platform_context.h"
#include "platform_store.h"

namespace ct = dvs::contract;

namespace {
std::string ws_idem_key(const std::string& msg_id, const std::string& tp,
                        json_object* data) {
    std::string node = dvs::json_get_str(data, "id");
    return msg_id + "#" + tp + "#" + node;
}
}  // namespace

void ws_channel_loop(PlatformContext& ctx, int fd, HttpRequest hs_req) {
    std::string hs_bearer = ct::extract_bearer(hs_req.headers);
    auto send_wrapped = [&](const std::string& idem_key, const std::string& type,
                            json_object* data) -> std::string {
        std::string hit;
        if (!idem_key.empty() && ctx.channels.idem_hit(idem_key, hit)) {
            std::lock_guard<std::mutex> lk(ctx.channels.send_mu);
            HttpServer::ws_send_text(fd, hit, false);
            return hit;
        }
        json_object* msg = ct::wrap(ct::next_msg_id("p"), type, data,
                                    ct::next_request_id("p"));
        const char* s = json_object_to_json_string(msg);
        std::string frame = s ? s : "{}";
        json_object_put(msg);
        {
            std::lock_guard<std::mutex> lk(ctx.channels.send_mu);
            HttpServer::ws_send_text(fd, frame, false);
        }
        if (!idem_key.empty()) ctx.channels.idem_store(idem_key, frame);
        return frame;
    };
    std::string first;
    if (!HttpServer::ws_recv_text(fd, first, 5000, false)) {
        ::close(fd);
        return;
    }
    json_object* jm = json_tokener_parse(first.c_str());
    std::string msg_id, type, req_id;
    json_object* data = nullptr;
    if (!jm || !ct::unwrap(jm, msg_id, type, data, req_id) ||
        type != ct::kHello) {
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
    bool auth =
        !ctx.api_token.empty() && ct::bearer_ok(provided, ctx.api_token);
    std::string node_id = dvs::json_get_str(data, "id");
    if (verr.empty() && !node_doc_ready(data))
        verr = "INVALID_ARGUMENT:node not ready: need agentBase + usable geometry";
    if (!verr.empty() || !auth || node_id.empty() ||
        ctx.store->tombstone_check(node_id, dvs::wall_sec_now())) {
        json_object* d = json_object_new_object();
        json_object_object_add(
            d, "reason",
            json_object_new_string(
                !auth ? "missing or bad token"
                      : (!verr.empty() ? verr.c_str() : "missing id")));
        send_wrapped(ws_idem_key(msg_id, type, data), ct::kError, d);
        if (data) json_object_put(data);
        json_object_put(jm);
        ::close(fd);
        return;
    }
    ctx.channels.register_conn(node_id, fd);
    apply_hello_doc(ctx, data);
    ctx.store->audit("{\"ws-hello\":\"" + dvs::json_escape(node_id) + "\"}");
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "heartbeatIntervalMs",
                               json_object_new_int(ct::heartbeat_interval_ms()));
        json_object_object_add(w, "barrierTimeoutMs",
                               json_object_new_int(ct::barrier_timeout_ms()));
        json_object_object_add(w, "id",
                               json_object_new_string(node_id.c_str()));
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
            std::lock_guard<std::mutex> lk(ctx.channels.send_mu);
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
            std::string hid = dvs::json_get_str(dd, "id");
            if (!e.empty() || hid.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(
                    d, "reason",
                    json_object_new_string(
                        e.empty() ? "bad heartbeat" : e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                apply_heartbeat_doc(ctx, hid, dd);
                json_object* ack = heartbeat_ack_for_node(ctx, hid);
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kBarrierAck) {
            std::string e = ct::validate_barrier_ack(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason",
                                       json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                std::string opid = dvs::json_get_str(dd, "operationId");
                std::string node = dvs::json_get_str(dd, "node");
                std::string st = dvs::json_get_str(dd, "status");
                json_object* lanes = nullptr;
                json_object_object_get_ex(dd, "lanes", &lanes);
                std::set<std::string> miss;
                std::string block_reason;
                std::string state = handle_barrier_ack(
                    ctx, opid, node, st,
                    (lanes && json_object_is_type(lanes, json_type_array))
                        ? lanes
                        : nullptr,
                    miss, dd, &block_reason);
                const Operation* op = nullptr;
                {
                    std::lock_guard<std::mutex> lk(ctx.ops_mu);
                    op = ctx.ops.find(opid);
                }
                json_object* ack = json_object_new_object();
                if (!op) {
                    json_object_object_add(ack, "state",
                                           json_object_new_string("unknown"));
                } else if (state == "blocked") {
                    int to_rev = 0;
                    std::string rb;
                    std::set<std::string> rb_members;
                    {
                        std::lock_guard<std::mutex> lk(ctx.ops_mu);
                        rb = ctx.ops.rollback_op(opid, &to_rev);
                        const Operation* op2 = ctx.ops.find(opid);
                        if (op2) rb_members = op2->members;
                    }
                    if (!rb.empty())
                        execute_rollback(ctx, rb, to_rev, rb_members);
                    json_object_object_add(ack, "rollback",
                                           json_object_new_string(rb.c_str()));
                    json_object_object_add(ack, "toRevision",
                                           json_object_new_int(to_rev));
                    if (!block_reason.empty())
                        json_object_object_add(
                            ack, "reason",
                            json_object_new_string(block_reason.c_str()));
                } else {
                    json_object_object_add(ack, "state",
                                           json_object_new_string(state.c_str()));
                    json_object* ma = json_object_new_array();
                    for (auto& mm : miss)
                        json_object_array_add(
                            ma, json_object_new_string(mm.c_str()));
                    json_object_object_add(ack, "missing", ma);
                }
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kUpgradeProgress) {
            std::string e = ct::validate_upgrade_progress(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason",
                                       json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                std::string tid = dvs::json_get_str(dd, "taskId");
                std::string node = dvs::json_get_str(dd, "node");
                std::string st = dvs::json_get_str(dd, "status");
                {
                    std::lock_guard<std::mutex> lk(ctx.channels.mu);
                    ctx.channels.upgrade_progress[tid + "/" + node] = st;
                }
                ctx.store->audit("{\"upgrade-progress\":\"" +
                                 dvs::json_escape(node) + "\",\"task\":\"" +
                                 dvs::json_escape(tid) + "\",\"status\":\"" +
                                 dvs::json_escape(st) + "\"}");
                json_object* ack = json_object_new_object();
                json_object_object_add(ack, "received",
                                       json_object_new_boolean(1));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else if (tp == ct::kConfigAck) {
            std::string e = ct::validate_config_ack(dd);
            if (!e.empty()) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "reason",
                                       json_object_new_string(e.c_str()));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
            } else {
                ctx.store->audit("{\"config-ack\":\"" +
                                 dvs::json_escape(dvs::json_get_str(dd, "node")) + "\"}");
                json_object* ack = json_object_new_object();
                json_object_object_add(ack, "received",
                                       json_object_new_boolean(1));
                send_wrapped(ws_idem_key(mid, tp, dd), ct::kAck, ack);
            }
        } else {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "reason",
                                   json_object_new_string("unknown type"));
            send_wrapped(ws_idem_key(mid, tp, dd), ct::kError, d);
        }
        if (dd) json_object_put(dd);
        if (m) json_object_put(m);
    }
    std::string gone = ctx.channels.node_of(fd);
    ctx.channels.unregister_fd(fd);
    if (!gone.empty()) {
        json_object* cur = ctx.store->get_doc("node", gone);
        if (cur) {
            json_object_object_add(cur, "online", json_object_new_boolean(0));
            ctx.store->put_doc("node", gone, cur);
            json_object_put(cur);
        }
        ctx.store->audit("{\"ws-offline\":\"" + dvs::json_escape(gone) + "\"}");
    }
    ::close(fd);
}
