#include "http_client.h"
#include "http_server.h"
#include "sha256.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <string>

static bool parse_url(const std::string& url, std::string& host, int& port,
                      std::string& path) {
    if (url.rfind("http://", 0) != 0) return false;
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        host = authority;
        port = 80;
    } else {
        host = authority.substr(0, colon);
        port = std::stoi(authority.substr(colon + 1));
    }
    return !host.empty();
}

int HttpClient::request(const std::string& method, const std::string& url,
                        const std::string& body, std::string& resp,
                        int timeout_ms, const std::string& auth) {
    resp.clear();
    if (url.empty()) return -1;

    std::string host;
    int port = 80;
    std::string path;
    if (!parse_url(url, host, port, path)) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == (uint32_t)-1) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        /* connect may be in progress; wait briefly on writable */
        pfd.events = POLLOUT;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0 || !(pfd.revents & POLLOUT)) {
            close(fd);
            return -1;
        }
    }

    std::string req = method + " " + path + " HTTP/1.0\r\n" +
                      "Host: " + host + "\r\n" +
                      "Content-Type: application/json\r\n" +
                      (auth.empty() ? std::string()
                                    : "Authorization: Bearer " + auth + "\r\n") +
                      "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    size_t total = 0;
    while (total < req.size()) {
        ssize_t n = write(fd, req.data() + total, req.size() - total);
        if (n <= 0) { close(fd); return -1; }
        total += (size_t)n;
    }

    std::string raw;
    char buf[4096];
    for (;;) {
        struct pollfd rp = {.fd = fd, .events = POLLIN};
        int pr = poll(&rp, 1, timeout_ms);
        if (pr <= 0) break;
        if (!(rp.revents & POLLIN)) break;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
    }
    close(fd);

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return -1;
    std::string head = raw.substr(0, header_end);
    resp = raw.substr(header_end + 4);

    /* parse status code from "HTTP/1.x NNN ..." */
    size_t sp1 = head.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return -1;
    int status = std::stoi(head.substr(sp1 + 1, sp2 - sp1 - 1));

    /* if server sent a Content-Length, trust it; otherwise chunked/close is
     * fine since we already read everything. */
    return status;
}

int HttpClient::post_json(const std::string& url, const std::string& body,
                          std::string& resp, int timeout_ms) {
    return request("POST", url, body, resp, timeout_ms, "");
}

int HttpClient::post_json(const std::string& url, const std::string& body,
                          std::string& resp, int timeout_ms, int retries,
                          const std::string& bearer) {
    int status = -1;
    int attempts = (retries < 1) ? 1 : retries;
    for (int i = 0; i < attempts; i++) {
        status = request("POST", url, body, resp, timeout_ms, bearer);
        /* success: status in [200,400); transport failure: -1. Retry both,
         * since a dropped connection mid-flight must not lose the op. */
        if (status >= 200 && status < 400) return status;
        if (i + 1 < attempts) {
            int backoff_ms = 200 * (1 << i);
            if (backoff_ms > 2000) backoff_ms = 2000;
            struct timespec ts = {backoff_ms / 1000,
                                  (backoff_ms % 1000) * 1000000L};
            nanosleep(&ts, nullptr);
        }
    }
    return status;
}

int HttpClient::get(const std::string& url, std::string& resp, int timeout_ms) {
    return request("GET", url, "", resp, timeout_ms, "");
}

// ---- WsClient ----

WsClient::WsClient() = default;
WsClient::~WsClient() { close(); }

void WsClient::close() {
    if (fd_ >= 0) {
        int f = fd_;
        fd_ = -1;
        ::shutdown(f, SHUT_RDWR);
        ::close(f);
    }
}

bool WsClient::connect_ws(const std::string& url, const std::string& bearer,
                          int timeout_ms) {
    close();
    std::string host;
    int port = 80;
    std::string path;
    if (!parse_url(url, host, port, path)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == (uint32_t)-1) { ::close(fd); return false; }
    // 非阻塞 connect + poll，弱网下不 hung 住拨号线程。
    (void)0;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) {
        struct pollfd pf{.fd = fd, .events = POLLOUT};
        if (poll(&pf, 1, timeout_ms) <= 0 || !(pf.revents & POLLOUT)) {
            ::close(fd);
            return false;
        }
        int err = 0;
        socklen_t n = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0 || err != 0) {
            ::close(fd);
            return false;
        }
    }
    fcntl(fd, F_SETFL, fl);

    uint8_t rnd[16];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd >= 0) {
        size_t got = 0;
        while (got < sizeof(rnd)) {
            ssize_t n = read(rfd, rnd + got, sizeof(rnd) - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        ::close(rfd);
        if (got != sizeof(rnd)) return false;
    } else {
        for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    }
    std::string key = dvs::base64_encode(rnd, 16);
    std::string hs = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
                     "Sec-WebSocket-Key: " + key + "\r\n" +
                     "Sec-WebSocket-Version: 13\r\n" +
                     (bearer.empty() ? std::string()
                                     : "Authorization: Bearer " + bearer + "\r\n") +
                     "\r\n";
    size_t off = 0;
    while (off < hs.size()) {
        ssize_t w = write(fd, hs.data() + off, hs.size() - off);
        if (w <= 0) { ::close(fd); return false; }
        off += (size_t)w;
    }
    std::string raw;
    char buf[4096];
    for (;;) {
        struct pollfd rp{.fd = fd, .events = POLLIN};
        if (poll(&rp, 1, timeout_ms) <= 0) { ::close(fd); return false; }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) { ::close(fd); return false; }
        raw.append(buf, (size_t)n);
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > 16384) { ::close(fd); return false; }
    }
    if (raw.find(" 101") == std::string::npos) { ::close(fd); return false; }
    // 校验 Accept，避免连到非 WS 服务。
    std::string want = HttpServer::ws_accept_key(key);
    if (raw.find(want) == std::string::npos) { ::close(fd); return false; }
    fd_ = fd;
    return true;
}

bool WsClient::send_text(const std::string& s) {
    if (fd_ < 0) return false;
    return HttpServer::ws_send_text(fd_, s, true);
}

bool WsClient::send_ping(const std::string& p) {
    if (fd_ < 0) return false;
    return HttpServer::ws_send_ping(fd_, p, true);
}

bool WsClient::recv_text(std::string& out, int timeout_ms) {
    out.clear();
    if (fd_ < 0) return false;
    std::string frag;
    int fd = fd_;
    for (;;) {
        uint8_t op = 0;
        std::string pl;
        if (!HttpServer::ws_recv_frame(fd, op, pl, timeout_ms)) return false;
        if (fd_ != fd || fd_ < 0) return false;
        if (op == 0x8) {
            HttpServer::ws_send_close(fd, true);
            return false;
        }
        if (op == 0x9) {
            HttpServer::ws_send_pong(fd, pl, true);
            continue;
        }
        if (op == 0xA) continue;
        if (op == 0x1 || op == 0x0) {
            frag += pl;
            out = frag;
            return true;
        }
        return false;
    }
}
