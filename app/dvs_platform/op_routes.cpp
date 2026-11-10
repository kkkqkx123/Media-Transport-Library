#include "op_routes.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "barrier.h"
#include "dvs_contract.h"
#include "dvs_http_json.h"
#include "layout.h"
#include "layout_fanout.h"
#include "node_state.h"
#include "platform_context.h"
#include "platform_store.h"

namespace ct = dvs::contract;

bool route_ops(PlatformContext& ctx, const HttpRequest& req,
               HttpResponse& res) {
    if (req.method == "POST" && req.path == "/api/v1/ops/prepare") {
        if (!platform_mutation_allowed(ctx, req)) {
            dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                                   "missing or bad token"));
            return true;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string opid = dvs::json_get_str(b, "operationId");
        int rev = 1;
        json_object* v = nullptr;
        if (b && json_object_object_get_ex(b, "revision", &v))
            rev = json_object_get_int(v);
        std::string wall_id = dvs::json_get_str(b, "wallId");
        std::string scene_id = dvs::json_get_str(b, "sceneId");
        std::set<std::string> members;
        json_object* arr = nullptr;
        if (b && json_object_object_get_ex(b, "members", &arr) &&
            json_object_is_type(arr, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(arr); i++)
                members.insert(json_object_get_string(
                    json_object_array_get_idx(arr, i)));
        }
        if (opid.empty() || (members.empty() && wall_id.empty())) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "need operationId+members"));
            return true;
        }
        int wall_w = 0, wall_h = 0;
        double fps = default_fps();
        int wall_applied = 0;
        int64_t wall_epoch = 0;
        int wall_min_side = ct::kMinWindowSide;
        std::string wall_base_json;
        std::string wall_adjust_json;
        std::set<std::string> pending;
        if (!wall_id.empty()) {
            json_object* wdoc = ctx.store->get_doc("walls", wall_id);
            if (!wdoc || wall_is_deleted(wdoc)) {
                if (wdoc) json_object_put(wdoc);
                if (b) json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                             "wall unknown or deleted"));
                return true;
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
                    wall_epoch = (int64_t)dvs::wall_ns_now();
                    json_object_object_del(wdoc, "epochTaiNs");
                    json_object_object_add(wdoc, "epochTaiNs",
                                           json_object_new_int64(wall_epoch));
                    ctx.store->put_doc("walls", wall_id, wdoc);
                }
                json_object* biv = nullptr;
                if (json_object_object_get_ex(wdoc, "baseImage", &biv) && biv &&
                    json_object_is_type(biv, json_type_object) &&
                    validate_base_image(biv).empty()) {
                    const char* bs = json_object_to_json_string(biv);
                    if (bs) wall_base_json = bs;
                }
                json_object* iav = nullptr;
                if (json_object_object_get_ex(wdoc, "imageAdjust", &iav) &&
                    iav && json_object_is_type(iav, json_type_object) &&
                    validate_image_adjust(iav).empty()) {
                    const char* as = json_object_to_json_string(iav);
                    if (as) wall_adjust_json = as;
                }
            }
            json_object* av = nullptr;
            if (json_object_object_get_ex(wdoc, "appliedRevision", &av) && av)
                wall_applied = json_object_get_int(av);
            json_object_put(wdoc);
            std::set<std::string> derived =
                derive_wall_members(ctx, wall_id, &pending);
            if (!members.empty() && members != derived) {
                if (b) json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                             "members do not match wall "
                                             "bindings"));
                return true;
            }
            members = derived;
            if (members.empty()) {
                if (b) json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                                             "wall has no active bindings"));
                return true;
            }
            if (!scene_id.empty()) {
                json_object* sc = ctx.store->get_doc("scenes", scene_id);
                std::string sw;
                if (sc) {
                    sw = dvs::json_get_str(sc, "wallId");
                    json_object_put(sc);
                }
                if (!sc || sw != wall_id) {
                    if (b) json_object_put(b);
                    dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                                 "scene does not belong to "
                                                 "wall"));
                    return true;
                }
            }
            if (rev <= wall_applied) {
                if (b) json_object_put(b);
                dvs::send_json(res,
                          dvs::envelope_fail("EXPIRED", 410, "revision not newer"));
                return true;
            }
            {
                std::lock_guard<std::mutex> lk(ctx.ops_mu);
                if (ctx.ops.wall_has_preparing(wall_id, opid)) {
                    if (b) json_object_put(b);
                    dvs::send_json(res,
                              dvs::envelope_fail("CONFLICT", 409,
                                            "wall already has an operation in "
                                            "flight"));
                    return true;
                }
                int committed = ctx.ops.wall_committed_revision(wall_id);
                if (rev <= committed) {
                    if (b) json_object_put(b);
                    dvs::send_json(res, dvs::envelope_fail("EXPIRED", 410,
                                                 "revision not newer"));
                    return true;
                }
            }
            for (const auto& mid : members) {
                json_object* nd = ctx.store->get_doc("node", mid);
                std::string reason;
                bool ok = node_clock_ok(nd, &reason);
                if (nd) json_object_put(nd);
                if (!ok) {
                    if (b) json_object_put(b);
                    ctx.store->audit("{\"clock-blocked\":\"" + dvs::json_escape(mid) +
                                     "\",\"reason\":\"" + dvs::json_escape(reason) +
                                     "\"}");
                    dvs::send_json(res,
                              dvs::envelope_fail("CLOCK_UNSYNCED", 409,
                                            std::string("node ") + mid + ": " +
                                                reason));
                    return true;
                }
            }
        }
        json_object* wall = nullptr;
        if (b) json_object_object_get_ex(b, "wall", &wall);
        if (wall_id.empty() && wall) {
            wall_w = dvs::json_rect_int(wall, "w");
            wall_h = dvs::json_rect_int(wall, "h");
            double rf = 0;
            if (b && json_object_object_get_ex(b, "fps", &v))
                rf = json_object_get_double(v);
            if (rf > 0.0) fps = rf;
        }
        json_object* windows = nullptr;
        if (b) json_object_object_get_ex(b, "windows", &windows);
        {
            int win_n =
                (windows && json_object_is_type(windows, json_type_array))
                    ? (int)json_object_array_length(windows)
                    : 0;
            {
                std::vector<std::pair<std::string, Rect>> known;
                for (const auto& mid : members) {
                    json_object* nd = ctx.store->get_doc("node", mid);
                    json_object* geo = nullptr;
                    if (nd) json_object_object_get_ex(nd, "geometry", &geo);
                    bool ok =
                        geo && dvs::json_rect_int(geo, "w") > 0 && dvs::json_rect_int(geo, "h") > 0;
                    if (ok)
                        known.emplace_back(
                            mid, Rect{dvs::json_rect_int(geo, "x"), dvs::json_rect_int(geo, "y"),
                                      dvs::json_rect_int(geo, "w"), dvs::json_rect_int(geo, "h")});
                    if (nd) json_object_put(nd);
                }
                std::string budget_err =
                    validate_per_node_budget(windows, known);
                if (!budget_err.empty()) {
                    if (b) json_object_put(b);
                    dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                                 budget_err));
                    return true;
                }
                for (const auto& kv : known) {
                    json_object* nd = ctx.store->get_doc("node", kv.first);
                    int budget = node_decode_budget(nd);
                    if (nd) json_object_put(nd);
                    if (budget <= 0) continue;
                    int sigs = count_node_signals(windows, kv.second);
                    if (sigs > budget) {
                        if (b) json_object_put(b);
                        dvs::send_json(res, dvs::envelope_fail(
                                           ct::media_err::kCapabilityExceeded,
                                           422,
                                           std::string("node ") + kv.first +
                                               " needs " +
                                               std::to_string(sigs) +
                                               " decodes, budget " +
                                               std::to_string(budget)));
                        return true;
                    }
                }
            }
            bool need_wall = win_n > 0;
            for (int i = 0; i < win_n; i++) {
                json_object* w = json_object_array_get_idx(windows, i);
                int wx = dvs::json_rect_int(w, "x"), wy = dvs::json_rect_int(w, "y");
                int ww = dvs::json_rect_int(w, "w"), wh = dvs::json_rect_int(w, "h");
                if (ww < wall_min_side || wh < wall_min_side) {
                    if (b) json_object_put(b);
                    dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                                 "window below minimum size"));
                    return true;
                }
                if (need_wall && (wall_w <= 0 || wall_h <= 0 || wx < 0 ||
                                  wy < 0 || wx + ww > wall_w ||
                                  wy + wh > wall_h)) {
                    if (b) json_object_put(b);
                    dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                                 "window outside wall canvas"));
                    return true;
                }
            }
        }
        int volume = default_volume();
        if (b && json_object_object_get_ex(b, "volume", &v) && v)
            volume = json_object_get_int(v);
        if (!validate_volume(volume).empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "volume must be 0..100"));
            return true;
        }
        if ((int)members.size() > max_wall_members()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "wall membership exceeds limit"));
            return true;
        }
        uint64_t apply_ns = layout_apply_at_ns(dvs::wall_ns_now(), fps);
        uint64_t deadline =
            dvs::wall_ns_now() + (uint64_t)ct::barrier_timeout_ms() * 1000000ULL;
        int64_t frame_index = -1;
        if (!wall_id.empty() && wall_epoch > 0)
            frame_index = ct::frame_index_for_apply_at((uint64_t)wall_epoch,
                                                       fps, apply_ns);
        std::string op_id_copy;
        uint64_t apply_copy = 0;
        {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            Operation& op = ctx.ops.begin_with_deadline(opid, rev, members,
                                                        apply_ns, deadline);
            op_id_copy = op.op_id;
            apply_copy = op.apply_at_ns;
        }
        const char* win_s =
            (windows && json_object_is_type(windows, json_type_array))
                ? json_object_to_json_string(windows)
                : "[]";
        std::string windows_json = win_s ? win_s : "[]";
        uint64_t apply_at = apply_copy;
        std::string opid_c = op_id_copy;
        std::string token_c = ctx.api_token;
        int volume_c = volume;
        std::string wall_c = wall_id;
        std::string scene_c = scene_id;
        std::string hash_c;
        {
            json_object* wtmp = json_tokener_parse(windows_json.c_str());
            hash_c = layout_windows_hash(wtmp);
            if (wtmp) json_object_put(wtmp);
        }
        {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            if (Operation* op2 = ctx.ops.mutable_find(opid_c)) {
                op2->windows_json = windows_json;
                op2->wall_w = wall_w;
                op2->wall_h = wall_h;
                op2->created_ns = dvs::wall_ns_now();
                op2->wall_id = wall_c;
                op2->scene_id = scene_c;
                op2->layout_hash = hash_c;
                op2->volume = volume_c;
                op2->epoch_ns = (uint64_t)wall_epoch;
                op2->fps = fps;
                op2->frame_index = frame_index;
            }
            ctx.ops.persist(opid_c);
        }
        if (members.size() > 32)
            ctx.store->audit("{\"fanout-scale\":\"" + dvs::json_escape(opid_c) +
                             "\",\"members\":" +
                             std::to_string(members.size()) + "}");
        json_object* missing = json_object_new_array();
        for (const auto& mid : members) {
            json_object* nd = ctx.store->get_doc("node", mid);
            std::string ab = nd ? dvs::json_get_str(nd, "agentBase") : "";
            bool has_chan = (ctx.channels.fd_of(mid) >= 0) || !ab.empty();
            std::string reason = node_shortfall_reason(nd, has_chan);
            if (!reason.empty()) {
                json_object_array_add(missing,
                                      node_shortfall_json(mid, reason));
                ctx.store->audit("{\"fanout-skip\":\"" + dvs::json_escape(mid) +
                                 "\",\"reason\":\"" +
                                 dvs::json_escape(reason) + "\"}");
            }
            if (nd) json_object_put(nd);
        }
        std::thread([opid_c, rev, apply_at, members, windows_json, volume_c,
                     token_c, wall_c, scene_c, hash_c, wall_epoch, fps,
                     frame_index, wall_w, wall_h, wall_base_json,
                     wall_adjust_json, deadline, &ctx]() {
            fanout_layout_parallel(ctx, members, opid_c, rev, (int64_t)apply_at,
                                   windows_json, volume_c, token_c, wall_c,
                                   scene_c, hash_c, wall_epoch, fps,
                                   frame_index, wall_w, wall_h, wall_base_json,
                                   wall_adjust_json, false, (int64_t)deadline);
        })
            .detach();
        if (b) json_object_put(b);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "operationId",
                               json_object_new_string(opid_c.c_str()));
        json_object_object_add(d, "applyAt",
                               json_object_new_int64((int64_t)apply_at));
        json_object_object_add(d, "state",
                               json_object_new_string("preparing"));
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
        dvs::send_json(res, dvs::envelope_ok(d, "preparing"));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/ops/commit") {
        if (!platform_mutation_allowed(ctx, req)) {
            dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                                   "missing or bad token"));
            return true;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string verr =
            b ? ct::validate_barrier_ack(b) : "INVALID_ARGUMENT:empty body";
        if (!verr.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        std::string opid = dvs::json_get_str(b, "operationId");
        std::string node = dvs::json_get_str(b, "node");
        std::string st = dvs::json_get_str(b, "status");
        json_object* lanes = nullptr;
        if (b) json_object_object_get_ex(b, "lanes", &lanes);
        std::set<std::string> still_missing;
        std::string block_reason;
        std::string state = handle_barrier_ack(ctx, opid, node, st, lanes,
                                              still_missing, b, &block_reason);
        uint64_t apply_ns = 0;
        int64_t frame_index = -1;
        bool known = false;
        {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            const Operation* op = ctx.ops.find(opid);
            if (op) {
                apply_ns = op->apply_at_ns;
                frame_index = op->frame_index;
                known = true;
            }
        }
        if (b) json_object_put(b);
        if (!known) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "op unknown"));
            return true;
        }
        if (state == "blocked") {
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
            json_object* d = json_object_new_object();
            json_object_object_add(d, "rollback",
                                   json_object_new_string(rb.c_str()));
            json_object_object_add(d, "toRevision",
                                   json_object_new_int(to_rev));
            json_object_object_add(
                d, "reason",
                json_object_new_string(block_reason.empty()
                                           ? "barrier-blocked"
                                           : block_reason.c_str()));
            dvs::send_json(res, dvs::envelope_ok(d, "rolled back"));
            return true;
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "state",
                               json_object_new_string(state.c_str()));
        json_object_object_add(d, "applyAt",
                               json_object_new_int64((int64_t)apply_ns));
        if (frame_index >= 0)
            json_object_object_add(d, "frameIndex",
                                   json_object_new_int64(frame_index));
        json_object* miss = json_object_new_array();
        for (const auto& m : still_missing)
            json_object_array_add(miss, json_object_new_string(m.c_str()));
        json_object_object_add(d, "missing", miss);
        dvs::send_json(res, dvs::envelope_ok(d, state));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/ops/config-ack") {
        if (!platform_mutation_allowed(ctx, req)) {
            dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                                   "missing or bad token"));
            return true;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string verr =
            b ? ct::validate_config_ack(b) : "INVALID_ARGUMENT:empty body";
        if (!verr.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        ctx.store->audit("{\"config-ack\":\"" +
                         dvs::json_escape(dvs::json_get_str(b, "node")) + "\"}");
        if (b) json_object_put(b);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "received", json_object_new_boolean(1));
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/ops/upgrade-progress") {
        if (!platform_mutation_allowed(ctx, req)) {
            dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                                   "missing or bad token"));
            return true;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string verr = b ? ct::validate_upgrade_progress(b)
                             : "INVALID_ARGUMENT:empty body";
        if (!verr.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        std::string tid = dvs::json_get_str(b, "taskId");
        std::string node = dvs::json_get_str(b, "node");
        std::string st = dvs::json_get_str(b, "status");
        {
            std::lock_guard<std::mutex> lk(ctx.channels.mu);
            ctx.channels.upgrade_progress[tid + "/" + node] = st;
        }
        ctx.store->audit("{\"upgrade-progress\":\"" + dvs::json_escape(node) +
                         "\",\"task\":\"" + dvs::json_escape(tid) +
                         "\",\"status\":\"" + dvs::json_escape(st) + "\"}");
        if (b) json_object_put(b);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "received", json_object_new_boolean(1));
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        return true;
    }
    if (req.method == "GET" && req.path.rfind("/api/v1/ops/", 0) == 0) {
        std::string opid = req.path.substr(strlen("/api/v1/ops/"));
        if (!opid.empty() && opid.find('/') == std::string::npos) {
            bool wait =
                req.query.count("wait") && req.query.at("wait") == "1";
            long wait_ms = 10000;
            if (wait && req.query.count("timeoutMs")) {
                try {
                    wait_ms = std::stol(req.query.at("timeoutMs"));
                } catch (...) {
                    wait_ms = 10000;
                }
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
                        std::lock_guard<std::mutex> lk(ctx.ops_mu);
                        op = ctx.ops.find(opid);
                        if (op) st = op->state;
                    }
                    if (!op || st != "preparing") break;
                    if (std::chrono::steady_clock::now() >= deadline) break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                }
            }
            {
                std::lock_guard<std::mutex> lk(ctx.ops_mu);
                op = ctx.ops.find(opid);
                if (op) miss = ctx.ops.missing(opid);
            }
            if (!op) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "op unknown"));
                return true;
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "operationId",
                                   json_object_new_string(op->op_id.c_str()));
            json_object_object_add(d, "state",
                                   json_object_new_string(op->state.c_str()));
            json_object_object_add(d, "revision",
                                   json_object_new_int(op->revision));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64(
                                       (int64_t)op->apply_at_ns));
            json_object_object_add(d, "wallId",
                                   json_object_new_string(op->wall_id.c_str()));
            json_object_object_add(d, "sceneId",
                                   json_object_new_string(
                                       op->scene_id.c_str()));
            json_object_object_add(d, "layoutHash",
                                   json_object_new_string(
                                       op->layout_hash.c_str()));
            json_object_object_add(d, "volume",
                                   json_object_new_int(op->volume));
            json_object_object_add(d, "epochTaiNs",
                                   json_object_new_int64(
                                       (int64_t)op->epoch_ns));
            json_object_object_add(d, "fps",
                                   json_object_new_double(op->fps));
            json_object_object_add(d, "frameIndex",
                                   json_object_new_int64(op->frame_index));
            json_object* ma = json_object_new_array();
            for (auto& m : miss)
                json_object_array_add(ma, json_object_new_string(m.c_str()));
            json_object_object_add(d, "missing", ma);
            dvs::send_json(res, dvs::envelope_ok(d, op->state));
            return true;
        }
    }
    return false;
}
