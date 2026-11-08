#include "agent_media.h"

#include <cstdio>
#include <mutex>
#include <thread>

#include "agent_audit.h"
#include "agent_context.h"
#include "dvs_node_bundle.h"
#include "media_ipc.h"
#include "node_store.h"
#include "registration.h"
#include "dvs_http_json.h"

namespace {
std::mutex g_ver_mu;
bool g_ver_known = false;
bool g_ver_match = false;
long long g_ver_at = 0;
const long long kVerCacheMs = 5000;

long long g_cap_ok_ms = 0;
const long long kCapStaleMs = 30000;
std::string g_cap_ports_sig;
std::string g_link_logged = "unknown";

bool configure_has_layout_keys(json_object* payload) {
  if (!payload || !json_object_is_type(payload, json_type_object))
    return false;
  json_object* t = nullptr;
  return (json_object_object_get_ex(payload, "streams", &t) && t) ||
         (json_object_object_get_ex(payload, "windows", &t) && t);
}

void revalidate_output_locked(AgentContext& ctx, json_object* caps) {
  json_object* out = nullptr;
  json_object_object_get_ex(ctx.store->node(), "output", &out);
  if (!out || json_object_is_type(out, json_type_null)) return;
  json_object* wv = nullptr;
  json_object* hv = nullptr;
  json_object* zv = nullptr;
  int w = 0, h = 0, z = 0;
  if (json_object_object_get_ex(out, "width", &wv) && wv)
    w = json_object_get_int(wv);
  if (json_object_object_get_ex(out, "height", &hv) && hv)
    h = json_object_get_int(hv);
  if (json_object_object_get_ex(out, "refreshHz", &zv) && zv)
    z = json_object_get_int(zv);
  if (w <= 0 || h <= 0 || z <= 0) return;
  std::string conn;
  json_object* cc = nullptr;
  if (json_object_object_get_ex(out, "connector", &cc) && cc)
    conn = json_object_get_string(cc);
  int lim_w = 7680, lim_h = 4320, lim_z = 60;
  bool known = false;
  json_object* arr = nullptr;
  if (caps && json_object_object_get_ex(caps, "outputPorts", &arr) && arr &&
      json_object_is_type(arr, json_type_array)) {
    for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
      json_object* e = json_object_array_get_idx(arr, (int)i);
      json_object* idv = nullptr;
      if (!e || !json_object_object_get_ex(e, "id", &idv) || !idv) continue;
      if (!conn.empty() && conn != json_object_get_string(idv)) continue;
      known = true;
      json_object* q = nullptr;
      if (json_object_object_get_ex(e, "maxWidth", &q) && q)
        lim_w = json_object_get_int(q);
      if (json_object_object_get_ex(e, "maxHeight", &q) && q)
        lim_h = json_object_get_int(q);
      if (json_object_object_get_ex(e, "maxRefreshHz", &q) && q)
        lim_z = json_object_get_int(q);
      break;
    }
    if (!conn.empty() && !known) {
      ctx.store->mark_failed("output", "HOTPLUG_CHANGED",
                             "output connector gone after hotplug");
      json_object* ep = json_object_new_object();
      json_object_object_add(ep, "code",
                             json_object_new_string("HOTPLUG_CHANGED"));
      json_object_object_add(ep, "reason",
                             json_object_new_string(
                                 "output connector gone after hotplug"));
      json_object* xp = json_object_new_object();
      json_object_object_add(xp, "exception", ep);
      ctx.store->update_node(xp);
      json_object_put(xp);
      agent_audit("output", "hotplug", "failed", "connector gone");
      return;
    }
    if (w > lim_w || h > lim_h || z > lim_z) {
      ctx.store->mark_failed("output", "HOTPLUG_CHANGED",
                             "saved mode exceeds new output caps");
      agent_audit("output", "hotplug", "failed", "saved mode out of range");
    }
  }
}

