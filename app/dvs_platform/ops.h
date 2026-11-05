#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Cross-node transaction coordinator. Pure logic, no sockets: the transport
// layer (short HTTP or WS channel) feeds node replies in and reads the next
// broadcast out.
struct Operation {
    std::string op_id;
    int revision = 0;
    uint64_t apply_at_ns = 0;
    uint64_t deadline_ns = 0;  // 0 = 无超时；超时未齐按阻塞处理并回滚
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
    // 带超时登记：deadline_ns 为 TAI 纳秒，0 表示不设超时。
    Operation& begin_with_deadline(const std::string& op_id, int revision,
                                   const std::set<std::string>& members,
                                   uint64_t apply_at_ns, uint64_t deadline_ns);
    void set_deadline(const std::string& op_id, uint64_t deadline_ns);
    // Record one node's prepare/commit reply. Returns the operation state.
    std::string on_reply(const std::string& op_id, const std::string& node,
                         bool ok, const std::string& status);
    // Nodes still missing a ready reply (empty when the barrier is met).
    std::set<std::string> missing(const std::string& op_id);
    // Rollback descriptor: reissue path back to revision-1. Empty when none.
    std::string rollback_op(const std::string& op_id, int* to_revision);
    const Operation* find(const std::string& op_id) const;
    // 超时回收：把已过 deadline 仍 preparing 的屏障置为 blocked 并返回其 id。
    std::vector<std::string> expire_sweep(uint64_t now_ns);

private:
    std::map<std::string, Operation> ops_;
};
