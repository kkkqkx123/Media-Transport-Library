#include "agent_auth.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "agent_context.h"
#include "dvs_contract.h"
#include "node_store.h"

namespace {
/* Per-source login rate limiter: 5 attempts / 60s window, then locked out. */
struct LoginLimiter {
  std::mutex mu;
  std::map<std::string, std::pair<int, time_t>> attempts;
  bool allow(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    time_t now = time(nullptr);
    auto& e = attempts[key];
    if (now - e.second > 60) {
      e.first = 0;
      e.second = now;
    }
    if (e.first >= 5) return false;
    e.first++;
    return true;
  }
  void clear(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    attempts.erase(key);
  }
};

/* Issued browser sessions with expiry. Random hex tokens replace the fixed
 * prototype value; logout invalidates. All access under the internal mutex. */
struct SessionStore {
  std::mutex mu;
  std::map<std::string, time_t> exp_;
  std::string issue() {
    uint8_t rnd[16] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      size_t got = 0;
      while (got < sizeof(rnd)) {
        ssize_t n = read(fd, rnd + got, sizeof(rnd) - got);
        if (n <= 0) break;
        got += (size_t)n;
      }
      close(fd);
    }
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", rnd[i]);
    std::string tok(hex, 32);
    std::lock_guard<std::mutex> lk(mu);
    exp_[tok] = time(nullptr) + 3600;
    return tok;
  }
  bool valid(const std::string& tok) {
    if (tok.empty()) return false;
    std::lock_guard<std::mutex> lk(mu);
    auto it = exp_.find(tok);
    if (it == exp_.end()) return false;
    if (time(nullptr) > it->second) {
      exp_.erase(it);
      return false;
    }
    return true;
  }
  void invalidate(const std::string& tok) {
    std::lock_guard<std::mutex> lk(mu);
    exp_.erase(tok);
  }
};

LoginLimiter g_limiter;
SessionStore g_sessions;
}  // namespace

std::string agent_session_from_cookie(
    const std::map<std::string, std::string>& headers) {
  auto it = headers.find("Cookie");
  if (it == headers.end()) return "";
  const std::string& c = it->second;
  size_t p = c.find("session=");
  if (p == std::string::npos) return "";
  size_t s = p + 8;
  size_t e = c.find(';', s);
  std::string v =
      c.substr(s, e == std::string::npos ? std::string::npos : e - s);
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
  while (!v.empty() &&
         (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
    v.pop_back();
  return v;
}

bool agent_bearer_ok(AgentContext& ctx, const HttpRequest& req) {
  auto it = req.headers.find("Authorization");
  std::string provided = (it == req.headers.end())
                             ? ""
                             : dvs::contract::strip_bearer(it->second);
  if (provided.empty())
    provided = dvs::contract::extract_bearer(
        const_cast<std::map<std::string, std::string>&>(req.headers));
  return dvs::contract::bearer_ok(provided, ctx.api_token);
}

bool agent_authorized(AgentContext& ctx, const HttpRequest& req) {
  // Strict auth is opt-in via DVS_REQUIRE_AUTH=1 (default off) so the
  // node-admin prototype keeps working without a login wall. When enabled,
  // every /api request except login must carry a valid session cookie.
  // Browser sessions use the cookie. Internal platform calls use a bearer
  // token so barrier delivery cannot be forged on a shared LAN.
  static bool require_auth = (std::getenv("DVS_REQUIRE_AUTH") != nullptr);
  if (!require_auth) return true;
  if (req.path.rfind("/api/", 0) != 0) return true;
  if (req.path == "/api/v1/node/login") return true;
  bool authed =
      g_sessions.valid(agent_session_from_cookie(req.headers));
  if (authed) return true;
  return agent_bearer_ok(ctx, req);
}

bool agent_handle_auth(AgentContext& ctx, const HttpRequest& req,
                       HttpResponse& res) {
  if (req.method == "POST" && req.path == "/api/v1/node/login") {
    json_object* body = json_tokener_parse(req.body.c_str());
    std::string u = agent_get_str(body, "username");
    std::string p = agent_get_str(body, "password");
    if (body) json_object_put(body);
    if (!g_limiter.allow(u)) {
      agent_send_json(
          res, agent_envelope_fail("TOO_MANY_REQUESTS", 429,
                                   "登录尝试过多,请稍后再试"));
      return true;
    }
    if (ctx.store->verify_password(u, p)) {
      g_limiter.clear(u);
      std::string tok = g_sessions.issue();
      json_object* d = json_object_new_object();
      json_object_object_add(d, "session",
                             json_object_new_string(tok.c_str()));
      json_object_object_add(d, "expiresIn", json_object_new_int(3600));
      res.headers["Set-Cookie"] = "session=" + tok + "; Path=/; HttpOnly";
      json_object* pc = json_object_new_object();
      json_object_object_add(
          pc, "passwordChangeRequired",
          json_object_new_boolean(ctx.store->password_change_required()
                                      ? 1
                                      : 0));
      json_object_object_add(d, "requirement", pc);
      agent_send_json(res, agent_envelope_ok(d, "登录成功"));
    } else {
      agent_send_json(res, agent_envelope_fail("UNAUTHORIZED", 401,
                                              "账号或密码错误"));
    }
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/password") {
    json_object* body = json_tokener_parse(req.body.c_str());
    std::string user = agent_get_str(body, "username");
    if (user.empty()) user = "admin";  // default account
    std::string oldp = agent_get_str(body, "oldPassword");
    std::string newp = agent_get_str(body, "newPassword");
    if (body) json_object_put(body);
    if (!ctx.store->verify_password(user, oldp)) {
      agent_send_json(
          res, agent_envelope_fail("UNAUTHORIZED", 401, "原密码错误"));
      return true;
    }
    if (newp.size() < 6) {
      agent_send_json(res, agent_envelope_fail("INVALID_ARGUMENT", 400,
                                              "新密码至少 6 位"));
      return true;
    }
    ctx.store->set_password(newp);
    agent_send_json(res,
                    agent_envelope_ok(json_object_new_object(), "密码已修改"));
    return true;
  }
  if (req.method == "POST" && req.path == "/api/v1/node/logout") {
    g_sessions.invalidate(agent_session_from_cookie(req.headers));
    res.headers["Set-Cookie"] = "session=; Path=/; Max-Age=0";
    agent_send_json(res,
                    agent_envelope_ok(json_object_new_object(), "已退出"));
    return true;
  }
  return false;
}
