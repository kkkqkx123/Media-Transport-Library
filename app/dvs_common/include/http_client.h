#pragma once
#include <string>

/* Minimal blocking HTTP/1.0 client over POSIX sockets (no libcurl). Used by the
 * registration/heartbeat loop to talk to the platform. When the URL is empty or
 * cannot be reached the caller gets a negative status and must degrade
 * gracefully (never throw, never block media output). */
class HttpClient {
public:
    /* Returns HTTP status code (>0) on success, or -1 on transport/parse
     * failure. Response body is stored in `resp` when status >= 0. */
    int post_json(const std::string& url, const std::string& body,
                  std::string& resp, int timeout_ms = 3000);

    /* Retrying variant: attempts up to `retries` times with exponential
     * backoff (200ms * 2^k, capped at 2s). When `bearer` is non-empty an
     * `Authorization: Bearer <bearer>` header is attached (used for the
     * platform<->agent control loop, see DVS_INTER_NODE_COMMS_FIX.md F3/F4).
     * Returns the status of the last attempt. */
    int post_json(const std::string& url, const std::string& body,
                  std::string& resp, int timeout_ms, int retries,
                  const std::string& bearer);

    /* GET variant, same contract. */
    int get(const std::string& url, std::string& resp, int timeout_ms = 3000);

private:
    int request(const std::string& method, const std::string& url,
                const std::string& body, std::string& resp, int timeout_ms,
                const std::string& auth = "");
};
