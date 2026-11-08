#pragma once
#include <json-c/json.h>

#include <cstdint>
#include <string>

struct AgentContext;

/* Media-engine liaison: everything the agent knows about dvs_media lives
 * here and only here. Owns the continuous bundle version gate, the
 * command/query channel split, config-outcome revision guard, engine-link
 * derivation, role-pending mirror, playback projection, capability merge +
 * hotplug revalidation, and the barrier reporter (prepare-result wait,
 * local 2PC close, uplink). HTTP and WS callers share these; they never
 * touch MediaIpcClient directly except for the abort escape hatch. */

// Version gate: seed from a hello envelope (startup + /version refresh).
void agent_media_note_hello(AgentContext& ctx, const std::string& hello);
// True when layout-affecting verbs may flow; reason carries VERSION_MISMATCH.
bool agent_media_layout_allowed(AgentContext& ctx, std::string& reason_out);
std::string agent_gated_envelope(const std::string& reason);

// Channel split: mutating verbs (gated) vs reads (never gated).
std::string agent_media_cmd(AgentContext& ctx, const std::string& type,
                            json_object* payload, int timeout_ms = 2000);
std::string agent_media_query(AgentContext& ctx, const std::string& type,
                              json_object* payload, int timeout_ms = 2000);

// Config outcome guard against stale engine answers.
int agent_config_desired_revision(AgentContext& ctx, const std::string& scope,
                                  const std::string& node_id);
void agent_apply_media_config_outcome(AgentContext& ctx,
                                      const std::string& scope,
                                      const std::string& node_id, int want_rev,
                                      bool ok, const std::string& code,
                                      const std::string& msg);

// Link + caps freshness for /info /version /logs.
std::string agent_engine_link_state(AgentContext& ctx);
long long agent_cap_age_ms();
bool agent_cap_stale();

// Capability merge (startup + on-demand) and background hotplug poll.
void agent_refresh_capability(AgentContext& ctx);
void agent_note_cap_ok();
void agent_hotplug_poll_once(AgentContext& ctx);
void agent_hotplug_loop(AgentContext& ctx);
void agent_init_cap_sig(AgentContext& ctx);
std::string agent_caps_ports_signature(json_object* caps);

// Role intent mirror (local save + platform sync share one rule).
std::string agent_sync_pending_role(AgentContext& ctx, const std::string& want);
// Playback projection folded from query_state data.
void agent_project_playback(AgentContext& ctx, json_object* data);

// Shared layout staging: translate platform sub-layout to engine streams
// and issue prepare. Returns the engine response; out params feed the
// reporter. False = whole-order rejection, nothing staged, no reporter.
bool agent_stage_layout(AgentContext& ctx, json_object* body,
                        std::string& resp_out, std::string& opid_out,
                        int& rev_out, int64_t& apply_at_ns_out,
                        std::string& err_out);
// Detached barrier reporter: wait prepare-result, close local 2PC, uplink.
void agent_spawn_reporter(AgentContext& ctx, const std::string& opid, int rev,
                          int64_t apply_at_ns, const std::string& node_id);
