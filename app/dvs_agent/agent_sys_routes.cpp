#include "agent_sys_routes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "agent_audit.h"
#include "agent_context.h"
#include "agent_media.h"
#include "dvs_node_bundle.h"
#include "maintenance.h"
#include "media_ipc.h"
#include "node_store.h"
#include "registration.h"

namespace {
json_object* node_capacity_json() {
  json_object* o = json_object_new_object();
  long long mem_total = 0, mem_avail = 0;
  {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
      char key[64];
      long long val = 0;
      if (sscanf(line.c_str(), "%63[^:]: %lld", key, &val) == 2) {
        if (strcmp(key, "MemTotal") == 0)
          mem_total = val;
        else if (strcmp(key, "MemAvailable") == 0)
          mem_avail = val;
      }
    }
  }
  double l1 = 0, l5 = 0, l15 = 0, uptime = 0;
  {
    std::ifstream f("/proc/loadavg");
    std::string line;
    if (std::getline(f, line))
      sscanf(line.c_str(), "%lf %lf %lf", &l1, &l5, &l15);
  }
  {
    std::ifstream f("/proc/uptime");
    double idle = 0;
    if (f >> uptime >> idle) {
    }
  }
  double cpu_pct = 0.0;
  {
    std::ifstream f("/proc/stat");
    std::string line;
    if (std::getline(f, line) && line.rfind("cpu ", 0) == 0) {
      long long u = 0, n = 0, s = 0, idle = 0, iow = 0, irq = 0, sirq = 0,
                steal = 0;
      sscanf(line.c_str() + 4, "%lld %lld %lld %lld %lld %lld %lld %lld", &u,
             &n, &s, &idle, &iow, &irq, &sirq, &steal);
      long long idle_all = idle + iow;
      long long total = u + n + s + idle + iow + irq + sirq + steal;
      if (total > 0)
        cpu_pct = 100.0 * (double)(total - idle_all) / (double)total;
    }
  }
  json_object_object_add(o, "cpuBusyPercent", json_object_new_double(cpu_pct));
  json_object_object_add(o, "memTotalKB", json_object_new_int64(mem_total));
  json_object_object_add(o, "memAvailableKB",
                         json_object_new_int64(mem_avail));
  int mem_used_pct =
      mem_total > 0
          ? (int)(100.0 * (double)(mem_total - mem_avail) / (double)mem_total)
          : 0;
  json_object_object_add(o, "memUsedPercent", json_object_new_int(mem_used_pct));
  json_object_object_add(o, "load1", json_object_new_double(l1));
  json_object_object_add(o, "load5", json_object_new_double(l5));
  json_object_object_add(o, "load15", json_object_new_double(l15));
  json_object_object_add(o, "uptimeSec",
                         json_object_new_int64((long long)uptime));
  return o;
}
}  // namespace

