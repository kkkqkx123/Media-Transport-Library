#pragma once
#include <json-c/json.h>

#include <string>

#include "http_server.h"

class NodeStore;
class MediaIpcClient;
class Maintenance;
class Registration;

/* Shared agent dependencies. main.cpp owns the instance (heap, never freed,
 * like the previous globals); every route/downlink module takes a reference.
 * No logic here, only wiring plus the tiny helpers every module needs. */
struct AgentContext {
  NodeStore* store = nullptr;
  MediaIpcClient* media = nullptr;    // command channel (mutating verbs)
  MediaIpcClient* media_q = nullptr;  // query channel (reads never block cmds)
  Maintenance* maint = nullptr;
  Registration* reg = nullptr;
  std::string web_root;
  std::string api_token;     // empty = no platform bearer check
  std::string platform_url;  // empty = local-autonomous, nothing to uplink
};

std::string agent_get_str(json_object* o, const char* key);
std::string agent_iso_now();
long long agent_now_ms();
std::string agent_env_or(const char* k, const char* d);
json_object* agent_envelope_ok(json_object* data, const char* msg);
json_object* agent_envelope_fail(const char* code, int status, const char* msg);
void agent_send_json(HttpResponse& res, json_object* o);
