#pragma once
#include <mutex>
#include <string>
#include <json-c/json.h>

/*
 * Client for the dvs_media IPC socket (Unix domain, JSON line protocol).
 *
 * - Persistent connection with connect-on-demand and one reconnect retry.
 * - Every request carries a generated `messageId`; the response echoes it.
 * - poll() based timeout; engine failures surface as structured error
 *   envelopes (MEDIA_ENGINE_UNREACHABLE / MEDIA_ENGINE_TIMEOUT) instead of
 *   empty strings.
 * - Event lines (media.prepare.result, media.layout_applied,
 *   media.state_changed) are cached in a fixed-size ring for later draining.
 *   (Fixed ring, not std::deque: the cross sysroot libstdc++ is older than
 *   the build toolchain.)
 */
class MediaIpcClient {
public:
    explicit MediaIpcClient(const std::string& sock_path);
    ~MediaIpcClient();

    /* Synchronous request; payload may be null. Returns the response JSON
     * line, or a synthesized error envelope on transport failure. */
    std::string request(const std::string& type, json_object* payload,
                        int timeout_ms = 2000);

    /* Convenience wrapper; returns response JSON (never empty). */
    std::string query_state();

    /* Bundle handshake; returns the engine hello envelope, or a structured
     * error envelope when the engine is unreachable. Never throws. */
    std::string hello();

    /* Take all cached events (returns a JSON array object, ownership passes). */
    json_object* drain_events();

    size_t pending_events() const;

private:
    bool ensure_conn_locked();
    void close_locked();

    std::string sock_path_;
    int fd_ = -1;
    long seq_ = 0;

    /* guarded by mtx_ */
    mutable std::mutex mtx_;
    std::string pending_line_;      /* partial line from the socket */
    static const size_t kEventRing = 64;
    std::string events_[kEventRing];
    size_t ev_head_ = 0;
    size_t ev_count_ = 0;
};
