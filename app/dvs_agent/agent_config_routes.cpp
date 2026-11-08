#include "agent_config_routes.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "agent_audit.h"
#include "agent_auth.h"
#include "agent_context.h"
#include "agent_media.h"
#include "dvs_contract.h"
#include "network_apply.h"
#include "node_store.h"
#include "registration.h"
#include "tx_config.h"

namespace {
std::string trim_str(const std::string& s) {
  size_t a = 0;
  while (a < s.size() &&
         (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
    a++;
  size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                   s[b - 1] == '\n'))
    b--;
  return s.substr(a, b - a);
}

bool valid_ip4(const std::string& s) {
  if (s.empty() || s.size() > 15) return false;
  int parts = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == '.') {
      if (i == start || i - start > 3) return false;
      for (size_t j = start; j < i; j++)
        if (s[j] < '0' || s[j] > '9') return false;
      int v = atoi(s.substr(start, i - start).c_str());
      if (v < 0 || v > 255) return false;
      parts++;
      start = i + 1;
    }
  }
  return parts == 4;
}

bool parse_endpoint_item(const std::string& raw, std::string& ip_out,
                         int& port_out, bool& has_port_out) {
  std::string t = trim_str(raw);
  if (t.empty()) return false;
  size_t colon = t.find(':');
  if (colon == std::string::npos) {
    if (!valid_ip4(t)) return false;
    ip_out = t;
    port_out = 0;
    has_port_out = false;
    return true;
  }
  if (t.find(':', colon + 1) != std::string::npos) return false;
  std::string ip = trim_str(t.substr(0, colon));
  std::string ps = trim_str(t.substr(colon + 1));
  if (!valid_ip4(ip) || ps.empty() || ps.size() > 5) return false;
  for (char c : ps)
    if (c < '0' || c > '9') return false;
  int p = atoi(ps.c_str());
  if (p < 1 || p > 65535) return false;
  ip_out = ip;
  port_out = p;
  has_port_out = true;
  return true;
}

bool validate_whitelist_value(json_object* wv, std::string& bad_item) {
  if (!wv) return true;
  if (json_object_is_type(wv, json_type_string)) {
    std::string s = json_object_get_string(wv);
    bool any = false;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
      if (i == s.size() || s[i] == ',') {
        std::string tok = trim_str(s.substr(start, i - start));
        start = i + 1;
        if (tok.empty()) continue;
        any = true;
        std::string ip;
        int port = 0;
        bool hp = false;
        if (!parse_endpoint_item(tok, ip, port, hp)) {
          bad_item = tok;
          return false;
        }
      }
    }
    if (!any) {
      bad_item = s;
      return false;
    }
    return true;
  }
  if (json_object_is_type(wv, json_type_array)) {
    int n = json_object_array_length(wv);
    if (n <= 0) {
      bad_item = "empty whitelist";
      return false;
    }
    for (int i = 0; i < n; i++) {
      json_object* e = json_object_array_get_idx(wv, i);
      if (!e) {
        bad_item = "null entry";
        return false;
      }
      if (json_object_is_type(e, json_type_string)) {
        std::string s = json_object_get_string(e);
        size_t start = 0;
        bool any = false;
        for (size_t k = 0; k <= s.size(); k++) {
          if (k == s.size() || s[k] == ',') {
            std::string tok = trim_str(s.substr(start, k - start));
            start = k + 1;
            if (tok.empty()) continue;
            any = true;
            std::string ip;
            int port = 0;
            bool hp = false;
            if (!parse_endpoint_item(tok, ip, port, hp)) {
              bad_item = tok;
              return false;
            }
          }
        }
        if (!any) {
          bad_item = s;
          return false;
        }
      } else if (json_object_is_type(e, json_type_object)) {
        json_object* iv = nullptr;
        std::string ip;
        if (json_object_object_get_ex(e, "sip", &iv) && iv)
          ip = json_object_get_string(iv);
        ip = trim_str(ip);
        json_object* pv = nullptr;
        bool has_p = false;
        int p = 0;
        if (json_object_object_get_ex(e, "udp_port", &pv) && pv) {
          if (json_object_is_type(pv, json_type_string)) {
            std::string ps = trim_str(json_object_get_string(pv));
            if (ps.empty()) return false;
            for (char c : ps)
              if (c < '0' || c > '9') {
                bad_item = ip + ":" + ps;
                return false;
              }
            p = atoi(ps.c_str());
          } else {
            p = json_object_get_int(pv);
          }
          has_p = true;
        }
        if (!valid_ip4(ip)) {
          bad_item = ip.empty() ? "missing ip" : ip;
          return false;
        }
        if (has_p && (p < 1 || p > 65535)) {
          bad_item = ip + ":" + std::to_string(p);
          return false;
        }
      } else {
        bad_item = "bad entry type";
        return false;
      }
    }
    return true;
  }
  bad_item = "bad whitelist value";
  return false;
}

