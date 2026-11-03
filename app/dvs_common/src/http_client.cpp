#include "http_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
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
                        int timeout_ms) {
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
    return request("POST", url, body, resp, timeout_ms);
}

int HttpClient::get(const std::string& url, std::string& resp, int timeout_ms) {
    return request("GET", url, "", resp, timeout_ms);
}
