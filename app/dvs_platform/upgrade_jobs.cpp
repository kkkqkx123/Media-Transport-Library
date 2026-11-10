#include "upgrade_jobs.h"

#include <cstring>
#include <mutex>
#include <thread>

#include "channels.h"
#include "dvs_contract.h"
#include "http_client.h"
#include "dvs_http_json.h"
#include "platform_context.h"
#include "platform_store.h"

namespace ct = dvs::contract;

bool route_upgrade(PlatformContext& ctx, const HttpRequest& req,
                   HttpResponse& res) {
    if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
        if (!platform_mutation_allowed(ctx, req)) {
            dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                                   "missing or bad token"));
            return true;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        json_object* arr = nullptr;
        if (b) json_object_object_get_ex(b, "targetNodeIds", &arr);
        std::string fn = dvs::json_get_str(b, "filename");
        std::string sha = dvs::json_get_str(b, "sha256");
        std::string ver = dvs::json_get_str(b, "fwVersion");
        std::vector<std::string> nodes;
        if (arr && json_object_is_type(arr, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(arr); i++) {
                json_object* e = json_object_array_get_idx(arr, i);
                if (e && json_object_is_type(e, json_type_string)) {
                    std::string nid = json_object_get_string(e);
                    if (!nid.empty()) nodes.push_back(nid);
                }
            }
        }
        if (nodes.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "need non-empty targetNodeIds"));
            return true;
        }
        if (fn.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "need filename and sha256 from an uploaded package"));
            return true;
        }
        if (!ver.empty())
            ctx.store->audit("{\"upgrade-version\":\"" +
                             dvs::json_escape(ver) + "\"}");
        if (b) json_object_put(b);
        std::string task_id = ct::next_msg_id("p");
        {
            std::lock_guard<std::mutex> lk(ctx.up_mu);
            ctx.up_q.push(UpgradeJob{task_id, nodes, fn, sha});
        }
        ctx.up_cv.notify_one();
        ctx.store->audit("{\"upgrade-enqueue\":\"" + dvs::json_escape(task_id) +
                         "\"}");
        json_object* d = json_object_new_object();
        json_object_object_add(d, "taskId",
                               json_object_new_string(task_id.c_str()));
        json_object_object_add(d, "queued",
                               json_object_new_int((int)nodes.size()));
        dvs::send_json(res, dvs::envelope_ok(d, "queued"));
        return true;
    }
    if (req.method == "GET" &&
        req.path.rfind("/api/v1/nodes/upgrade/", 0) == 0) {
        std::string tid = req.path.substr(strlen("/api/v1/nodes/upgrade/"));
        if (tid.empty() || tid.find('/') != std::string::npos) return false;
        json_object* d = json_object_new_object();
        json_object* items = json_object_new_array();
        int done = 0, total = 0;
        {
            std::lock_guard<std::mutex> lk(ctx.channels.mu);
            for (auto& kv : ctx.channels.upgrade_progress) {
                const std::string& k = kv.first;
                if (k.rfind(tid + "/", 0) != 0) continue;
                std::string node = k.substr(tid.size() + 1);
                json_object* e = json_object_new_object();
                json_object_object_add(e, "node",
                                       json_object_new_string(node.c_str()));
                json_object_object_add(e, "status",
                                       json_object_new_string(
                                           kv.second.c_str()));
                json_object_array_add(items, e);
                total++;
                if (kv.second == "done" || kv.second == "succeeded") done++;
            }
        }
        json_object_object_add(d, "taskId",
                               json_object_new_string(tid.c_str()));
        json_object_object_add(d, "items", items);
        json_object_object_add(d, "done", json_object_new_int(done));
        json_object_object_add(d, "total", json_object_new_int(total));
        dvs::send_json(res, dvs::envelope_ok(d, "upgrade-progress"));
        return true;
    }
    return false;
}

void start_upgrade_worker(PlatformContext& ctx) {
    std::thread([&ctx]() {
        HttpClient cli;
        for (;;) {
            UpgradeJob job;
            {
                std::unique_lock<std::mutex> lk(ctx.up_mu);
                ctx.up_cv.wait(lk, [&ctx]() { return !ctx.up_q.empty(); });
                job = ctx.up_q.front();
                ctx.up_q.pop();
            }
            for (auto& nid : job.nodes) {
                json_object* task = json_object_new_object();
                json_object_object_add(task, "taskId",
                                       json_object_new_string(
                                           job.task_id.c_str()));
                json_object_object_add(task, "filename",
                                       json_object_new_string(
                                           job.filename.c_str()));
                if (!job.sha256.empty())
                    json_object_object_add(task, "sha256",
                                           json_object_new_string(
                                               job.sha256.c_str()));
                bool via_ws = false;
                if (ct::validate_upgrade_task(task).empty())
                    via_ws = ws_push(ctx.channels, nid, ct::kUpgradeTask,
                                     json_object_get(task));
                if (via_ws) {
                    ctx.store->audit("{\"upgrade-push\":\"" +
                                     dvs::json_escape(nid) +
                                     "\",\"via\":\"ws\"}");
                    json_object_put(task);
                    continue;
                }
                json_object* nd = ctx.store->get_doc("node", nid);
                std::string base = nd ? dvs::json_get_str(nd, "agentBase") : "";
                if (nd) json_object_put(nd);
                if (base.empty()) {
                    ctx.store->audit("{\"upgrade-skip\":\"" + dvs::json_escape(nid) +
                                     "\"}");
                    json_object_put(task);
                    continue;
                }
                const char* tb = json_object_to_json_string(task);
                std::string body = tb ? tb : "{}";
                json_object_put(task);
                std::string resp;
                int sc = cli.post_json(dvs::url_join(base, "/api/v1/node/upgrade"), body,
                                       resp, 10000, 2, ctx.api_token);
                ctx.store->audit("{\"upgrade-push\":\"" + dvs::json_escape(nid) +
                                 "\",\"status\":" + std::to_string(sc) + "}");
            }
        }
    })
        .detach();
}
