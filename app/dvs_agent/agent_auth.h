#pragma once
#include <map>
#include <string>

#include "http_server.h"

struct AgentContext;

/* Browser sessions + per-source login rate limiting + the shared auth gate.
 * Owns: LoginLimiter, SessionStore, session cookie parsing, bearer check,
 * DVS_REQUIRE_AUTH opt-in, and the three auth routes (login/password/logout).
 * Nothing else in the agent mutates session state. */
std::string agent_session_from_cookie(
    const std::map<std::string, std::string>& headers);

/* Platform bearer check against ctx.api_token (empty token = no check). */
bool agent_bearer_ok(AgentContext& ctx, const HttpRequest& req);

/* Global cookie/bearer gate for all non-login routes. True = may proceed. */
bool agent_authorized(AgentContext& ctx, const HttpRequest& req);

/* Handles POST /api/v1/node/login|password and POST /api/v1/node/logout.
 * Returns true when the path was claimed (handled or rejected). */
bool agent_handle_auth(AgentContext& ctx, const HttpRequest& req,
                       HttpResponse& res);
