#pragma once
#include <json-c/json.h>

#include <cstdint>
#include <string>

#include "http_server.h"

// Shared HTTP/JSON plumbing for dvs_agent and dvs_platform. Single source for
// the tiny helpers both services need: JSON field reads, escaping, clocks,
// envelopes, response writing and the bearer gate. Origin ("a"/"p") selects
// the request-id namespace so the two services stay distinguishable.
namespace dvs {

std::string json_get_str(json_object* o, const char* key);
std::string json_escape(const std::string& s);
int json_rect_int(json_object* o, const char* k);
std::string iso_now();
int64_t wall_sec_now();
uint64_t wall_ns_now();
long long steady_ms_now();
std::string env_or(const char* k, const char* d);
json_object* envelope_ok(json_object* data, const std::string& msg,
                         const char* origin = "p");
json_object* envelope_fail(const char* code, int status,
                           const std::string& msg, const char* origin = "p");
void send_json(HttpResponse& res, json_object* o);
bool http_bearer_ok(const HttpRequest& req, const std::string& token);
bool require_bearer(const HttpRequest& req, HttpResponse& res,
                    const std::string& token, const char* origin = "p");
std::string idempotency_key(const HttpRequest& req);

}  // namespace dvs