bool tcp_reachable(const std::string& host, int port, int timeout_ms) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char ps[16];
  snprintf(ps, sizeof(ps), "%d", port);
  struct addrinfo* ai = nullptr;
  if (getaddrinfo(host.c_str(), ps, &hints, &ai) != 0) return false;
  bool ok = false;
  for (struct addrinfo* p = ai; p; p = p->ai_next) {
    int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, p->ai_addr, p->ai_addrlen);
    if (rc == 0) {
      ok = true;
      close(fd);
      break;
    }
    if (errno == EINPROGRESS) {
      struct pollfd pfd{.fd = fd, .events = POLLOUT};
      if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t n = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && err == 0)
          ok = true;
      }
    }
    close(fd);
    if (ok) break;
  }
  freeaddrinfo(ai);
  return ok;
}

bool intent_log_network(const std::string& intent, std::string& err) {
  if (std::getenv("DVS_FORCE_NETWORK_FAIL") &&
      std::string(std::getenv("DVS_FORCE_NETWORK_FAIL")) == "1") {
    err = "injected failure (DVS_FORCE_NETWORK_FAIL=1)";
    return false;
  }
  fprintf(stderr, "dvs-agent: network intent stored (system untouched): %s\n",
          intent.c_str());
  return true;
}

// Accept the platform-owned config subset. Only geometry and role apply;
// every other key is reported as ignored and never stored.
bool handle_config_sync(AgentContext& ctx, const HttpRequest& req,
                        HttpResponse& res) {
  if (!agent_bearer_ok(ctx, req)) {
    agent_send_json(res, agent_envelope_fail("UNAUTHORIZED", 401,
                                            "missing or bad token"));
    return true;
  }
  json_object* body = json_tokener_parse(req.body.c_str());
  std::string verr = body ? dvs::contract::validate_config_update(body)
                          : "INVALID_ARGUMENT:empty body";
  if (!verr.empty()) {
    if (body) json_object_put(body);
    agent_send_json(res,
                    agent_envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
    return true;
  }
  json_object* cfg = nullptr;
  if (body) json_object_object_get_ex(body, "config", &cfg);
  json_object* patch = json_object_new_object();
  json_object* ignored = json_object_new_array();
  std::string role_err;
  if (cfg && json_object_is_type(cfg, json_type_object)) {
    json_object_object_foreach(cfg, key, val) {
      std::string k(key);
      if (k == "geometry") {
        json_object_object_add(patch, "geometry", json_object_get(val));
      } else if (k == "role") {
        std::string want;
        json_object* rv = nullptr;
        if (json_object_is_type(val, json_type_object) &&
            json_object_object_get_ex(val, "role", &rv) && rv)
          want = json_object_get_string(rv);
        else if (json_object_is_type(val, json_type_string))
          want = json_object_get_string(val);
        if (want != "input" && want != "output") {
          role_err = std::string("INVALID_ARGUMENT: illegal role '") + want + "'";
          ctx.store->mark_failed("role", "INVALID_ARGUMENT", "角色取值非法");
          agent_audit("role", "platform-sync", "failed", role_err);
        } else {
          ctx.store->set_desired("role", json_object_get(val), nullptr);
          agent_sync_pending_role(ctx, want);
          agent_audit("role", "platform-sync", "pending",
                      std::string("pending ") + want);
        }
      } else {
        json_object_array_add(ignored, json_object_new_string(k.c_str()));
      }
    }
  }
  json_object_object_add(patch, "configSource",
                         json_object_new_string("platform"));
  ctx.store->update_node(patch);
  json_object_put(patch);
  if (!role_err.empty()) {
    if (body) json_object_put(body);
    agent_send_json(res, agent_envelope_fail("INVALID_ARGUMENT", 400,
                                            role_err.c_str()));
    return true;
  }
  json_object* d = json_object_new_object();
  json_object_object_add(d, "applied", json_object_new_boolean(1));
  json_object_object_add(d, "source", json_object_new_string("platform"));
  json_object_object_add(d, "ignored", ignored);
  if (body) json_object_put(body);
  agent_send_json(res, agent_envelope_ok(d, "config synced"));
  return true;
}
}  // namespace