void report_commit(AgentContext* ctxp, const std::string& opid, int rev,
                   int64_t applyAtNs, const std::string& nodeId) {
  (void)applyAtNs;  // reserved for barrier diagnostics
  AgentContext& ctx = *ctxp;
  std::string status = "blocked";
  std::string lanes_json = "[]";
  bool done = false;
  if (ctx.media) {
    std::string ev_status, ev_lanes;
    if (ctx.media->wait_prepare_result(opid, 5000, ev_status, ev_lanes)) {
      status = ev_status;
      lanes_json = ev_lanes;
      done = true;
    }
  }
  if (!done) {
    std::string qs = agent_media_query(ctx, "query_state", nullptr);
    json_object* st = json_tokener_parse(qs.c_str());
    if (st) {
      json_object* d = nullptr;
      if (json_object_object_get_ex(st, "data", &d) && d) {
        std::string s = dvs::json_get_str(d, "state");
        status = (s == "prepared" || s == "idle") ? "ready" : "blocked";
        json_object* lj = nullptr;
        if (json_object_object_get_ex(d, "lanes", &lj) && lj) {
          const char* ls = json_object_to_json_string(lj);
          if (ls) lanes_json = ls;
        }
      } else {
        status = "blocked";
      }
      json_object_put(st);
    } else {
      status = "blocked";
    }
  }
  json_object* lanes = nullptr;
  if (lanes_json != "[]") lanes = json_tokener_parse(lanes_json.c_str());
  Registration::BarrierFrame fr;
  {
    json_object* term = json_object_new_object();
    json_object_object_add(term, "operationId",
                           json_object_new_string(opid.c_str()));
    json_object_object_add(term, "revision", json_object_new_int(rev));
    std::string tr = (status == "ready")
                         ? agent_media_cmd(ctx, "commit", term)
                         : ctx.media->request("abort", term);
    json_object_put(term);
    json_object* to = json_tokener_parse(tr.c_str());
    bool ok = false;
    if (to) {
      json_object* okv = nullptr;
      if (json_object_object_get_ex(to, "ok", &okv) && okv)
        ok = json_object_get_boolean(okv) != 0;
      json_object* dd = nullptr;
      if (json_object_object_get_ex(to, "data", &dd) && dd) {
        json_object* fv = nullptr;
        if (json_object_object_get_ex(dd, "frameIndex", &fv) && fv &&
            json_object_is_type(fv, json_type_int)) {
          fr.has_frame = true;
          fr.frame_index = json_object_get_int64(fv);
        }
        json_object* atv = nullptr;
        if (json_object_object_get_ex(dd, "appliedAtTaiNs", &atv) && atv &&
            json_object_is_type(atv, json_type_int)) {
          fr.has_applied_at = true;
          fr.applied_at_ns = json_object_get_int64(atv);
        }
        json_object* lv = nullptr;
        if (json_object_object_get_ex(dd, "late", &lv) && lv)
          fr.late = json_object_get_boolean(lv) != 0;
      }
      if (!ok) {
        json_object* msgv = nullptr;
        if (json_object_object_get_ex(to, "msg", &msgv) && msgv &&
            json_object_is_type(msgv, json_type_string))
          fr.reason = json_object_get_string(msgv);
      }
      json_object_put(to);
    }
    if (!ok && status == "ready") {
      fprintf(stderr,
              "dvs-agent: media commit (%s) refused, downgrade to blocked\n",
              opid.c_str());
      status = "blocked";
    }
  }
  if (ctx.platform_url.empty()) {
    if (lanes) json_object_put(lanes);
    return;
  }
  if (ctx.reg)
    ctx.reg->uplink_barrier_ack(opid, nodeId, status, lanes, fr);
  if (lanes) json_object_put(lanes);
}
}  // namespace

void agent_media_note_hello(AgentContext& ctx, const std::string& hello) {
  bool answered = false, match = false;
  std::string eb;
  int ei = -1;
  MediaIpcClient::parse_hello(hello, answered, match, eb, ei);
  std::lock_guard<std::mutex> lk(g_ver_mu);
  g_ver_at = dvs::steady_ms_now();
  if (answered) {
    g_ver_known = true;
    g_ver_match = match;
  }
  (void)ctx;
}

bool agent_media_layout_allowed(AgentContext& ctx, std::string& reason_out) {
  reason_out.clear();
  if (!ctx.media_q) return true;
  {
    std::lock_guard<std::mutex> lk(g_ver_mu);
    if (g_ver_known && dvs::steady_ms_now() - g_ver_at < kVerCacheMs) {
      if (!g_ver_match)
        reason_out = "VERSION_MISMATCH: media engine bundle differs";
      return g_ver_match;
    }
  }
  bool answered = false, match = false;
  std::string eb;
  int ei = -1;
  MediaIpcClient::parse_hello(ctx.media_q->hello(), answered, match, eb, ei);
  std::lock_guard<std::mutex> lk(g_ver_mu);
  bool prev_known = g_ver_known;
  bool prev_match = g_ver_match;
  g_ver_at = dvs::steady_ms_now();
  if (!answered) return true;
  g_ver_known = true;
  g_ver_match = match;
  if (!match) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "VERSION_MISMATCH: engine bundle %s ipc %d, want %s/%d",
             eb.empty() ? "?" : eb.c_str(), ei, DVS_NODE_BUNDLE_VERSION,
             DVS_MEDIA_IPC_VERSION);
    reason_out = buf;
    fprintf(stderr, "dvs-agent: %s\n", buf);
    if (!prev_known || prev_match)
      agent_audit("version", "gate", "failed", buf);
  }
  return match;
}

