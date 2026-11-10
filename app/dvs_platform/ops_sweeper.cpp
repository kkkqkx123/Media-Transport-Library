#include "ops_sweeper.h"

#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>

#include "dvs_http_json.h"
#include "layout.h"
#include "layout_fanout.h"
#include "platform_context.h"
#include "platform_store.h"

void restore_ops(PlatformContext& ctx) {
    for (const auto& id : ctx.store->list_ids("op")) {
        json_object* d = ctx.store->get_doc("op", id);
        if (!d) continue;
        Operation op;
        if (operation_from_json(d, &op)) {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            ctx.ops.restore(op);
        } else {
            ctx.store->del_doc("op", id);
        }
        json_object_put(d);
    }
}

void start_barrier_sweeper(PlatformContext& ctx) {
    std::thread([&ctx]() {
        time_t last_gc = 0;
        const uint64_t kOpTtlNs = 24ULL * 3600 * 1000000000ULL;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::vector<std::string> expired;
            {
                std::lock_guard<std::mutex> lk(ctx.ops_mu);
                expired = ctx.ops.expire_sweep(dvs::wall_ns_now());
            }
            for (auto& id : expired) {
                int to_rev = 0;
                std::string rb;
                std::set<std::string> rb_members;
                {
                    std::lock_guard<std::mutex> lk(ctx.ops_mu);
                    rb = ctx.ops.rollback_op(id, &to_rev);
                    const Operation* op2 = ctx.ops.find(id);
                    if (op2) rb_members = op2->members;
                }
                ctx.store->audit("{\"barrier-timeout\":\"" + dvs::json_escape(id) +
                                 "\"}");
                if (!rb.empty())
                    execute_rollback(ctx, rb, to_rev, rb_members);
            }
            std::vector<std::string> replay;
            {
                std::lock_guard<std::mutex> lk(ctx.ops_mu);
                replay = ctx.ops.pending_rollbacks();
            }
            for (auto& id : replay) {
                int to_rev = 0;
                std::string rb;
                std::set<std::string> rb_members;
                {
                    std::lock_guard<std::mutex> lk(ctx.ops_mu);
                    rb = ctx.ops.rollback_op(id, &to_rev);
                    const Operation* op2 = ctx.ops.find(id);
                    if (op2) rb_members = op2->members;
                }
                ctx.store->audit("{\"op-rollback-replay\":\"" +
                                 dvs::json_escape(id) + "\",\"rollback\":\"" +
                                 dvs::json_escape(rb) + "\"}");
                if (!rb.empty())
                    execute_rollback(ctx, rb, to_rev, rb_members);
            }
            time_t now_s = time(nullptr);
            if (last_gc == 0 || now_s - last_gc >= 3600) {
                last_gc = now_s;
                {
                    std::lock_guard<std::mutex> lk(ctx.ops_mu);
                    ctx.ops.gc(dvs::wall_ns_now(), kOpTtlNs);
                }
                ctx.store->audit_trim();
                for (auto& wid : ctx.store->list_ids("walls")) {
                    json_object* w = ctx.store->get_doc("walls", wid);
                    if (!w) continue;
                    bool gone = false;
                    if (wall_is_deleted(w)) {
                        json_object* v = nullptr;
                        if (json_object_object_get_ex(w, "deletedAtEpoch", &v) &&
                            v) {
                            int64_t at = json_object_get_int64(v);
                            if (at > 0 && now_s - (time_t)at > 7 * 24 * 3600)
                                gone = true;
                        }
                    }
                    json_object_put(w);
                    if (gone) {
                        ctx.store->del_doc("walls", wid);
                        ctx.store->del_doc("base-images", wid);
                        ctx.store->audit("{\"wall-gc\":\"" +
                                         dvs::json_escape(wid) + "\"}");
                    }
                }
            }
        }
    })
        .detach();
}
