#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

// Cross-node transaction coordinator (T8). Pure logic, no sockets: the HTTP
// layer feeds node replies in and reads the next broadcast out.
struct Operation {
    std::string op_id;
    int revision = 0;
    uint64_t apply_at_ns = 0;
    std::set<std::string> members;
    std::set<std::string> ready;
    std::map<std::string, std::string> result; // node -> status
    std::string state; // preparing | committed | rolledback | blocked
};

class OpCoordinator {
public:
    // Begin a prepare barrier for the given members. apply_at_ns should come
    // from layout_apply_at_ns(); 0 means commit immediately once all ready.
    Operation& begin(const std::string& op_id, int revision,
                     const std::set<std::string>& members, uint64_t apply_at_ns);
    // Record one node's prepare/commit reply. Returns the operation state.
    std::string on_reply(const std::string& op_id, const std::string& node,
                         bool ok, const std::string& status);
    // Nodes still missing a ready reply (empty when the barrier is met).
    std::set<std::string> missing(const std::string& op_id);
    // Rollback descriptor: reissue path back to revision-1. Empty when none.
    std::string rollback_op(const std::string& op_id, int* to_revision);
    const Operation* find(const std::string& op_id) const;

private:
    std::map<std::string, Operation> ops_;
};