bool agent_handle_sys(AgentContext& ctx, const HttpRequest& req,
                      HttpResponse& res) {
  if (req.method == "GET" && req.path == "/api/v1/node/info") {
    json_object* base = json_object_get(ctx.store->node());
    json_object* cap = node_capacity_json();
    json_object* metrics = json_object_new_object();
    json_object* v = nullptr;
    if (json_object_object_get_ex(cap, "cpuBusyPercent", &v) && v)
      json_object_object_add(metrics, "cpuBusyPercent", json_object_get(v));
    if (json_object_object_get_ex(cap, "memUsedPercent", &v) && v)
      json_object_object_add(metrics, "memUsedPercent", json_object_get(v));
    if (json_object_object_get_ex(cap, "memTotalKB", &v) && v)
      json_object_object_add(metrics, "memTotalKB", json_object_get(v));
    if (json_object_object_get_ex(cap, "memAvailableKB", &v) && v)
      json_object_object_add(metrics, "memAvailableKB", json_object_get(v));
    if (json_object_object_get_ex(cap, "load1", &v) && v)
      json_object_object_add(metrics, "load1", json_object_get(v));
    if (json_object_object_get_ex(cap, "uptimeSec", &v) && v)
      json_object_object_add(metrics, "uptimeSec", json_object_get(v));
    long temp = 0;
    {
      std::ifstream tf("/sys/class/thermal/thermal_zone0/temp");
      if (tf) tf >> temp;
    }
    if (temp > 0) {
      char tbuf[32];
      snprintf(tbuf, sizeof(tbuf), "%.1fC", temp / 1000.0);
      json_object_object_add(metrics, "temperature",
                             json_object_new_string(tbuf));
    }
    json_object_object_add(base, "metrics", metrics);
    json_object_put(cap);
    json_object* link = json_object_new_object();
    json_object_object_add(
        link, "state",
        json_object_new_string(agent_engine_link_state(ctx).c_str()));
    json_object_object_add(base, "engineLink", link);
    agent_send_json(res, agent_envelope_ok(base, ""));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/role") {
    json_object* n = ctx.store->node();
    json_object* d = json_object_new_object();
    json_object* role = nullptr;
    json_object_object_get_ex(n, "role", &role);
    json_object* pr = nullptr;
    json_object_object_get_ex(n, "pendingRole", &pr);
    json_object_object_add(d, "role",
                           role ? json_object_get(role)
                                : json_object_new_null());
    json_object_object_add(d, "pendingRole",
                           pr ? json_object_get(pr) : json_object_new_null());
    json_object* rc = nullptr;
    json_object_object_get_ex(n, "roleCleanup", &rc);
    json_object_object_add(d, "roleCleanup",
                           rc ? json_object_get(rc) : json_object_new_null());
    json_object* cr = nullptr;
    json_object_object_get_ex(n, "configRevision", &cr);
    json_object* rrev = nullptr;
    if (cr) json_object_object_get_ex(cr, "role", &rrev);
    int rev = 1;
    if (rrev) {
      json_object* dd = nullptr;
      if (json_object_object_get_ex(rrev, "desired", &dd))
        rev = json_object_get_int(dd);
    }
    json_object_object_add(d, "revision", json_object_new_int(rev));
    agent_send_json(res, agent_envelope_ok(d, ""));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/capability") {
    std::string cr = agent_media_query(ctx, "capability", nullptr);
    json_object* co = json_tokener_parse(cr.c_str());
    if (co) {
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
        agent_note_cap_ok();
      }
      json_object_put(co);
    }
    json_object* n = ctx.store->node();
    json_object* cap = nullptr;
    json_object_object_get_ex(n, "capabilities", &cap);
    json_object* cap_out =
        cap ? json_object_get(cap) : json_object_new_object();
    long long age = agent_cap_age_ms();
    bool stale = agent_cap_stale();
    json_object_object_add(cap_out, "capStale",
                           json_object_new_boolean(stale ? 1 : 0));
    json_object_object_add(cap_out, "capAgeMs", json_object_new_int64(age));
    agent_send_json(res, agent_envelope_ok(cap_out, ""));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/register") {
    bool ok = ctx.reg ? ctx.reg->trigger_once() : false;
    agent_audit("registration", "manual-register", ok ? "applied" : "failed",
                ok ? "registered" : "trigger failed");
    json_object* d = json_object_new_object();
    json_object_object_add(d, "registerState",
                           json_object_new_string(ok ? "registered" : "failed"));
    json_object_object_add(d, "online",
                           json_object_new_boolean(
                               ctx.reg && ctx.reg->online() ? 1 : 0));
    agent_send_json(res,
                    agent_envelope_ok(d, ok ? "设备注册成功" : "注册失败，可修改地址后重试"));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/upgrade/upload") {
    std::string fn = req.query.count("filename") ? req.query.at("filename")
                                                 : "fw_upload.bin";
    auto hit = req.headers.find("X-Filename");
    if (hit != req.headers.end() && !hit->second.empty()) fn = hit->second;
    std::string base = fn;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.empty() || base.size() > 128) {
      agent_send_json(res,
                      agent_envelope_fail("INVALID_ARGUMENT", 400, "文件名非法"));
      return true;
    }
    std::string dest = "/tmp/" + base;
    std::ofstream out(dest, std::ios::binary);
    if (!out) {
      agent_send_json(res, agent_envelope_fail("INTERNAL", 500, "无法写入暂存"));
      return true;
    }
    out.write(req.body.data(), (std::streamsize)req.body.size());
    out.close();
    json_object* d = json_object_new_object();
    json_object_object_add(d, "path", json_object_new_string(dest.c_str()));
    json_object_object_add(d, "size",
                           json_object_new_int64((int64_t)req.body.size()));
    agent_send_json(res, agent_envelope_ok(d, "固件已上传"));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/capacity") {
    agent_send_json(res, agent_envelope_ok(node_capacity_json(), ""));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/node/version") {
    std::string hr = agent_media_query(ctx, "hello", nullptr);
    bool answered = false, match = false;
    std::string eb;
    int ei = -1;
    MediaIpcClient::parse_hello(hr, answered, match, eb, ei);
    agent_media_note_hello(ctx, hr);
    std::string gate_reason;
    bool allowed = agent_media_layout_allowed(ctx, gate_reason);
    json_object* ho = json_tokener_parse(hr.c_str());
    json_object* d = json_object_new_object();
    json_object_object_add(d, "bundleVersion",
                           json_object_new_string(DVS_NODE_BUNDLE_VERSION));
    json_object_object_add(d, "ipcVersion",
                           json_object_new_int(DVS_MEDIA_IPC_VERSION));
    json_object_object_add(d, "media",
                           ho ? ho : json_object_new_object());
    json_object_object_add(d, "match", json_object_new_boolean(match ? 1 : 0));
    json_object_object_add(d, "gateBlocked",
                           json_object_new_boolean(allowed ? 0 : 1));
    json_object_object_add(d, "gateReason",
                           json_object_new_string(gate_reason.c_str()));
    json_object* link = json_object_new_object();
    json_object_object_add(
        link, "state",
        json_object_new_string(agent_engine_link_state(ctx).c_str()));
    json_object_object_add(link, "failStreak",
                           json_object_new_int(ctx.media_q
                                                   ? ctx.media_q->fail_streak()
                                                   : 0));
    long long ok_age = -1;
    if (ctx.media_q && ctx.media_q->last_ok_ms() > 0)
      ok_age = agent_now_ms() - ctx.media_q->last_ok_ms();
    json_object_object_add(link, "lastOkAgeMs", json_object_new_int64(ok_age));
    json_object_object_add(d, "engineLink", link);
    agent_send_json(res, agent_envelope_ok(d, ""));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/reboot") {
    agent_audit("maintenance", "reboot", "applied", "reboot requested");
    agent_send_json(res, agent_envelope_ok(ctx.maint->reboot(), ""));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/factory-reset") {
    agent_audit("maintenance", "factory-reset", "applied", "reset requested");
    agent_send_json(res, agent_envelope_ok(ctx.maint->factory_reset(), ""));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/upgrade") {
    json_object* body = json_tokener_parse(req.body.c_str());
    std::string fn = body ? agent_get_str(body, "filename") : std::string("fw.bin");
    if (fn.empty()) fn = "fw.bin";
    std::string sha = body ? agent_get_str(body, "sha256") : std::string();
    if (body) json_object_put(body);
    json_object* ur = ctx.maint->upgrade(fn, sha);
    bool uok = false;
    if (ur) {
      json_object* okv = nullptr;
      if (json_object_object_get_ex(ur, "ok", &okv) && okv)
        uok = json_object_get_boolean(okv) != 0;
    }
    agent_audit("maintenance", "upgrade", uok ? "applied" : "failed", fn);
    agent_send_json(res, agent_envelope_ok(ur, ""));
    return true;
  }
  if (req.method == "GET" && req.path == "/api/v1/audit-events") {
    int page = 1, pageSize = 100;
    auto itp = req.query.find("page");
    if (itp != req.query.end()) page = std::max(1, atoi(itp->second.c_str()));
    auto itp2 = req.query.find("pageSize");
    if (itp2 != req.query.end())
      pageSize = std::min(200, std::max(1, atoi(itp2->second.c_str())));
    int total = 0;
    json_object* items = agent_audit_page(page, pageSize, &total);
    json_object* d = json_object_new_object();
    json_object_object_add(d, "items", items);
    json_object_object_add(d, "page", json_object_new_int(page));
    json_object_object_add(d, "pageSize", json_object_new_int(pageSize));
    json_object_object_add(d, "total", json_object_new_int(total));
    agent_send_json(res, agent_envelope_ok(d, ""));
    return true;
  }
  return false;
}