std::string agent_gated_envelope(const std::string& reason) {
  std::string code = "OK";
  std::string msg = reason;
  size_t colon = reason.find(':');
  if (colon != std::string::npos) {
    code = reason.substr(0, colon);
    msg = reason.substr(colon + 1);
    while (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
  }
  json_object* o = json_object_new_object();
  json_object_object_add(o, "ok", json_object_new_boolean(0));
  json_object_object_add(o, "status", json_object_new_int(409));
  json_object_object_add(o, "code", json_object_new_string(code.c_str()));
  json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
  json_object_object_add(o, "data", json_object_new_object());
  json_object_object_add(o, "requestId", json_object_new_string("gate"));
  const char* s = json_object_to_json_string(o);
  std::string out(s ? s : "{}");
  json_object_put(o);
  return out;
}

std::string agent_media_cmd(AgentContext& ctx, const std::string& type,
                            json_object* payload, int timeout_ms) {
  if (!ctx.media)
    return agent_gated_envelope("MEDIA_ENGINE_UNCONFIGURED: no IPC client");
  if (type == "prepare" || type == "commit" || type == "configure" ||
      type == "clear" || type == "tx_start" || type == "tx_stop" ||
      type == "set_sources") {
    std::string reason;
    if (!agent_media_layout_allowed(ctx, reason))
      return agent_gated_envelope(reason);
    if (type == "configure" && configure_has_layout_keys(payload))
      return agent_gated_envelope(
          "INVALID_ARGUMENT: picture changes must use prepare/commit");
  }
  return ctx.media->request(type, payload, timeout_ms);
}

std::string agent_media_query(AgentContext& ctx, const std::string& type,
                              json_object* payload, int timeout_ms) {
  if (!ctx.media_q)
    return agent_gated_envelope("MEDIA_ENGINE_UNCONFIGURED: no IPC client");
  return ctx.media_q->request(type, payload, timeout_ms);
}

int agent_config_desired_revision(AgentContext& ctx, const std::string& scope,
                                  const std::string& node_id) {
  int want_rev = 0;
  json_object* cur = ctx.store->get_config(scope, node_id);
  json_object* drv = nullptr;
  if (cur && json_object_object_get_ex(cur, "desiredRevision", &drv) && drv)
    want_rev = json_object_get_int(drv);
  if (cur) json_object_put(cur);
  return want_rev;
}

void agent_apply_media_config_outcome(AgentContext& ctx,
                                      const std::string& scope,
                                      const std::string& node_id, int want_rev,
                                      bool ok, const std::string& code,
                                      const std::string& msg) {
  int cur_rev = agent_config_desired_revision(ctx, scope, node_id);
  if (cur_rev != want_rev) {
    fprintf(stderr, "dvs-agent: media configure (%s) stale rev %d vs %d, skip\n",
            scope.c_str(), want_rev, cur_rev);
    return;
  }
  if (!ok) {
    ctx.store->mark_failed(scope,
                           code.empty() ? "MEDIA_CONFIGURE_FAILED" : code.c_str(),
                           msg.empty() ? "media engine refused config"
                                       : msg.c_str());
    fprintf(stderr, "dvs-agent: media configure (%s) failed code=%s msg=%s\n",
            scope.c_str(), code.c_str(), msg.c_str());
    agent_audit(scope, "media-configure", "failed",
                (code.empty() ? "MEDIA_CONFIGURE_FAILED" : code) + ": " +
                    (msg.empty() ? "media engine refused config" : msg));
  } else {
    ctx.store->mark_applied(scope, 0);
    fprintf(stderr, "dvs-agent: media configure (%s) ok\n", scope.c_str());
    agent_audit(scope, "media-configure", "applied", "engine accepted");
  }
}

std::string agent_engine_link_state(AgentContext& ctx) {
  if (!ctx.media_q) return "unknown";
  int streak = ctx.media_q->fail_streak();
  long long ok_age = -1;
  if (ctx.media_q->last_ok_ms() > 0)
    ok_age = dvs::steady_ms_now() - ctx.media_q->last_ok_ms();
  std::string st;
  if (streak >= 3)
    st = "offline";
  else if (streak > 0)
    st = "degraded";
  else if (ok_age >= 0)
    st = "online";
  else
    st = "unknown";
  if (st != g_link_logged) {
    char detail[96];
    snprintf(detail, sizeof(detail), "%s -> %s (streak=%d)",
             g_link_logged.c_str(), st.c_str(), streak);
    fprintf(stderr, "dvs-agent: engine link %s\n", detail);
    agent_audit("engine", "link", st, detail);
    g_link_logged = st;
  }
  return st;
}

long long agent_cap_age_ms() {
  return (g_cap_ok_ms > 0) ? dvs::steady_ms_now() - g_cap_ok_ms : -1;
}

bool agent_cap_stale() {
  long long age = agent_cap_age_ms();
  return (age < 0 || age > kCapStaleMs);
}

std::string agent_caps_ports_signature(json_object* caps) {
  if (!caps) return "";
  json_object* ip = nullptr;
  json_object* op = nullptr;
  json_object_object_get_ex(caps, "inputPorts", &ip);
  json_object_object_get_ex(caps, "outputPorts", &op);
  const char* a = ip ? json_object_to_json_string(ip) : "[]";
  const char* b = op ? json_object_to_json_string(op) : "[]";
  return std::string(a ? a : "[]") + "|" + std::string(b ? b : "[]");
}

void agent_refresh_capability(AgentContext& ctx) {
  if (!ctx.media_q || !ctx.store) return;
  std::string cr = ctx.media_q->request("capability", nullptr);
  json_object* co = json_tokener_parse(cr.c_str());
  if (!co) {
    fprintf(stderr,
            "dvs-agent: startup capability pull failed (unparsable)\n");
    return;
  }
  json_object* okv = nullptr;
  json_object* data = nullptr;
  if (json_object_object_get_ex(co, "ok", &okv) && okv &&
      json_object_get_boolean(okv) &&
      json_object_object_get_ex(co, "data", &data) && data) {
    json_object* patch = json_object_new_object();
    json_object* caps = nullptr;
    json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
    json_object* merged =
        caps ? json_object_get(caps) : json_object_new_object();
    json_object_object_foreach(data, k, v) {
      json_object_object_add(merged, k, json_object_get(v));
    }
    json_object_object_add(patch, "capabilities", merged);
    ctx.store->update_node(patch);
    json_object_put(patch);
    g_cap_ok_ms = dvs::steady_ms_now();
    fprintf(stderr, "dvs-agent: startup capability merged\n");
  } else {
    fprintf(stderr, "dvs-agent: startup capability pull returned error\n");
  }
  json_object_put(co);
}

void agent_hotplug_poll_once(AgentContext& ctx) {
  if (!ctx.media_q || !ctx.store) return;
  std::string cr = ctx.media_q->request("capability", nullptr);
  json_object* co = json_tokener_parse(cr.c_str());
  if (!co) return;
  json_object* okv = nullptr;
  json_object* data = nullptr;
  if (!(json_object_object_get_ex(co, "ok", &okv) && okv &&
        json_object_get_boolean(okv) &&
        json_object_object_get_ex(co, "data", &data) && data)) {
    json_object_put(co);
    return;
  }
  json_object* caps = nullptr;
  json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
  json_object* merged =
      caps ? json_object_get(caps) : json_object_new_object();
  json_object_object_foreach(data, k, v) {
    json_object_object_add(merged, k, json_object_get(v));
  }
  std::string sig = agent_caps_ports_signature(merged);
  if (g_cap_ports_sig.empty())
    g_cap_ports_sig = agent_caps_ports_signature(caps);
  json_object* patch = json_object_new_object();
  json_object_object_add(patch, "capabilities", merged);
  ctx.store->update_node(patch);
  json_object_put(patch);
  g_cap_ok_ms = dvs::steady_ms_now();
  if (!g_cap_ports_sig.empty() && sig != g_cap_ports_sig) {
    g_cap_ports_sig = sig;
    revalidate_output_locked(ctx, merged);
    agent_audit("output", "hotplug", "reprobed", "port set changed");
    fprintf(stderr, "dvs-agent: hotplug port set changed\n");
  }
  json_object_put(co);
}

void agent_hotplug_loop(AgentContext& ctx) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    agent_hotplug_poll_once(ctx);
  }
}

