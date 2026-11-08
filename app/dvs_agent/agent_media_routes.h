#pragma once
#include <string>

#include "http_server.h"

struct AgentContext;

/* Engine-facing HTTP surface: query_state projections, event drain, manual
 * two-phase verbs, platform layout staging + barrier reporting, TX/souce
 * snapshots and the ops log bundle. Mutating verbs enforce the platform
 * bearer token; reads never do. The WS downlink reuses the same staging
 * helper in agent_media instead of duplicating it here. */
bool agent_handle_media(AgentContext& ctx, const HttpRequest& req,
                        HttpResponse& res);
