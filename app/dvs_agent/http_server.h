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
    HttpServer(int port, Handler h);
    ~HttpServer();
    void start();
    void stop();
private:
    void loop();
    void handle_client(int fd);
    int port_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int listen_fd_=-1;
};
