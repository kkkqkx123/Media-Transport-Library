#pragma once
#include <json-c/json.h>

#include <set>
#include <string>

struct PlatformContext;

// Node document projection: how register/heartbeat (short or WS) merge into
// the stored node doc, plus the wall/binding helpers the admission paths
// share. Pure store logic, no sockets here.
std::set<std::string> derive_wall_members(PlatformContext& ctx,
                                          const std::string& wall_id,
                                          std::set<std::string>* pending_out);
void mark_wall_committed(PlatformContext& ctx, const std::string& wall_id,
                         const std::string& scene_id, int revision,
                         const std::string& hash);
void store_role_intent(json_object* cur, json_object* data);
void promote_pending_bindings(PlatformContext& ctx, const std::string& node_id);
// Display projection shared by the HTTP and WS chains: register fills the
// identity fields once, every heartbeat refreshes the volatile ones.
void project_hello_fields(json_object* doc);
void project_heartbeat_fields(json_object* doc, json_object* data);
// Readiness for wall membership: reachable base URL plus usable geometry.
// Unready docs are rejected at register and excluded from member derivation.
bool node_doc_ready(json_object* doc);
// Honest heartbeat ack for one node: applied/pending revisions from the
// bound wall and open barriers. Caller owns the returned object.
json_object* heartbeat_ack_for_node(PlatformContext& ctx,
                                    const std::string& node_id);
void store_ptp_playback(json_object* cur, json_object* data);
void apply_hello_doc(PlatformContext& ctx, json_object* data);
void apply_heartbeat_doc(PlatformContext& ctx, const std::string& id,
                         json_object* data);