void agent_note_cap_ok() { g_cap_ok_ms = dvs::steady_ms_now(); }

void agent_init_cap_sig(AgentContext& ctx) {
  json_object* caps0 = nullptr;
  json_object_object_get_ex(ctx.store->node(), "capabilities", &caps0);
  g_cap_ports_sig = agent_caps_ports_signature(caps0);
}

std::string agent_sync_pending_role(AgentContext& ctx,
                                    const std::string& want) {
  if (want != "input" && want != "output") return "";
  std::string cur = dvs::json_get_str(ctx.store->node(), "role");
  json_object* pp = json_object_new_object();
  std::string out;
  if (want == cur) {
    json_object_object_add(pp, "pendingRole", json_object_new_null());
    json_object_object_add(pp, "roleCleanup", json_object_new_null());
  } else {
    json_object_object_add(pp, "pendingRole",
                           json_object_new_string(want.c_str()));
    json_object_object_add(pp, "roleCleanup",
                           json_object_new_string("pending-reboot"));
    out = want;
  }
  ctx.store->update_node(pp);
  json_object_put(pp);
  return out;
}

void agent_project_playback(AgentContext& ctx, json_object* data) {
  if (!ctx.store || !data) return;
  int ready = 0, total = 0;
  json_object* lanes = nullptr;
  if (json_object_object_get_ex(data, "lanes", &lanes) && lanes &&
      json_object_is_type(lanes, json_type_array)) {
    total = (int)json_object_array_length(lanes);
    for (int i = 0; i < total; i++) {
      json_object* e = json_object_array_get_idx(lanes, i);
      json_object* rv = nullptr;
      if (e && json_object_object_get_ex(e, "ready", &rv) && rv &&
          json_object_get_boolean(rv))
        ready++;
    }
  } else {
    json_object* dec = nullptr;
    if (json_object_object_get_ex(data, "decoding", &dec) && dec)
      ready = json_object_get_int(dec);
  }
  json_object* patch = json_object_new_object();
  if (ready > 0) {
    json_object* cur = json_object_new_object();
    json_object_object_add(cur, "status", json_object_new_string("playing"));
    json_object_object_add(cur, "lanesReady", json_object_new_int(ready));
    json_object_object_add(cur, "lanesTotal", json_object_new_int(total));
    json_object_object_add(cur, "updatedAt",
                           json_object_new_string(dvs::iso_now().c_str()));
    json_object_object_add(patch, "currentPlayback", cur);
  } else {
    json_object_object_add(patch, "currentPlayback", json_object_new_null());
  }
  ctx.store->update_node(patch);
  json_object_put(patch);
}

