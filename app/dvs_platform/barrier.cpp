#include "barrier.h"

#include <mutex>

#include "dvs_http_json.h"
#include "layout.h"
#include "node_state.h"
#include "platform_context.h"
#include "platform_store.h"

std::string handle_barrier_ack(PlatformContext& ctx, const std::string& opid,
                              const std::string& node, std::string st,
                              json_object* lanes,
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
        json_object* nd =
            ctx.store ? ctx.store->get_doc("node", node) : nullptr;
        std::string reason;
        bool ok = node_clock_ok(nd, &reason);
        if (nd) json_object_put(nd);
        if (!ok) {
            st = "blocked";
            if (ctx.store)
                ctx.store->audit("{\"commit-clock-blocked\":\"" +
                                 dvs::json_escape(node) + "\",\"reason\":\"" +
                                 dvs::json_escape(reason) + "\"}");
        }
    }
    if (ack) {
        int64_t want_frame = -2;
        uint64_t want_epoch = 0;
        {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            const Operation* op = ctx.ops.find(opid);
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
                if (ctx.store)
                    ctx.store->audit("{\"commit-frame-mismatch\":\"" +
                                     dvs::json_escape(node) + "\",\"want\":" +
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
                if (ctx.store)
                    ctx.store->audit("{\"commit-frame-late\":\"" +
                                     dvs::json_escape(node) + "\"}");
            }
        }
        std::string why = dvs::json_get_str(ack, "reason");
        if (!why.empty() && ctx.store)
            ctx.store->audit("{\"commit-reason\":\"" + dvs::json_escape(node) +
                             "\",\"reason\":\"" + dvs::json_escape(why) + "\"}");
    }
    std::string state;
    std::string commit_wall, commit_scene, commit_hash;
    int commit_rev = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.ops_mu);
        state = ctx.ops.on_reply(opid, node, st != "blocked", st);
        missing_out = ctx.ops.missing(opid);
        if (state == "committed") {
            const Operation* op = ctx.ops.find(opid);
            if (op) {
                commit_wall = op->wall_id;
                commit_scene = op->scene_id;
                commit_rev = op->revision;
                commit_hash = op->layout_hash;
            }
        }
    }
    if (state == "committed" && !commit_wall.empty())
        mark_wall_committed(ctx, commit_wall, commit_scene, commit_rev,
                            commit_hash);
    ctx.store->audit("{\"commit\":\"" + dvs::json_escape(node) + "\",\"state\":\"" +
                     dvs::json_escape(st) + "\"}");
    return state;
}
