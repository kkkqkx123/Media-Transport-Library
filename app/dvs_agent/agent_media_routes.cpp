#include "agent_media_routes.h"

#include <cstdio>

#include "agent_audit.h"
#include "agent_auth.h"
#include "agent_context.h"
#include "agent_media.h"
#include "dvs_contract.h"
#include "media_ipc.h"
#include "node_store.h"
#include "dvs_http_json.h"

bool agent_handle_media(AgentContext& ctx, const HttpRequest& req,
                        HttpResponse& res) {
  if (req.method == "GET" && req.path == "/api/v1/node/media/status") {
    std::string r = agent_media_query(ctx, "query_state", nullptr);
    json_object* o = json_tokener_parse(r.c_str());
    if (!o) o = json_object_new_object();
    json_object* okv = nullptr;
    json_object* data = nullptr;
    if (json_object_object_get_ex(o, "ok", &okv) && okv &&
        json_object_get_boolean(okv) &&
        json_object_object_get_ex(o, "data", &data) && data)
      agent_project_playback(ctx, data);
    dvs::send_json(res, o);
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/media/events") {
    json_object* merged = json_object_new_array();
    json_object* a = ctx.media ? ctx.media->drain_events() : nullptr;
    json_object* b = ctx.media_q ? ctx.media_q->drain_events() : nullptr;
    for (json_object* src : {a, b}) {
      if (!src) continue;
      int n = json_object_array_length(src);
      for (int i = 0; i < n; i++)
        json_object_array_add(
            merged, json_object_get(json_object_array_get_idx(src, i)));
      json_object_put(src);
    }
    dvs::send_json(res, dvs::envelope_ok(merged, "", "a"));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/media/prepare") {
    if (ctx.api_token.empty() || !dvs::http_bearer_ok(req, ctx.api_token)) {
      dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                              "missing or bad token", "a"));
      return true;
    }
    json_object* body = json_tokener_parse(req.body.c_str());
    std::string r = agent_media_cmd(ctx, "prepare", body);
    if (body) json_object_put(body);
    json_object* o = json_tokener_parse(r.c_str());
    if (!o) o = json_object_new_object();
    dvs::send_json(res, o);
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/media/commit") {
    if (ctx.api_token.empty() || !dvs::http_bearer_ok(req, ctx.api_token)) {
      dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                              "missing or bad token", "a"));
      return true;
    }
    json_object* body = json_tokener_parse(req.body.c_str());
    std::string r = agent_media_cmd(ctx, "commit", body);
    if (body) json_object_put(body);
    json_object* o = json_tokener_parse(r.c_str());
    if (!o) o = json_object_new_object();
    dvs::send_json(res, o);
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/media/abort") {
    if (ctx.api_token.empty() || !dvs::http_bearer_ok(req, ctx.api_token)) {
      dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                              "missing or bad token", "a"));
      return true;
    }
    json_object* body = json_tokener_parse(req.body.c_str());
    // Abort is the escape hatch: direct channel, never version-gated.
    std::string r = ctx.media ? ctx.media->request("abort", body)
                              : agent_gated_envelope(
                                    "MEDIA_ENGINE_UNCONFIGURED: no IPC client");
    if (body) json_object_put(body);
    json_object* o = json_tokener_parse(r.c_str());
    if (!o) o = json_object_new_object();
    dvs::send_json(res, o);
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/media/layout") {
    if (ctx.api_token.empty() || !dvs::http_bearer_ok(req, ctx.api_token)) {
      dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                              "missing or bad token", "a"));
      return true;
    }
    json_object* body = json_tokener_parse(req.body.c_str());
    if (!body || !dvs::contract::validate_layout_prepare(body).empty()) {
      std::string reason =
          body ? dvs::contract::validate_layout_prepare(body)
               : "INVALID_ARGUMENT:empty body";
      if (body) json_object_put(body);
      dvs::send_json(
          res, dvs::envelope_fail("INVALID_ARGUMENT", 400, reason.c_str(), "a"));
      return true;
    }
    json_object* wins0 = nullptr;
    if (body) json_object_object_get_ex(body, "windows", &wins0);
    if (wins0 && !json_object_is_type(wins0, json_type_array)) {
      if (body) json_object_put(body);
      dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                              "windows must be an array", "a"));
      return true;
    }
    std::string resp, opid, err;
    int rev = 1;
    int64_t applyAtNs = 0;
    if (!agent_stage_layout(ctx, body, resp, opid, rev, applyAtNs, err)) {
      if (body) json_object_put(body);
      dvs::send_json(res,
                      dvs::envelope_fail("INVALID_ARGUMENT", 400, err.c_str(), "a"));
      return true;
    }
    std::string nodeId = dvs::json_get_str(ctx.store->node(), "id");
    if (nodeId.empty()) nodeId = "local-node";
    agent_spawn_reporter(ctx, opid, rev, applyAtNs, nodeId);
    json_object* o = json_tokener_parse(resp.c_str());
    if (!o) o = json_object_new_object();
    dvs::send_json(res, o);
    if (body) json_object_put(body);
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/media/lanes") {
    std::string r = agent_media_query(ctx, "query_state", nullptr);
    json_object* o = json_tokener_parse(r.c_str());
    json_object* d = nullptr;
    json_object* lanes = nullptr;
    if (o && json_object_object_get_ex(o, "data", &d) && d)
      json_object_object_get_ex(d, "lanes", &lanes);
    dvs::send_json(res, dvs::envelope_ok(
                             lanes ? json_object_get(lanes)
                                   : json_object_new_array(),
                             "", "a"));
    if (o) json_object_put(o);
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/media/tx") {
    std::string r = agent_media_query(ctx, "query_state", nullptr);
    json_object* o = json_tokener_parse(r.c_str());
    json_object* okv = nullptr;
    bool ok = o && json_object_object_get_ex(o, "ok", &okv) && okv &&
              json_object_get_boolean(okv);
    json_object* d = nullptr;
    json_object* tx = nullptr;
    if (ok && json_object_object_get_ex(o, "data", &d) && d)
      json_object_object_get_ex(d, "tx", &tx);
    if (!ok) {
      dvs::send_json(res, o ? o : json_object_new_object());
      return true;
    }
    dvs::send_json(res, dvs::envelope_ok(
                             tx ? json_object_get(tx) : json_object_new_object(),
                             "", "a"));
    if (o) json_object_put(o);
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/media/sources") {
    std::string r = agent_media_query(ctx, "query_state", nullptr);
    json_object* o = json_tokener_parse(r.c_str());
    json_object* d = json_object_new_object();
    json_object* lanes = json_object_new_array();
    bool engine_ok = false;
    std::string eng_code, eng_msg;
    if (o) {
      json_object* okv = nullptr;
      json_object* data = nullptr;
      if (json_object_object_get_ex(o, "ok", &okv) && okv &&
          json_object_get_boolean(okv) &&
          json_object_object_get_ex(o, "data", &data) && data) {
        engine_ok = true;
        json_object* lj = nullptr;
        if (json_object_object_get_ex(data, "lanes", &lj) && lj)
          lanes = json_object_get(lj);
        agent_project_playback(ctx, data);
      } else {
        json_object* cv = nullptr;
        json_object* mv = nullptr;
        if (json_object_object_get_ex(o, "code", &cv) && cv)
          eng_code = json_object_get_string(cv);
        if (json_object_object_get_ex(o, "msg", &mv) && mv)
          eng_msg = json_object_get_string(mv);
      }
      json_object_put(o);
    }
    json_object_object_add(d, "lanes", lanes);
    json_object* inp = nullptr;
    json_object_object_get_ex(ctx.store->node(), "input", &inp);
    json_object_object_add(d, "input",
                           inp ? json_object_get(inp) : json_object_new_object());
    json_object* caps = nullptr;
    json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
    json_object* iparr = nullptr;
    if (caps && json_object_object_get_ex(caps, "inputPorts", &iparr) && iparr)
      json_object_object_add(d, "inputPorts", json_object_get(iparr));
    else
      json_object_object_add(d, "inputPorts", json_object_new_array());
    if (!engine_ok) {
      json_object* eo = json_object_new_object();
      json_object_object_add(eo, "engineUnreachable",
                             json_object_new_boolean(1));
      json_object_object_add(d, "engine", eo);
      std::string code =
          eng_code.empty() ? "MEDIA_ENGINE_UNREACHABLE" : eng_code;
      std::string msg =
          eng_msg.empty() ? "media engine unreachable" : eng_msg;
      json_object* fail = dvs::envelope_fail(code.c_str(), 503, msg.c_str(), "a");
      json_object_object_del(fail, "data");
      json_object_object_add(fail, "data", d);
      dvs::send_json(res, fail);
      return true;
    }
    dvs::send_json(res, dvs::envelope_ok(d, "", "a"));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/media/logs") {
    json_object* d = json_object_new_object();
    json_object_object_add(d, "items", agent_audit_snapshot(50));
    json_object* link = json_object_new_object();
    json_object_object_add(
        link, "state",
        json_object_new_string(agent_engine_link_state(ctx).c_str()));
    json_object_object_add(link, "failStreak",
                           json_object_new_int(ctx.media_q
                                                   ? ctx.media_q->fail_streak()
                                                   : 0));
    json_object_object_add(d, "engineLink", link);
    long long age = agent_cap_age_ms();
    json_object_object_add(d, "capAgeMs", json_object_new_int64(age));
    json_object_object_add(d, "capStale",
                           json_object_new_boolean(agent_cap_stale() ? 1 : 0));
    json_object* nsys = nullptr;
    json_object_object_get_ex(ctx.store->node(), "networkSystem", &nsys);
    json_object_object_add(d, "networkSystem",
                           nsys ? json_object_get(nsys)
                                : json_object_new_object());
    json_object* lup = nullptr;
    json_object_object_get_ex(ctx.store->node(), "lastUpgrade", &lup);
    if (lup) json_object_object_add(d, "lastUpgrade", json_object_get(lup));
    dvs::send_json(res, dvs::envelope_ok(d, "", "a"));
    return true;
  }
  return false;
}