bool agent_stage_layout(AgentContext& ctx, json_object* body,
                        std::string& resp_out, std::string& opid_out,
                        int& rev_out, int64_t& apply_at_ns_out,
                        std::string& err_out) {
  opid_out = dvs::json_get_str(body, "operationId");
  rev_out = 1;
  json_object* rv = nullptr;
  if (body && json_object_object_get_ex(body, "revision", &rv))
    rev_out = json_object_get_int(rv);
  apply_at_ns_out = 0;
  json_object* av = nullptr;
  if (body && json_object_object_get_ex(body, "applyAtNs", &av))
    apply_at_ns_out = (int64_t)json_object_get_int64(av);
  if (opid_out.empty()) {
    err_out = "missing operationId";
    return false;
  }
  json_object* prep = nullptr;
  if (!translate_platform_layout(body, &prep, err_out)) return false;
  resp_out = agent_media_cmd(ctx, "prepare", prep);
  json_object_put(prep);
  return true;
}

void agent_spawn_reporter(AgentContext& ctx, const std::string& opid, int rev,
                          int64_t apply_at_ns, const std::string& node_id) {
  AgentContext* ctxp = &ctx;
  std::string plat = ctx.platform_url;
  std::thread([ctxp, opid, rev, apply_at_ns, node_id, plat]() {
    // platform_url lives in ctx too, but capture by value: ctx outlives
    // main's sleep loop, yet be explicit about the uplink destination.
    (void)plat;
    report_commit(ctxp, opid, rev, apply_at_ns, node_id);
  }).detach();
}
