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
 * No logic here, only wiring. Shared JSON/HTTP helpers live in
 * dvs_common/dvs_http_json.h. */
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
