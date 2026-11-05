#include "http_server.h"
#include "sha256.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <random>

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i=0;i<s.size();++i){
        if (s[i]=='%') { if (i+2<s.size()) { int v; sscanf(s.c_str()+i+1,"%2x",&v); out.push_back((char)v); i+=2; } }
        else if (s[i]=='+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

static std::string lower_str(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string header_get(const std::map<std::string,std::string>& hs,
                              const std::string& key) {
    std::string lk = lower_str(key);
    for (auto& kv : hs)
        if (lower_str(kv.first) == lk) return kv.second;
    return "";
}

static bool send_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

static bool recv_all(int fd, char* p, size_t n, int timeout_ms) {
    size_t off = 0;
    while (off < n) {
        struct pollfd pf{.fd = fd, .events = POLLIN};
        int pr = poll(&pf, 1, timeout_ms);
        if (pr <= 0) return false;
        if (!(pf.revents & POLLIN)) return false;
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

HttpServer::HttpServer(int port, Handler h) : port_(port), handler_(h) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() { running_=true; thread_=std::thread(&HttpServer::loop,this); }

void HttpServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        // 唤醒阻塞的 accept：先 shutdown 再 close。
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) {
        // 若 stop 在 loop 线程自身被调用则不可 join。
        if (std::this_thread::get_id() != thread_.get_id()) thread_.join();
        else thread_.detach();
    }
}

void HttpServer::set_ws_handler(const std::string& path, WsHandler h) {
    ws_path_ = path;
    ws_handler_ = h;
}

std::string HttpServer::ws_accept_key(const std::string& client_key) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string src = client_key + kGuid;
    uint8_t h[20];
    dvs::sha1((const uint8_t*)src.data(), src.size(), h);
    return dvs::base64_encode(h, 20);
}

bool HttpServer::ws_send_frame(int fd, uint8_t opcode, const std::string& payload,
                               bool mask) {
    std::string hdr;
    hdr.push_back((char)0x80u);  // FIN + opcode high bit set by caller below
    hdr[0] = (char)(0x80u | (opcode & 0x0Fu));
    size_t n = payload.size();
    uint8_t m = mask ? 0x80u : 0x00u;
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    if (mask) {
        int rfd = open("/dev/urandom", O_RDONLY);
        if (rfd >= 0) {
            ssize_t gr = read(rfd, mask_key, 4);
            (void)gr;
            close(rfd);
        } else {
            uint32_t r = (uint32_t)rand();
            memcpy(mask_key, &r, 4);
        }
    }
    std::string out = hdr;
    if (n < 126) {
        out.push_back((char)(m | (uint8_t)n));
    } else if (n <= 0xFFFF) {
        out.push_back((char)(m | 126));
        out.push_back((char)((n >> 8) & 0xFF));
        out.push_back((char)(n & 0xFF));
    } else {
        out.push_back((char)(m | 127));
        for (int i = 7; i >= 0; i--) out.push_back((char)((n >> (i * 8)) & 0xFF));
    }
    if (mask) out.append((char*)mask_key, 4);
    if (!send_all(fd, out.data(), out.size())) return false;
    if (n == 0) return true;
    if (!mask) return send_all(fd, payload.data(), n);
    std::string masked = payload;
    for (size_t i = 0; i < n; i++) masked[i] ^= mask_key[i % 4];
    return send_all(fd, masked.data(), n);
}

bool HttpServer::ws_send_text(int fd, const std::string& payload, bool mask) {
    return ws_send_frame(fd, 0x1, payload, mask);
}

bool HttpServer::ws_send_ping(int fd, const std::string& payload, bool mask) {
    return ws_send_frame(fd, 0x9, payload, mask);
}

bool HttpServer::ws_send_pong(int fd, const std::string& payload, bool mask) {
    return ws_send_frame(fd, 0xA, payload, mask);
}

bool HttpServer::ws_send_close(int fd, bool mask) {
    return ws_send_frame(fd, 0x8, "", mask);
}

bool HttpServer::ws_recv_frame(int fd, uint8_t& opcode, std::string& payload,
                               int timeout_ms) {
    payload.clear();
    uint8_t h[2];
    if (!recv_all(fd, (char*)h, 2, timeout_ms)) return false;
    bool fin = (h[0] & 0x80u) != 0;
    opcode = h[0] & 0x0Fu;
    bool masked = (h[1] & 0x80u) != 0;
    uint64_t len = h[1] & 0x7Fu;
    if (len == 126) {
        uint8_t e[2];
        if (!recv_all(fd, (char*)e, 2, timeout_ms)) return false;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!recv_all(fd, (char*)e, 8, timeout_ms)) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        if (len > (64u << 20)) return false;  // 64MB 上限，防内存耗尽
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked && !recv_all(fd, (char*)mk, 4, timeout_ms)) return false;
    payload.resize((size_t)len);
    if (len && !recv_all(fd, payload.data(), (size_t)len, timeout_ms)) return false;
    if (masked)
        for (uint64_t i = 0; i < len; i++) payload[(size_t)i] ^= mk[i % 4];
    (void)fin;  // 分片组装由 ws_recv_text 完成
    return true;
}

bool HttpServer::ws_recv_text(int fd, std::string& out, int timeout_ms,
                              bool mask_pong) {
    out.clear();
    std::string frag;
    for (;;) {
        uint8_t op = 0;
        std::string pl;
        if (!ws_recv_frame(fd, op, pl, timeout_ms)) return false;
        if (op == 0x8) {  // close
            ws_send_frame(fd, 0x8, "", mask_pong);
            return false;
        }
        if (op == 0x9) {  // ping -> pong
            ws_send_frame(fd, 0xA, pl, mask_pong);
            continue;
        }
        if (op == 0xA) continue;  // pong 忽略
        if (op == 0x1 || op == 0x0) {
            frag += pl;
            // FIN 位在 ws_recv_frame 中未回传；单帧常见，多帧按“短帧拼装”处理：
            // 只要收到文本/延续帧即视为完整（发送方不分片）。
            out = frag;
            return true;
        }
        // 二进制帧不支持，直接拒绝该连接。
        return false;
    }
}

void HttpServer::loop() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port_);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr))<0) { perror("bind"); return; }
    listen(listen_fd_, 16);
    while (running_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (!running_) break;
            continue;
        }
        std::thread(&HttpServer::handle_client, this, fd).detach();
    }
}

