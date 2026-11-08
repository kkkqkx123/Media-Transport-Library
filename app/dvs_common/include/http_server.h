#pragma once
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <atomic>

struct HttpRequest { std::string method, path, body; std::map<std::string,std::string> headers; std::map<std::string,std::string> query; };
struct HttpResponse { int status=200; std::string body; std::string content_type="application/json; charset=utf-8"; std::map<std::string,std::string> headers; };

class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    // 长连接接管：握手成功后 fd 交给 h，h 负责帧循环并最终关闭 fd。
    using WsHandler = std::function<void(int fd, const HttpRequest& hs_req)>;
    HttpServer(int port, Handler h);
    ~HttpServer();
    void start();
    void stop();
    void set_ws_handler(const std::string& path, WsHandler h);

    // WS 帧原语（服务端发送不掩码，客户端发送必须掩码由调用方指定）。
    static bool ws_send_frame(int fd, uint8_t opcode, const std::string& payload,
                              bool mask = false);
    static bool ws_send_text(int fd, const std::string& payload, bool mask = false);
    static bool ws_send_ping(int fd, const std::string& payload = "", bool mask = false);
    static bool ws_send_pong(int fd, const std::string& payload = "", bool mask = false);
    static bool ws_send_close(int fd, bool mask = false);
    // 读取一帧（原始，不自动回 pong）。timeout_ms 内无数据返回 false。
    static bool ws_recv_frame(int fd, uint8_t& opcode, std::string& payload,
                              int timeout_ms);
    // 读取一个文本消息：自动回 pong、忽略 pong、拼装分片、遇 close 回 close 并返回 false。
    // mask_pong 决定自动 pong 是否掩码（服务端 false，客户端 true）。
    static bool ws_recv_text(int fd, std::string& out, int timeout_ms,
                             bool mask_pong = false);
    static std::string ws_accept_key(const std::string& client_key);
    // 统一静态文件 serving：共享的目录穿越防护与类型映射。
    // "/" 映射到 "/index.html"；命中常规文件时填充 res 并返回 true，
    // 否则保持 res 不变返回 false（调用方回 404 包络）。
    static bool serve_static_file(const std::string& web_root,
                                  const std::string& req_path,
                                  HttpResponse& res);

private:
    void loop();
    void handle_client(int fd);
    int port_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int listen_fd_=-1;
    std::string ws_path_;
    WsHandler ws_handler_;
};