bool agent_handle_config(AgentContext& ctx, const HttpRequest& req,
                         HttpResponse& res) {
  // Exact sync path first: it shares the /config/ prefix, and the generic
  // scope block below would reject "sync" as an unknown scope, leaving this
  // route unreachable. Exact match wins.
  if (req.method == "POST" && req.path == "/api/v1/node/config/sync")
    return handle_config_sync(ctx, req, res);

  std::string prefix = "/api/v1/node/config/";
  if (req.path.rfind(prefix, 0) != 0) return false;
  std::string rest = req.path.substr(prefix.size());
  std::string scope = rest;
  std::string action;
  size_t slash = rest.find('/');
  if (slash != std::string::npos) {
    scope = rest.substr(0, slash);
    action = rest.substr(slash + 1);
  }
  bool valid = false;
  for (int i = 0; i < 6; i++)
    if (scope == NodeStore::SCOPES[i]) valid = true;
  if (!valid) {
    agent_send_json(res, agent_envelope_fail("INVALID_ARGUMENT", 400,
                                            "不支持的配置范围"));
    return true;
  }
  std::string node_id = req.query.count("nodeId")
                            ? req.query.at("nodeId")
                            : agent_get_str(ctx.store->node(), "id");
  if (req.method == "GET" && action.empty()) {
    json_object* cfg = ctx.store->get_config(scope, node_id);
    agent_send_json(res, agent_envelope_ok(cfg, ""));
    return true;
  }
  if (req.method == "POST" && action.empty()) {
    json_object* body = json_tokener_parse(req.body.c_str());
    json_object* dv = nullptr;
    if (body) {
      if (!json_object_object_get_ex(body, "desiredValue", &dv))
        json_object_object_get_ex(body, "value", &dv);
    }
    json_object* val = dv ? json_object_get(dv) : json_object_new_object();
    const char* opid = nullptr;
    std::string opstr;
    if (body) {
      json_object* o = nullptr;
      if (json_object_object_get_ex(body, "operationId", &o) && o) {
        opstr = json_object_get_string(o);
        opid = opstr.c_str();
      }
    }
    std::string cur_role = agent_get_str(ctx.store->node(), "role");
    ctx.store->set_desired(scope, val, opid);
    std::string fail_code;
    std::string fail_reason;
    std::string scope_ok_msg;
    json_object* node_patch = json_object_new_object();
    bool forward_media = false;
    json_object* fwd = nullptr;
    if (scope == "role") {
      json_object* r = nullptr;
      std::string want =
          (val && json_object_object_get_ex(val, "role", &r) && r)
              ? json_object_get_string(r)
              : "";
      if (want != "input" && want != "output") {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "角色取值非法";
      } else {
        std::string pend = agent_sync_pending_role(ctx, want);
        if (pend.empty()) ctx.store->mark_applied(scope, 0);
        agent_audit(scope, "save",
                    fail_code.empty()
                        ? (pend.empty() ? "applied" : "pending")
                        : "failed",
                    pend.empty() ? "already active" : ("pending " + pend));
      }
    } else if (scope == "input") {
      json_object* pv = nullptr;
      std::string port =
          (val && json_object_object_get_ex(val, "port", &pv) && pv)
              ? json_object_get_string(pv)
              : "";
      bool ok = !port.empty() && port.size() < 32;
      if (ok) {
        json_object* caps = nullptr;
        json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
        json_object* arr = nullptr;
        if (caps && json_object_object_get_ex(caps, "inputPorts", &arr) && arr &&
            json_object_is_type(arr, json_type_array)) {
          if (json_object_array_length(arr) == 0) {
            ok = false;
            fail_code = "INVALID_ARGUMENT";
            fail_reason = "无可用采集口（节点上报零端口）";
          } else {
            ok = false;
            for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
              json_object* e = json_object_array_get_idx(arr, (int)i);
              json_object* idv = nullptr;
              if (e && json_object_object_get_ex(e, "id", &idv) && idv &&
                  port == json_object_get_string(idv)) {
                ok = true;
                break;
              }
            }
            if (!ok) {
              fail_code = "INVALID_ARGUMENT";
              fail_reason = "采集口超出本机能力";
            }
          }
        }
      } else {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "采集口不能为空";
      }
      if (fail_code.empty()) {
        json_object* wcheck = nullptr;
        if (val) json_object_object_get_ex(val, "whitelist", &wcheck);
        if (wcheck) {
          std::string bad;
          if (!validate_whitelist_value(wcheck, bad)) {
            fail_code = "INVALID_ARGUMENT";
            fail_reason = std::string("白名单地址/端口非法: ") + bad;
          }
        }
      }
      if (fail_code.empty()) {
        TxParams txp;
        std::string tx_err;
        if (!tx_parse(val, txp, tx_err)) {
          fail_code = "INVALID_ARGUMENT";
          fail_reason = tx_err;
        } else {
          json_object* caps = nullptr;
          json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
          TxCaps txcaps = tx_caps_from_node(caps);
          if (!tx_validate(txp, txcaps, tx_err)) {
            fail_code = "INVALID_ARGUMENT";
            fail_reason = tx_err;
          }
        }
        if (fail_code.empty()) {
          json_object* in = json_object_new_object();
          json_object_object_add(in, "port",
                                 json_object_new_string(txp.port.c_str()));
          if (txp.hasEncode) {
            json_object* enc = json_object_new_object();
            json_object_object_add(
                enc, "codec", json_object_new_string(txp.enc.codec.c_str()));
            json_object_object_add(enc, "width",
                                   json_object_new_int(txp.enc.width));
            json_object_object_add(enc, "height",
                                   json_object_new_int(txp.enc.height));
            json_object_object_add(enc, "fps", json_object_new_int(txp.enc.fps));
            json_object_object_add(enc, "bitrateKbps",
                                   json_object_new_int(txp.enc.bitrate));
            json_object_object_add(in, "encode", enc);
          }
          if (txp.hasTarget) {
            json_object* tgt = json_object_new_object();
            json_object_object_add(
                tgt, "sip", json_object_new_string(txp.tgt.sip.c_str()));
            json_object_object_add(tgt, "udpPort",
                                   json_object_new_int(txp.tgt.udpPort));
            json_object_object_add(in, "target", tgt);
          }
          json_object_object_add(node_patch, "input", in);
          json_object* wv = nullptr;
          if (val) json_object_object_get_ex(val, "whitelist", &wv);
          if (wv) {
            fwd = json_object_new_object();
            json_object_object_add(fwd, "whitelist", json_object_get(wv));
            forward_media = true;
          }
          ctx.store->update_node(node_patch);
          json_object_put(node_patch);
          node_patch = json_object_new_object();
          bool tx_requested = txp.hasEncode || txp.hasTarget;
          if (!tx_requested) {
            ctx.store->mark_applied(scope, 0);
            scope_ok_msg = "已保存·采集口已存储（未下发引擎）";
          } else if (!txp.hasEncode || !txp.hasTarget) {
            fail_code = "INVALID_ARGUMENT";
            fail_reason = "发送需同时给出编码与目标参数";
          } else {
            int tx_want_rev =
                agent_config_desired_revision(ctx, scope, node_id);
            json_object* rv = json_object_new_object();
            json_object_object_add(rv, "scope",
                                   json_object_new_string("role"));
            json_object* rval = json_object_new_object();
            json_object_object_add(rval, "role",
                                   json_object_new_string(cur_role.c_str()));
            json_object_object_add(rv, "value", rval);
            std::string rr = agent_media_cmd(ctx, "configure", rv);
            (void)rr;
            json_object_put(rv);
            json_object* payload = tx_start_payload(txp);
            std::string tr = agent_media_cmd(ctx, "tx_start", payload);
            json_object_put(payload);
            json_object* to = json_tokener_parse(tr.c_str());
            bool ok2 = false;
            std::string code, msg, state;
            if (to) {
              json_object* okv = nullptr;
              if (json_object_object_get_ex(to, "ok", &okv) && okv)
                ok2 = json_object_get_boolean(okv) != 0;
              json_object* cv = nullptr;
              if (json_object_object_get_ex(to, "code", &cv) && cv)
                code = json_object_get_string(cv);
              json_object* mv2 = nullptr;
              if (json_object_object_get_ex(to, "msg", &mv2) && mv2)
                msg = json_object_get_string(mv2);
              json_object* dd = nullptr;
              if (json_object_object_get_ex(to, "data", &dd) && dd)
                state = agent_get_str(dd, "state");
              json_object_put(to);
            } else {
              code = "TX_START_UNPARSABLE";
              msg = "media engine response unparsable";
            }
            if (agent_config_desired_revision(ctx, scope, node_id) !=
                tx_want_rev) {
              fprintf(stderr,
                      "dvs-agent: tx_start (%s) stale rev %d vs %d, skip\n",
                      scope.c_str(), tx_want_rev,
                      agent_config_desired_revision(ctx, scope, node_id));
            } else if (!ok2) {
              fail_code = code.empty() ? "TX_START_FAILED" : code;
              fail_reason = msg.empty() ? "media engine refused tx start" : msg;
              ctx.store->mark_failed(scope, fail_code.c_str(),
                                    fail_reason.c_str());
            } else if (state == "running") {
              ctx.store->mark_applied(scope, 0);
              scope_ok_msg = "输入采集发送已启动";
            } else {
              ctx.store->mark_applied(scope, 0);
              scope_ok_msg = "参数已下发，数据面待单板联调（TX_BACKEND_PENDING）";
            }
          }
        }
      }
    } else if (scope == "output") {
      json_object* wv = nullptr;
      json_object* hv = nullptr;
      json_object* zv = nullptr;
      json_object* mv = nullptr;
      json_object* cv = nullptr;
      int w = (val && json_object_object_get_ex(val, "width", &wv) && wv)
                  ? json_object_get_int(wv)
                  : 0;
      int h = (val && json_object_object_get_ex(val, "height", &hv) && hv)
                  ? json_object_get_int(hv)
                  : 0;
      int z = (val && json_object_object_get_ex(val, "refreshHz", &zv) && zv)
                  ? json_object_get_int(zv)
                  : 0;
      std::string mode =
          (val && json_object_object_get_ex(val, "mode", &mv) && mv)
              ? json_object_get_string(mv)
              : "force";
      std::string conn =
          (val && json_object_object_get_ex(val, "connector", &cv) && cv)
              ? json_object_get_string(cv)
              : "";
      int lim_w = 7680, lim_h = 4320, lim_z = 60;
      bool conn_known = false;
      json_object* caps = nullptr;
      json_object_object_get_ex(ctx.store->node(), "capabilities", &caps);
      json_object* arr = nullptr;
      if (caps && json_object_object_get_ex(caps, "outputPorts", &arr) && arr &&
          json_object_is_type(arr, json_type_array)) {
        if (json_object_array_length(arr) == 0) {
          fail_code = "INVALID_ARGUMENT";
          fail_reason = "无可用输出口（节点上报零端口）";
        } else {
          for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
            json_object* e = json_object_array_get_idx(arr, (int)i);
            json_object* idv = nullptr;
            if (!e || !json_object_object_get_ex(e, "id", &idv) || !idv)
              continue;
            if (!conn.empty() && conn != json_object_get_string(idv)) continue;
            conn_known = true;
            json_object* q = nullptr;
            if (json_object_object_get_ex(e, "maxWidth", &q) && q)
              lim_w = json_object_get_int(q);
            if (json_object_object_get_ex(e, "maxHeight", &q) && q)
              lim_h = json_object_get_int(q);
            if (json_object_object_get_ex(e, "maxRefreshHz", &q) && q)
              lim_z = json_object_get_int(q);
            break;
          }
          if (!conn.empty() && !conn_known) {
            fail_code = "INVALID_ARGUMENT";
            fail_reason = "输出口不存在";
          }
        }
      }
      if (fail_code.empty()) {
        if (w < 640 || w > lim_w || h < 480 || h > lim_h || z < 24 ||
            z > lim_z) {
          fail_code = "INVALID_ARGUMENT";
          fail_reason = "输出参数超出本机能力范围";
        } else if (mode != "force" && mode != "hotplug") {
          fail_code = "INVALID_ARGUMENT";
          fail_reason = "输出模式取值非法";
        }
      }
      if (fail_code.empty()) {
        json_object_object_add(node_patch, "output", json_object_get(val));
        ctx.store->update_node(node_patch);
        json_object_put(node_patch);
        node_patch = json_object_new_object();
        fwd = json_object_new_object();
        json_object_object_add(fwd, "scope", json_object_new_string("output"));
        json_object_object_add(fwd, "value", json_object_get(val));
        forward_media = true;
      }
    } else if (scope == "network") {
      json_object* mv = nullptr;
      std::string nmode =
          (val && json_object_object_get_ex(val, "mode", &mv) && mv)
              ? json_object_get_string(mv)
              : "dhcp";
      json_object* iv = nullptr;
      json_object* kv = nullptr;
      json_object* gv = nullptr;
      json_object* dvv = nullptr;
      json_object* sv = nullptr;
      json_object* ifv = nullptr;
      json_object* cfv = nullptr;
      std::string ip =
          (val && json_object_object_get_ex(val, "ip", &iv) && iv)
              ? json_object_get_string(iv)
              : "";
      std::string mask =
          (val && json_object_object_get_ex(val, "mask", &kv) && kv)
              ? json_object_get_string(kv)
              : "";
      std::string gw =
          (val && json_object_object_get_ex(val, "gateway", &gv) && gv)
              ? json_object_get_string(gv)
              : "";
      std::string dns =
          (val && json_object_object_get_ex(val, "dns", &dvv) && dvv)
              ? json_object_get_string(dvv)
              : "";
      std::string srv =
          (val && json_object_object_get_ex(val, "server", &sv) && sv)
              ? json_object_get_string(sv)
              : "";
      std::string iface =
          (val && json_object_object_get_ex(val, "iface", &ifv) && ifv)
              ? json_object_get_string(ifv)
              : "eth0";
      bool confirm = val && json_object_object_get_ex(val, "confirm", &cfv) &&
                     cfv && json_object_get_boolean(cfv);
      std::string srv_host;
      int srv_port = 0;
      std::string srv_err;
      if (nmode != "dhcp" && nmode != "static") {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "网络方式非法";
      } else if (nmode == "static" &&
                 (!valid_ip4(ip) || !valid_ip4(mask) || !valid_ip4(gw) ||
                  !valid_ip4(dns))) {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "静态地址格式不正确";
      } else if (srv.empty()) {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "缺少注册服务器地址";
      } else if (!net_parse_server(srv, srv_host, srv_port, srv_err)) {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = std::string("注册服务器地址非法: ") + srv_err;
      } else if (!net_valid_iface(iface)) {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "网卡名非法";
      }
      if (fail_code.empty()) {
        json_object* net_old = nullptr;
        json_object_object_get_ex(ctx.store->node(), "network", &net_old);
        json_object* net_old_copy =
            net_old ? json_object_get(net_old) : json_object_new_object();
        std::string ip_old = agent_get_str(ctx.store->node(), "ip");
        NetExecParams nep;
        nep.mode = nmode;
        nep.ip = ip;
        nep.mask = mask;
        nep.gw = gw;
        nep.dns = dns;
        nep.server = srv;
        nep.iface = iface;
        nep.opid = opstr;
        bool executed = false;
        bool sys_ok = false;
        bool sys_rb = false;
        std::string sys_err;
        std::string method = "systemd-networkd";
        if (net_execution_requested(confirm, opstr)) {
          NetExecResult nr = net_apply_real(nep);
          executed = nr.executed;
          sys_ok = nr.ok;
          sys_rb = nr.rolledBack;
          sys_err = nr.error;
          method = nr.method;
        } else {
          std::string intent = std::string("{\"mode\":\"") + nmode +
                               "\",\"iface\":\"" + iface + "\",\"server\":\"" +
                               srv + "\"}";
          sys_ok = intent_log_network(intent, sys_err);
        }
        json_object* nsys = json_object_new_object();
        json_object_object_add(nsys, "executed",
                               json_object_new_boolean(executed ? 1 : 0));
        json_object_object_add(nsys, "method",
                               json_object_new_string(method.c_str()));
        json_object_object_add(
            nsys, "at", json_object_new_string(agent_iso_now().c_str()));
        if (!sys_err.empty())
          json_object_object_add(nsys, "error",
                                 json_object_new_string(sys_err.c_str()));
        json_object_object_add(nsys, "rolledBack",
                               json_object_new_boolean(sys_rb ? 1 : 0));
        if (!sys_ok) {
          json_object* rb = json_object_new_object();
          json_object_object_add(rb, "network", net_old_copy);
          json_object_object_add(
              rb, "ip", json_object_new_string(ip_old.c_str()));
          json_object_object_add(rb, "networkSystem", nsys);
          ctx.store->update_node(rb);
          json_object_put(rb);
          fail_code = "NET_APPLY_FAILED";
          fail_reason =
              executed ? (std::string("网络系统应用失败，已回滚: ") + sys_err)
                       : (std::string("网络意图落库失败: ") + sys_err);
          json_object_put(net_old_copy);
        } else {
          json_object_put(net_old_copy);
          json_object* net = json_object_new_object();
          json_object_object_add(net, "mode",
                                 json_object_new_string(nmode.c_str()));
          if (nmode == "static") {
            json_object_object_add(net, "mask",
                                   json_object_new_string(mask.c_str()));
            json_object_object_add(net, "gateway",
                                   json_object_new_string(gw.c_str()));
            json_object_object_add(net, "dns",
                                   json_object_new_string(dns.c_str()));
            json_object_object_add(net, "server",
                                   json_object_new_string(srv.c_str()));
            json_object_object_add(net, "iface",
                                   json_object_new_string(iface.c_str()));
            json_object_object_add(node_patch, "ip",
                                   json_object_new_string(ip.c_str()));
            json_object_object_add(net, "ip", json_object_new_string(ip.c_str()));
          } else {
            json_object_object_add(net, "server",
                                   json_object_new_string(srv.c_str()));
            json_object_object_add(net, "iface",
                                   json_object_new_string(iface.c_str()));
          }
          json_object_object_add(node_patch, "network", net);
          json_object_object_add(node_patch, "networkSystem", nsys);
          ctx.store->update_node(node_patch);
          json_object_put(node_patch);
          node_patch = json_object_new_object();
          ctx.store->mark_applied(scope, 0);
          scope_ok_msg = executed ? "系统网络已切换并通过连通检查"
                                  : "意图已保存，系统网络未切换（需隔离环境确认执行）";
        }
      }
    } else if (scope == "registration") {
      json_object* mv = nullptr;
      json_object* sv = nullptr;
      std::string rm =
          (val && json_object_object_get_ex(val, "mode", &mv) && mv)
              ? json_object_get_string(mv)
              : "";
      std::string srv =
          (val && json_object_object_get_ex(val, "server", &sv) && sv)
              ? json_object_get_string(sv)
              : "";
      if ((rm != "auto" && rm != "manual") || srv.empty()) {
        fail_code = "INVALID_ARGUMENT";
        fail_reason = "缺少注册服务器地址";
      } else {
        json_object_object_add(node_patch, "registerMode",
                               json_object_new_string(rm.c_str()));
        json_object* net0 = nullptr;
        json_object_object_get_ex(ctx.store->node(), "network", &net0);
        json_object* net =
            net0 ? json_object_get(net0) : json_object_new_object();
        if (!json_object_is_type(net, json_type_object)) {
          json_object_put(net);
          net = json_object_new_object();
        }
        json_object_object_add(net, "server",
                               json_object_new_string(srv.c_str()));
        json_object_object_add(node_patch, "network", net);
        ctx.store->update_node(node_patch);
        json_object_put(node_patch);
        node_patch = json_object_new_object();
        ctx.store->mark_applied(scope, 0);
        if (rm == "auto" && ctx.reg) ctx.reg->trigger_async();
      }
    } else if (scope == "sip") {
      json_object* ev = nullptr;
      bool enabled = val && json_object_object_get_ex(val, "enabled", &ev) &&
                     ev && json_object_get_boolean(ev);
      if (!enabled) {
        json_object* sip = json_object_new_object();
        json_object_object_add(sip, "enabled", json_object_new_boolean(0));
        json_object_object_add(sip, "state",
                               json_object_new_string("disabled"));
        json_object_object_add(sip, "mode",
                               json_object_new_string("connectivity-probe"));
        json_object_object_add(node_patch, "sip", sip);
        ctx.store->update_node(node_patch);
        json_object_put(node_patch);
        node_patch = json_object_new_object();
        ctx.store->mark_applied(scope, 0);
      } else {
        json_object* sv = nullptr;
        json_object* pv = nullptr;
        json_object* dvv = nullptr;
        json_object* uv = nullptr;
        std::string srv =
            (val && json_object_object_get_ex(val, "server", &sv) && sv)
                ? json_object_get_string(sv)
                : "";
        int port = (val && json_object_object_get_ex(val, "port", &pv) && pv)
                       ? json_object_get_int(pv)
                       : 0;
        std::string dev =
            (val && json_object_object_get_ex(val, "deviceId", &dvv) && dvv)
                ? json_object_get_string(dvv)
                : "";
        std::string usr =
            (val && json_object_object_get_ex(val, "user", &uv) && uv)
                ? json_object_get_string(uv)
                : "";
        if (srv.empty() || port < 1 || port > 65535 || dev.empty() ||
            usr.empty()) {
          fail_code = "INVALID_ARGUMENT";
          fail_reason = "SIP 参数不完整";
        } else {
          json_object* probing = json_object_new_object();
          json_object* sip_probe = json_object_new_object();
          json_object_object_add(sip_probe, "enabled", json_object_new_boolean(1));
          json_object_object_add(sip_probe, "state",
                                 json_object_new_string("probing"));
          json_object_object_add(sip_probe, "mode",
                                 json_object_new_string("connectivity-probe"));
          json_object_object_add(sip_probe, "server",
                                 json_object_new_string(srv.c_str()));
          json_object_object_add(sip_probe, "port", json_object_new_int(port));
          json_object_object_add(probing, "sip", sip_probe);
          ctx.store->update_node(probing);
          json_object_put(probing);
          if (!tcp_reachable(srv, port, 3000)) {
            json_object* sip = json_object_new_object();
            json_object_object_add(sip, "enabled", json_object_new_boolean(1));
            json_object_object_add(sip, "state",
                                   json_object_new_string("failed"));
            json_object_object_add(sip, "mode",
                                   json_object_new_string("connectivity-probe"));
            json_object_object_add(sip, "server",
                                   json_object_new_string(srv.c_str()));
            json_object_object_add(sip, "port", json_object_new_int(port));
            json_object_object_add(node_patch, "sip", sip);
            ctx.store->update_node(node_patch);
            json_object_put(node_patch);
            node_patch = json_object_new_object();
            fail_code = "SIP_UNREACHABLE";
            fail_reason = "SIP 服务不可达";
          } else {
            json_object* sip = json_object_new_object();
            json_object_object_add(sip, "enabled", json_object_new_boolean(1));
            json_object_object_add(sip, "state",
                                   json_object_new_string("connected"));
            json_object_object_add(sip, "mode",
                                   json_object_new_string("connectivity-probe"));
            json_object_object_add(sip, "server",
                                   json_object_new_string(srv.c_str()));
            json_object_object_add(sip, "port", json_object_new_int(port));
            json_object_object_add(node_patch, "sip", sip);
            ctx.store->update_node(node_patch);
            json_object_put(node_patch);
            node_patch = json_object_new_object();
            ctx.store->mark_applied(scope, 0);
          }
        }
      }
    }
    if (!fail_code.empty()) {
      ctx.store->mark_failed(scope, fail_code.c_str(), fail_reason.c_str());
      if (scope != "role")
        agent_audit(scope, "save", "failed", fail_code + ": " + fail_reason);
    } else if (!forward_media) {
      if (scope != "role")
        agent_audit(scope, "save", "applied",
                    scope_ok_msg.empty() ? "saved" : scope_ok_msg);
    }
    if (forward_media && fwd) {
      if (scope == "output") {
        int want_rev = agent_config_desired_revision(ctx, scope, node_id);
        json_object* fwd_bg = json_object_get(fwd);
        AgentContext* ctxp = &ctx;
        std::string sc = scope;
        std::string nid = node_id;
        std::thread([ctxp, fwd_bg, sc, nid, want_rev]() {
          std::string fr = agent_media_cmd(*ctxp, "configure", fwd_bg);
          json_object* fo = json_tokener_parse(fr.c_str());
          bool ok = false;
          std::string code, msg;
          if (fo) {
            json_object* okv = nullptr;
            json_object* codev = nullptr;
            json_object* msgv = nullptr;
            if (json_object_object_get_ex(fo, "ok", &okv) && okv)
              ok = json_object_get_boolean(okv) != 0;
            if (json_object_object_get_ex(fo, "code", &codev) && codev)
              code = json_object_get_string(codev);
            if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
              msg = json_object_get_string(msgv);
            json_object_put(fo);
          } else {
            code = "MEDIA_CONFIGURE_UNPARSABLE";
            msg = "media engine response unparsable";
          }
          agent_apply_media_config_outcome(*ctxp, sc, nid, want_rev, ok, code,
                                          msg);
          json_object_put(fwd_bg);
        }).detach();
        json_object_put(fwd);
      } else {
        std::string fr = agent_media_cmd(ctx, "configure", fwd);
        json_object* fo = json_tokener_parse(fr.c_str());
        if (fo) {
          json_object* okv = nullptr;
          bool ok = false;
          std::string code, msg;
          if (json_object_object_get_ex(fo, "ok", &okv) && okv)
            ok = json_object_get_boolean(okv) != 0;
          json_object* codev = nullptr;
          if (json_object_object_get_ex(fo, "code", &codev) && codev)
            code = json_object_get_string(codev);
          json_object* msgv = nullptr;
          if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
            msg = json_object_get_string(msgv);
          int want_rev = agent_config_desired_revision(ctx, scope, node_id);
          agent_apply_media_config_outcome(ctx, scope, node_id, want_rev, ok,
                                           code, msg);
          json_object_put(fo);
        } else {
          int want_rev = agent_config_desired_revision(ctx, scope, node_id);
          agent_apply_media_config_outcome(ctx, scope, node_id, want_rev, false,
                                           "MEDIA_CONFIGURE_UNPARSABLE",
                                           "media engine response unparsable");
        }
        json_object_put(fwd);
      }
    } else if (fwd) {
      json_object_put(fwd);
    }
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "configStates", ctx.store->all_states());
    json_object_object_add(patch, "configRevision",
                           ctx.store->config_revision());
    ctx.store->update_node(patch);
    json_object_put(patch);
    json_object_put(node_patch);
    json_object* cfg = ctx.store->get_config(scope, node_id);
    if (body) json_object_put(body);
    const char* ok_msg = (scope == "output" && forward_media)
                             ? "配置已保存，等待媒体生效"
                             : "配置已提交";
    const char* ok_text = scope_ok_msg.empty() ? ok_msg : scope_ok_msg.c_str();
    agent_send_json(res,
                    agent_envelope_ok(cfg, fail_code.empty() ? ok_text : "配置失败"));
    return true;
  }
  if (req.method == "POST" && action == "retry") {
    ctx.store->retry(scope);
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "configStates", ctx.store->all_states());
    ctx.store->update_node(patch);
    json_object_put(patch);
    json_object* cfg = ctx.store->get_config(scope, node_id);
    agent_send_json(res, agent_envelope_ok(cfg, "配置已重新提交，等待应用"));
    return true;
  }
  agent_send_json(res,
                  agent_envelope_fail("NOT_FOUND", 404, "未定义的节点配置操作"));
  return true;
}