void HttpServer::handle_client(int fd) {
    char buf[8192];
    std::string data;
    // read headers
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, n);
        if (data.find("\r\n\r\n") != std::string::npos) break;
        if (data.size() > 65536) break;
    }
    HttpRequest req;
    std::istringstream iss(data);
    std::string line;
    std::getline(iss, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream ls(line);
    ls >> req.method >> req.path;
    // headers
    std::string header;
    while (std::getline(iss, header) && header != "\r" && !header.empty()) {
        if (header.back()=='\r') header.pop_back();
        size_t colon = header.find(':');
        if (colon != std::string::npos) {
            std::string key = header.substr(0,colon);
            std::string val = header.substr(colon+1);
            // trim
            while (!val.empty() && (val[0]==' '||val[0]=='\t')) val.erase(0,1);
            req.headers[key]=val;
        }
    }
    // query
    size_t q = req.path.find('?');
    if (q != std::string::npos) {
        std::string qs = req.path.substr(q+1);
        req.path = req.path.substr(0,q);
        std::istringstream qss(qs);
        std::string pair;
        while (std::getline(qss, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq != std::string::npos) req.query[url_decode(pair.substr(0,eq))] = url_decode(pair.substr(eq+1));
            else if (!pair.empty()) req.query[url_decode(pair)] = "";
        }
    }

    // WS 升级：同端口双服，流式路径由 set_ws_handler 登记。
    if (!ws_path_.empty() && req.path == ws_path_ && ws_handler_) {
        std::string up = header_get(req.headers, "Upgrade");
        std::string key = header_get(req.headers, "Sec-WebSocket-Key");
        if (lower_str(up).find("websocket") != std::string::npos && !key.empty()) {
            std::string acc = ws_accept_key(key);
            std::ostringstream hs;
            hs << "HTTP/1.1 101 Switching Protocols\r\n"
               << "Upgrade: websocket\r\n"
               << "Connection: Upgrade\r\n"
               << "Sec-WebSocket-Accept: " << acc << "\r\n\r\n";
            std::string s = hs.str();
            if (!send_all(fd, s.c_str(), s.size())) { close(fd); return; }
            // 接管：不再走短连接应答，fd 所有权移交 handler。
            ws_handler_(fd, req);
            return;
        }
    }

    // body
    auto it = req.headers.find("Content-Length");
    if (it == req.headers.end()) {
        // 大小写不敏感兜底
        for (auto& kv : req.headers)
            if (lower_str(kv.first) == "content-length") { it = req.headers.find(kv.first); break; }
    }
    if (it != req.headers.end()) {
        int len = 0;
        try { len = std::stoi(it->second); } catch (...) { len = 0; }
        if (len < 0) len = 0;
        if (len > (8 << 20)) len = (8 << 20);
        // data already may contain body
        size_t pos = data.find("\r\n\r\n");
        std::string bodypart = (pos==std::string::npos) ? "" : data.substr(pos+4);
        while ((int)bodypart.size() < len) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            bodypart.append(buf, n);
        }
        req.body = bodypart.substr(0, (size_t)len);
    }

    HttpResponse res;
    handler_(req, res);

    std::ostringstream out;
    out << "HTTP/1.1 " << res.status << " " << (res.status==200?"OK":"ERR") << "\r\n";
    out << "Content-Type: " << res.content_type << "\r\n";
    out << "Content-Length: " << res.body.size() << "\r\n";
    for (auto& h : res.headers) out << h.first << ": " << h.second << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << res.body;
    std::string s = out.str();
    send_all(fd, s.c_str(), s.size());
    close(fd);
}
