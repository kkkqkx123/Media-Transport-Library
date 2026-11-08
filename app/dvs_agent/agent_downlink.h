#pragma once

struct AgentContext;

/* WS downlink dispatch: platform layout.prepare / config.update /
 * upgrade.task arriving over the long-lived channel run with short-link
 * semantics. Layout shares the staging helper with the HTTP path so a
 * whole-order rejection skips the reporter in both; config mirrors the
 * local scope rule; upgrade runs detached and uplinks progress. */
void agent_install_downlink(AgentContext& ctx);
