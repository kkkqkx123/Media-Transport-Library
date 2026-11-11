#pragma once
#include <condition_variable>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "channels.h"
#include "dvs_http_json.h"
#include "http_server.h"
#include "ops.h"

class PlatformStore;

// Shared service state. main.cpp owns the single instance for the process
// lifetime; every other module takes it by reference instead of touching
// file-static globals so transports, barrier and routes stay testable.
struct UpgradeJob {
    std::string task_id;
    std::vector<std::string> nodes;
    std::string filename;
    std::string sha256;
};

struct PlatformContext {
    PlatformStore* store = nullptr;
    OpCoordinator ops;
    std::mutex ops_mu;
    ChannelRegistry channels;
    std::mutex up_mu;
    std::condition_variable up_cv;
    // Upgrade backlog on std::list, not the default deque: the cross
    // toolchain (GCC 13) emits std::__throw_bad_array_new_length for deque
    // growth, which the board sysroot libstdc++ (6.0.28) does not provide.
    // vector members must likewise be moved, never copied, for the same
    // reason (see route_upgrade / start_upgrade_worker).
    std::queue<UpgradeJob, std::list<UpgradeJob>> up_q;
    // Bearer token for agent-to-platform calls. Empty means misconfigured:
    // main refuses to start and the HTTP/WS gates reject every mutating call.
    std::string api_token;
    std::string web_root;
    // Serializes doc allocation (multicast pool + id generation) so two
    // concurrent allocates never hand out the same address/id.
    std::mutex alloc_mu;
};

// Mutating-call gate: empty token never passes, otherwise delegate to the
// shared bearer check. Read-only GET bypasses this at the server layer.
inline bool platform_mutation_allowed(const PlatformContext& ctx,
                                      const HttpRequest& req) {
    if (ctx.api_token.empty()) return false;
    return dvs::http_bearer_ok(req, ctx.api_token);
}
