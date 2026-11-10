#include "agent_downlink.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include "agent_audit.h"
#include "agent_context.h"
#include "agent_media.h"
#include "dvs_contract.h"
#include "maintenance.h"
#include "media_ipc.h"
#include "node_store.h"
#include "registration.h"
#include "dvs_http_json.h"

void agent_install_downlink(AgentContext& ctx) {
  if (!ctx.reg) return;
  AgentContext* ctxp = &ctx;
  ctx.reg->set_downlink_handler(
      [ctxp](const std::string& type, json_object* data,
             const std::string& msg_id) {
        (void)msg_id;
        AgentContext& ctx = *ctxp;
        namespace ct = dvs::contract;
        if (type == ct::kLayoutPrepare) {
          if (!ct::validate_layout_prepare(data).empty()) return;
          std::string resp, opid, terr;
          int rev = 1;
          int64_t applyAtNs = 0;
          if (!agent_stage_layout(ctx, data, resp, opid, rev, applyAtNs,
                                  terr)) {
            fprintf(stderr, "dvs_agent: layout rejected (%s)\n", terr.c_str());
            return;
          }
          (void)resp;
          std::string nodeId = dvs::json_get_str(ctx.store->node(), "id");
          if (nodeId.empty()) nodeId = "local-node";
          agent_spawn_reporter(ctx, opid, rev, applyAtNs, nodeId);
        } else if (type == ct::kConfigUpdate) {
          if (!ct::validate_config_update(data).empty()) return;
          json_object* cfg = nullptr;
          json_object_object_get_ex(data, "config", &cfg);
          json_object* patch2 = json_object_new_object();
          json_object* role_val = nullptr;
          if (cfg && json_object_is_type(cfg, json_type_object)) {
            json_object_object_foreach(cfg, key, val) {
              std::string k(key);
              if (k == "geometry")
                json_object_object_add(patch2, "geometry",
                                       json_object_get(val));
              else if (k == "role")
                role_val = val;
            }
          }
          json_object_object_add(patch2, "configSource",
                                 json_object_new_string("platform-ws"));
          ctx.store->update_node(patch2);
          json_object_put(patch2);
          if (role_val) {
            std::string want;
            if (json_object_is_type(role_val, json_type_string))
              want = json_object_get_string(role_val);
            else {
              json_object* rv = nullptr;
              if (json_object_object_get_ex(role_val, "role", &rv) && rv)
                want = json_object_get_string(rv);
            }
            if (want != "input" && want != "output") {
              ctx.store->mark_failed("role", "INVALID_ARGUMENT", "角色取值非法");
              agent_audit("role", "platform-ws", "failed",
                          std::string("INVALID_ARGUMENT: illegal role '") +
                              want + "'");
            } else {
              if (json_object_is_type(role_val, json_type_string)) {
                json_object* rv2 = json_object_new_object();
                json_object_object_add(rv2, "role",
                                       json_object_new_string(want.c_str()));
                ctx.store->set_desired("role", rv2, nullptr);
                json_object_put(rv2);
              } else {
                ctx.store->set_desired("role", json_object_get(role_val),
                                       nullptr);
              }
              std::string pend = agent_sync_pending_role(ctx, want);
              agent_audit("role", "platform-ws",
                          pend.empty() ? "applied" : "pending",
                          pend.empty() ? "already active"
                                       : ("pending " + pend));
            }
          }
          std::string nodeId = dvs::json_get_str(ctx.store->node(), "id");
          if (nodeId.empty()) nodeId = "local-node";
          int applied = 0;
          json_object* nd = ctx.store->node();
          json_object* crr = nullptr;
          if (nd && json_object_object_get_ex(nd, "configRevision", &crr) &&
              crr) {
            for (int i = 0; i < 6; i++) {
              json_object* s = nullptr;
              if (json_object_object_get_ex(crr, NodeStore::SCOPES[i], &s) &&
                  s) {
                json_object* a = nullptr;
                if (json_object_object_get_ex(s, "applied", &a) && a)
                  applied = std::max(applied, json_object_get_int(a));
              }
            }
          }
          if (ctx.reg) ctx.reg->uplink_config_ack(nodeId, applied);
        } else if (type == ct::kUpgradeTask) {
          if (!ct::validate_upgrade_task(data).empty()) return;
          std::string tid = dvs::json_get_str(data, "taskId");
          std::string fn = dvs::json_get_str(data, "filename");
          std::string sha = dvs::json_get_str(data, "sha256");
          std::string ver = dvs::json_get_str(data, "version");
          std::string nodeId = dvs::json_get_str(ctx.store->node(), "id");
          if (nodeId.empty()) nodeId = "local-node";
          std::thread([ctxp, tid, fn, sha, ver, nodeId]() {
            AgentContext& ctx = *ctxp;
            if (ctx.reg)
              ctx.reg->uplink_upgrade_progress(tid, nodeId, "running");
            // Platform tasks are explicit operator intent: staged package is
            // switched immediately (confirm=true); version travels in task.
            json_object* r =
                ctx.maint ? ctx.maint->upgrade(fn, sha, ver, true) : nullptr;
            std::string stt = "failed";
            if (r) {
              json_object* sv = nullptr;
              if (json_object_object_get_ex(r, "status", &sv) && sv)
                stt = json_object_get_string(sv);
              json_object_put(r);
            }
            if (ctx.reg)
              ctx.reg->uplink_upgrade_progress(tid, nodeId, stt);
          }).detach();
        }
      });
}
