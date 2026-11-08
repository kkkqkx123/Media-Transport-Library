#pragma once
#include <json-c/json.h>

#include <cstdint>
#include <string>

#include "http_server.h"

// Shared HTTP/JSON plumbing for the platform service: untrusted-string
// escaping, JSON field reads, clock helpers, idempotency keys and the
// uniform response envelope. Used by main.cpp and docs_routes.cpp.
std::string json_escape(const std::string& s);
std::string body_str(json_object* o, const char* key);
std::string now_iso();
int64_t now_sec();
uint64_t now_ns();
std::string idempotency_key(const HttpRequest& req);
json_object* ok_envelope(json_object* data, const std::string& msg);
json_object* fail_envelope(const char* code, int status, const std::string& msg);
void send_json(HttpResponse& res, json_object* o);
