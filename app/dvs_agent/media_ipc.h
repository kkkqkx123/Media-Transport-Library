#pragma once
#include <mutex>
#include <string>
#include <json-c/json.h>

/*
 * Client for the dvs_media IPC socket (Unix domain, JSON line protocol).
 *
 * - Persistent connection with connect-on-demand and one reconnect retry.
 * - Every request carries a generated `requestId`; the response echoes it.
 * - poll() based timeout; engine failures surface as structured error
 *   envelopes (MEDIA_ENGINE_UNREACHABLE / MEDIA_ENGINE_TIMEOUT) instead of
 *   empty strings.
 * - The client counts consecutive transport failures and remembers the last
 *   success time so the agent can mark the engine link without extra probes.
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

    /* Parse a hello envelope into answered/match verdicts against the
     * bundle contract. Pure input/output, no socket, so the version gate
     * matrix stays unit-testable without an engine. */
    static bool parse_hello(const std::string& hello, bool& answered, bool& match,
                            std::string& eng_bundle, int& eng_ipc);

    /* Take all cached events (returns a JSON array object, ownership passes). */
    json_object* drain_events();

    /* Blocking wait for the async result of one prepare operation. Drains
     * the socket without holding the client lock while polling so other
     * verbs on the same client keep flowing. Consumes the matching event;
     * unrelated events stay cached. Lanes carry the engine channel summary
     * when the result provides one, otherwise "[]". */
    bool wait_prepare_result(const std::string& opid, int timeout_ms,
                             std::string& status_out, std::string& lanes_json_out);

    size_t pending_events() const;
    bool connected() const;
    /* Consecutive transport failures since the last good response. */
    int fail_streak() const;
    /* Monotonic milliseconds of the last good response (0 when never). */
    long long last_ok_ms() const;
    long long last_err_ms() const;
    std::string last_code() const;

private:
    bool ensure_conn_locked();
    void close_locked();
    void drain_socket_locked();
    static long long now_ms();
    void note_ok_locked();
    void note_err_locked(const char* code);
    /* Scan the ring for the async result of opid; on hit consume it and
     * fill status/lanes. Caller holds mtx_. */
    bool scan_result_locked(const std::string& opid, std::string& status_out,
                            std::string& lanes_json_out);

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
    int fail_streak_ = 0;
    long long ok_ms_ = 0;
    long long err_ms_ = 0;
    std::string last_code_;
};

/* Translate a platform sub-layout (flat windows with windowId/sig/dst/src/z/
 * muted/partial plus full-frame dims, an endpoint table, audio owner and
 * scene volume) into the engine "streams" prepare payload. Returns false on a
 * whole-order rejection (signal not in the endpoint table or over the lane
 * cap); on success *prep_out holds the payload with lanes assigned by z
 * ascending, ties in array order. Pure JSON logic, no socket. */
bool translate_platform_layout(json_object* body, json_object** prep_out,
                               std::string& err_out);
