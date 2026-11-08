#pragma once
#include <json-c/json.h>

#include <map>
#include <mutex>
#include <string>

#include "http_server.h"

// Transport sessions to nodes: at most one live WS handle per node, plus a
// small idempotency cache so retried frames never double-apply. Short HTTP
// stays stateless and is resolved per fanout from the node doc.
struct ChannelRegistry {
    std::mutex mu;
    std::map<std::string, int> node_fd;
    std::map<int, std::string> fd_node;
    std::map<std::string, std::string> transport;
    std::map<std::string, std::string> idem;
    std::map<std::string, std::string> upgrade_progress;
    std::mutex send_mu;
    void register_conn(const std::string& node, int fd);
    void unregister_fd(int fd);
    std::string node_of(int fd);
    int fd_of(const std::string& node);
    void set_transport(const std::string& node, const std::string& t);
    bool idem_hit(const std::string& k, std::string& out);
    void idem_store(const std::string& k, const std::string& v);
    // Lookup and send atomically: holds both locks so a replaced handle can
    // never receive a frame meant for the previous connection.
    bool send_to(const std::string& node, const std::string& frame);
};

// Wrap a contract payload and push it over the node's WS channel.
bool ws_push(ChannelRegistry& chan, const std::string& node,
             const std::string& type, json_object* data);
