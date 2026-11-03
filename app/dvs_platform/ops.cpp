#include "ops.h"

Operation& OpCoordinator::begin(const std::string& op_id, int revision,
                                const std::set<std::string>& members,
                                uint64_t apply_at_ns) {
    Operation& op = ops_[op_id];
    op.op_id = op_id;
    op.revision = revision;
    op.members = members;
    op.apply_at_ns = apply_at_ns;
    op.ready.clear();
    op.result.clear();
    op.state = "preparing";
    return op;
}

std::string OpCoordinator::on_reply(const std::string& op_id,
                                    const std::string& node, bool ok,
                                    const std::string& status) {
    auto it = ops_.find(op_id);
    if (it == ops_.end()) return "unknown";
    Operation& op = it->second;
    if (op.members.count(node)) op.ready.insert(node);
    op.result[node] = status;
    if (!ok) {
        op.state = "blocked";
        return op.state;
    }
    op.state = (op.ready.size() >= op.members.size()) ? "committed" : "preparing";
    return op.state;
}

std::set<std::string> OpCoordinator::missing(const std::string& op_id) {
    std::set<std::string> out;
    auto it = ops_.find(op_id);
    if (it == ops_.end()) return out;
    for (const auto& m : it->second.members)
        if (!it->second.ready.count(m)) out.insert(m);
    return out;
}

std::string OpCoordinator::rollback_op(const std::string& op_id, int* to_revision) {
    auto it = ops_.find(op_id);
    if (it == ops_.end()) return "";
    Operation& op = it->second;
    if (op.state != "blocked") return "";
    op.state = "rolledback";
    if (to_revision) *to_revision = op.revision - 1;
    return op.op_id + "-rollback";
}

const Operation* OpCoordinator::find(const std::string& op_id) const {
    auto it = ops_.find(op_id);
    return (it == ops_.end()) ? nullptr : &it->second;
}
