#include "ops.h"

Operation& OpCoordinator::begin(const std::string& op_id, int revision,
                                const std::set<std::string>& members,
                                uint64_t apply_at_ns) {
    return begin_with_deadline(op_id, revision, members, apply_at_ns, 0);
}

Operation& OpCoordinator::begin_with_deadline(const std::string& op_id, int revision,
                                              const std::set<std::string>& members,
                                              uint64_t apply_at_ns,
                                              uint64_t deadline_ns) {
    Operation& op = ops_[op_id];
    op.op_id = op_id;
    op.revision = revision;
    op.members = members;
    op.apply_at_ns = apply_at_ns;
    op.deadline_ns = deadline_ns;
    op.ready.clear();
    op.result.clear();
    op.state = "preparing";
    return op;
}

void OpCoordinator::set_deadline(const std::string& op_id, uint64_t deadline_ns) {
    auto it = ops_.find(op_id);
    if (it == ops_.end()) return;
    it->second.deadline_ns = deadline_ns;
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

std::vector<std::string> OpCoordinator::expire_sweep(uint64_t now_ns) {
    std::vector<std::string> out;
    for (auto& kv : ops_) {
        Operation& op = kv.second;
        if (op.state == "preparing" && op.deadline_ns != 0 && now_ns >= op.deadline_ns) {
            op.state = "blocked";
            out.push_back(op.op_id);
        }
    }
    return out;
}
