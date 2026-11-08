#pragma once
#include <json-c/json.h>

#include <set>
#include <string>

struct PlatformContext;

// Record one node reply for a barrier op and drive commit/rollback.
// Validates lane readiness, frame consistency and the node clock, audits the
// outcome, and stamps the wall doc on commit. Returns the op state.
std::string handle_barrier_ack(PlatformContext& ctx, const std::string& opid,
                              const std::string& node, std::string st,
                              json_object* lanes,
                              std::set<std::string>& missing_out,
                              json_object* ack);
