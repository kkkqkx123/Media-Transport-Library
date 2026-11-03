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

    /* GET variant, same contract. */
    int get(const std::string& url, std::string& resp, int timeout_ms = 3000);

private:
    int request(const std::string& method, const std::string& url,
                const std::string& body, std::string& resp, int timeout_ms);
};
