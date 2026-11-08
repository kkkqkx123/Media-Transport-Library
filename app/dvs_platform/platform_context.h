#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "channels.h"
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
    std::queue<UpgradeJob> up_q;
    // Empty means no check so a local prototype keeps working; when set,
    // agent-to-platform calls must carry it.
    std::string api_token;
    std::string web_root;
};
