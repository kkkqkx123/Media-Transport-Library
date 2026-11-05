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
     * `Authorization: Bearer <bearer>` header is attached for the
     * platform-agent control loop. Returns the status of the last attempt. */
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

/* 持久化 WS 客户端：由代理主动拨出，与平台同端口的流式路径建链。
 * 握手为标准 RFC6455（掩码发送、不掩码接收），鉴权不在握手头完成，
 * 而由首条 hello 消息携带 token 经共享契约校验。 */
class WsClient {
public:
    WsClient();
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // url 形如 http://host:port/api/v1/channel/ws；bearer 为裸 token。
    bool connect_ws(const std::string& url, const std::string& bearer = "",
                    int timeout_ms = 5000);
    bool send_text(const std::string& s);
    // 读取一个文本消息；自动回 pong（掩码）、忽略 pong、遇 close 返回 false。
    bool recv_text(std::string& out, int timeout_ms);
    bool send_ping(const std::string& p = "");
    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
};
