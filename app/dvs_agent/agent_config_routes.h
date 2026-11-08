#pragma once
#include <string>

#include "http_server.h"

struct AgentContext;

/* Desired-state config surface: six scopes (role/input/output/network/
 * registration/sip) plus the platform-owned config/sync subset and the
 * retry verb. Owns all scope validation (ports, modes, IPs, whitelist,
 * TX params, SIP reachability) and the store + media-engine forwarding
 * that decides applied/pending/failed. Nothing else validates scopes. */
bool agent_handle_config(AgentContext& ctx, const HttpRequest& req,
                         HttpResponse& res);
