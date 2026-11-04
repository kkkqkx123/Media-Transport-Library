#pragma once
#include <atomic>
#include <json-c/json.h>
#include <string>
#include <thread>

class NodeStore;
class MediaIpcClient;

/* Registration + heartbeat against the platform over HTTP polling.
 * - Empty platform URL => local autonomous mode: node stays online and
 *   functional, no network egress.
 * - Non-empty URL => register once (with retry/backoff), then heartbeat every
 *   `interval_ms` (default 3000). Three consecutive failures => offline.
 * Transport failures never throw and never disturb the media pipeline. */
class Registration {
public:
    Registration(NodeStore* store, MediaIpcClient* media,
                 const std::string& platform_url, int interval_ms = 3000,
                 const std::string& agent_base = "");
    ~Registration();

    void start();
    void stop();

    bool online() const { return online_; }
    std::string register_state() const { return register_state_; }
    bool trigger_once();
    void trigger_async();

private:
    void loop();
    bool register_once();
    bool heartbeat_once();
    /* Build the heartbeat payload, pulling media stats/ptp from the engine. */
    json_object* build_heartbeat();
    json_object* build_register();

    NodeStore* store_;
    MediaIpcClient* media_;
    std::string platform_url_;
    std::string agent_base_;  // externally reachable base URL of this agent
    int interval_ms_;

    std::atomic<bool> running_{false};
    std::atomic<bool> online_{true};
    std::string register_state_;  // unregistered | registering | registered
    std::thread thread_;
    int fail_streak_ = 0;

    time_t start_time_;
};
