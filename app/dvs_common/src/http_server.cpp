#include "http_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <map>

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i=0;i<s.size();++i){
        if (s[i]=='%') { if (i+2<s.size()) { int v; sscanf(s.c_str()+i+1,"%2x",&v); out.push_back((char)v); i+=2; } }
        else if (s[i]=='+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

HttpServer::HttpServer(int port, Handler h) : port_(port), handler_(h) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() { running_=true; thread_=std::thread(&HttpServer::loop,this); }

void HttpServer::stop() { running_=false; if (listen_fd_>=0) close(listen_fd_); if (thread_.joinable()) thread_.join(); }

void HttpServer::loop() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port_);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr))<0) { perror("bind"); return; }
    listen(listen_fd_, 16);
    while (running_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
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
    // body
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        int len = std::stoi(it->second);
        // data already may contain body
        size_t pos = data.find("\r\n\r\n");
        std::string bodypart = (pos==std::string::npos) ? "" : data.substr(pos+4);
        while ((int)bodypart.size() < len) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            bodypart.append(buf, n);
        }
        req.body = bodypart.substr(0, len);
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
    send(fd, s.c_str(), s.size(), 0);
    close(fd);
}
