#include "media_ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <string>

MediaIpcClient::MediaIpcClient(const std::string& sock_path) : sock_path_(sock_path) {}

std::string MediaIpcClient::request(const std::string& json_line) {
    if (sock_path_.empty()) return "";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path)-1);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return ""; }
    std::string msg = json_line + "\n";
    write(fd, msg.c_str(), msg.size());
    char buf[8192];
    std::string out;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, n);
        if (out.find('\n') != std::string::npos) break;
    }
    close(fd);
    return out;
}

std::string MediaIpcClient::query_state() {
    return request("{\"type\":\"query_state\"}");
}
