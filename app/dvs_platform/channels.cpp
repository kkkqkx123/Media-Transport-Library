#include "channels.h"

#include <sys/socket.h>
#include <unistd.h>

#include "dvs_contract.h"

namespace ct = dvs::contract;

void ChannelRegistry::register_conn(const std::string& node, int fd) {
    int old = -1;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = node_fd.find(node);
        if (it != node_fd.end() && it->second != fd) {
            old = it->second;
            fd_node.erase(old);
        }
        node_fd[node] = fd;
        fd_node[fd] = node;
        transport[node] = "ws";
    }
    if (old >= 0) ::shutdown(old, SHUT_RDWR);
}

void ChannelRegistry::unregister_fd(int fd) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = fd_node.find(fd);
    if (it == fd_node.end()) return;
    std::string node = it->second;
    fd_node.erase(it);
    auto nit = node_fd.find(node);
    if (nit != node_fd.end() && nit->second == fd) node_fd.erase(nit);
}

std::string ChannelRegistry::node_of(int fd) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = fd_node.find(fd);
    return it == fd_node.end() ? "" : it->second;
}

int ChannelRegistry::fd_of(const std::string& node) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = node_fd.find(node);
    return it == node_fd.end() ? -1 : it->second;
}

void ChannelRegistry::set_transport(const std::string& node,
                                    const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    transport[node] = t;
}

bool ChannelRegistry::idem_hit(const std::string& k, std::string& out) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = idem.find(k);
    if (it == idem.end()) return false;
    out = it->second;
    return true;
}

void ChannelRegistry::idem_store(const std::string& k, const std::string& v) {
    std::lock_guard<std::mutex> lk(mu);
    if (idem.size() > 4096) idem.clear();
    idem[k] = v;
}

bool ChannelRegistry::send_to(const std::string& node,
                              const std::string& frame) {
    std::unique_lock<std::mutex> l1(mu, std::defer_lock);
    std::unique_lock<std::mutex> l2(send_mu, std::defer_lock);
    std::lock(l1, l2);
    auto it = node_fd.find(node);
    if (it == node_fd.end()) return false;
    return HttpServer::ws_send_text(it->second, frame, false);
}

bool ws_push(ChannelRegistry& chan, const std::string& node,
             const std::string& type, json_object* data) {
    json_object* msg =
        ct::wrap(ct::next_msg_id("p"), type, data, ct::next_request_id("p"));
    const char* s = json_object_to_json_string(msg);
    std::string frame = s ? s : "{}";
    json_object_put(msg);
    return chan.send_to(node, frame);
}
