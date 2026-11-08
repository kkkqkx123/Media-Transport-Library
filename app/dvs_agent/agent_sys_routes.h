#pragma once
#include <string>

#include "http_server.h"

struct AgentContext;

/* Node-level read + maintenance surface: info/role/capability/capacity/
 * version snapshots, register trigger, firmware upload staging, reboot/
 * factory-reset/upgrade, and the audit-events page. No scope validation
 * and no media mutation here; those live in agent_config_routes and
 * agent_media_routes. */
bool agent_handle_sys(AgentContext& ctx, const HttpRequest& req,
                      HttpResponse& res);
