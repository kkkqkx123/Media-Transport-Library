#include "agent_context.h"

#include <chrono>
#include <cstdlib>
#include <ctime>

#include "dvs_contract.h"

std::string agent_get_str(json_object* o, const char* key) {
  json_object* v = nullptr;
  if (o && json_object_object_get_ex(o, key, &v) && v)
    return json_object_get_string(v);
  return "";
}

std::string agent_iso_now() {
  char buf[64];
  time_t t = time(nullptr);
  struct tm tmv{};
  gmtime_r(&t, &tmv);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return std::string(buf);
}

long long agent_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string agent_env_or(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return v ? std::string(v) : std::string(d ? d : "");
}

json_object* agent_envelope_ok(json_object* data, const char* msg) {
  return dvs::contract::make_ok(data, msg ? msg : "", "a");
}

json_object* agent_envelope_fail(const char* code, int status,
                                 const char* msg) {
  return dvs::contract::make_fail(code, status, msg ? msg : code, "a");
}

void agent_send_json(HttpResponse& res, json_object* o) {
  const char* s = json_object_to_json_string(o);
  res.body = s ? s : "{}";
  res.content_type = "application/json; charset=utf-8";
  json_object_put(o);
}
