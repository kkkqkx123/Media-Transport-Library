#include "ops.h"

#include "platform_store.h"

json_object* operation_to_json(const Operation& op) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "opId", json_object_new_string(op.op_id.c_str()));
    json_object_object_add(o, "revision", json_object_new_int(op.revision));
    json_object_object_add(o, "applyAtNs", json_object_new_int64((int64_t)op.apply_at_ns));
    json_object_object_add(o, "deadlineNs", json_object_new_int64((int64_t)op.deadline_ns));
    json_object_object_add(o, "createdNs", json_object_new_int64((int64_t)op.created_ns));
    json_object_object_add(o, "state", json_object_new_string(op.state.c_str()));
    json_object_object_add(o, "windowsJson", json_object_new_string(op.windows_json.c_str()));
    json_object_object_add(o, "wallW", json_object_new_int(op.wall_w));
    json_object_object_add(o, "wallH", json_object_new_int(op.wall_h));
    json_object_object_add(o, "wallId", json_object_new_string(op.wall_id.c_str()));
    json_object_object_add(o, "sceneId", json_object_new_string(op.scene_id.c_str()));
    json_object_object_add(o, "layoutHash",
                           json_object_new_string(op.layout_hash.c_str()));
    json_object_object_add(o, "volume", json_object_new_int(op.volume));
    json_object* members = json_object_new_array();
    for (const auto& m : op.members)
        json_object_array_add(members, json_object_new_string(m.c_str()));
    json_object_object_add(o, "members", members);
    json_object* ready = json_object_new_array();
    for (const auto& m : op.ready)
        json_object_array_add(ready, json_object_new_string(m.c_str()));
    json_object_object_add(o, "ready", ready);
    json_object* result = json_object_new_object();
    for (const auto& kv : op.result)
        json_object_object_add(result, kv.first.c_str(),
                               json_object_new_string(kv.second.c_str()));
    json_object_object_add(o, "result", result);
    return o;
}

static std::string jstr(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v &&
        json_object_is_type(v, json_type_string))
        return json_object_get_string(v);
    return "";
}
static int64_t jint64(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_int64(v);
    return 0;
}
static int jint(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_int(v);
    return 0;
}

bool operation_from_json(json_object* o, Operation* out) {
    if (!o || !out || !json_object_is_type(o, json_type_object)) return false;
    std::string op_id = jstr(o, "opId");
    std::string state = jstr(o, "state");
    if (op_id.empty() || state.empty()) return false;
    out->op_id = op_id;
    out->revision = jint(o, "revision");
    out->apply_at_ns = (uint64_t)jint64(o, "applyAtNs");
    out->deadline_ns = (uint64_t)jint64(o, "deadlineNs");
    out->created_ns = (uint64_t)jint64(o, "createdNs");
    out->state = state;
    out->windows_json = jstr(o, "windowsJson");
    out->wall_w = jint(o, "wallW");
    out->wall_h = jint(o, "wallH");
    out->wall_id = jstr(o, "wallId");
    out->scene_id = jstr(o, "sceneId");
    out->layout_hash = jstr(o, "layoutHash");
    out->volume = 80;
    {
        json_object* vv = nullptr;
        if (json_object_object_get_ex(o, "volume", &vv) && vv &&
            json_object_is_type(vv, json_type_int))
            out->volume = json_object_get_int(vv);
    }
    json_object* arr = nullptr;
    out->members.clear();
    if (json_object_object_get_ex(o, "members", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(arr); i++) {
            json_object* e = json_object_array_get_idx(arr, (int)i);
            if (e) out->members.insert(json_object_get_string(e));
        }
    }
    out->ready.clear();
    if (json_object_object_get_ex(o, "ready", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(arr); i++) {
            json_object* e = json_object_array_get_idx(arr, (int)i);
            if (e) out->ready.insert(json_object_get_string(e));
        }
    }
    out->result.clear();
    json_object* res = nullptr;
    if (json_object_object_get_ex(o, "result", &res) &&
        json_object_is_type(res, json_type_object)) {
        json_object_object_foreach(res, k, v) {
            if (k && v) out->result[k] = json_object_get_string(v);
        }
    }
    return true;
}

void OpCoordinator::persist(const Operation& op) {
    if (!store_) return;
    json_object* rec = operation_to_json(op);
    if (rec) {
        store_->put_doc("op", op.op_id, rec);  // borrows rec
        json_object_put(rec);
    }
}

void OpCoordinator::persist(const std::string& op_id) {
    auto it = ops_.find(op_id);
    if (it != ops_.end()) persist(it->second);
}

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
    // Snapshot/created fields are stamped by the caller via mutable_find();
    // reset them here so a re-begun op id never inherits stale values.
    op.created_ns = 0;
    op.windows_json.clear();
    op.wall_w = 0;
    op.wall_h = 0;
    op.wall_id.clear();
    op.scene_id.clear();
    op.layout_hash.clear();
    op.volume = 80;
    persist(op);
    return op;
}

void OpCoordinator::set_deadline(const std::string& op_id, uint64_t deadline_ns) {
    auto it = ops_.find(op_id);
    if (it == ops_.end()) return;
    it->second.deadline_ns = deadline_ns;
    persist(it->second);
}

Operation* OpCoordinator::mutable_find(const std::string& op_id) {
    auto it = ops_.find(op_id);
    return (it == ops_.end()) ? nullptr : &it->second;
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
        persist(op);
        return op.state;
    }
    op.state = (op.ready.size() >= op.members.size()) ? "committed" : "preparing";
    persist(op);
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
    // Marker makes the rollback replayable and idempotent: a blocked op
    // restored from the store without this marker is re-driven by the
    // sweeper (crash window between the blocked persist and the rollback).
    op.result["__rolledback__"] = "1";
    if (to_revision) *to_revision = op.revision - 1;
    persist(op);
    return op.op_id + "-rollback";
}

const Operation* OpCoordinator::find(const std::string& op_id) const {
    auto it = ops_.find(op_id);
    return (it == ops_.end()) ? nullptr : &it->second;
}

bool OpCoordinator::wall_has_preparing(const std::string& wall_id,
                                       const std::string& exclude_op) const {
    if (wall_id.empty()) return false;
    for (const auto& kv : ops_) {
        const Operation& op = kv.second;
        if (op.op_id == exclude_op) continue;
        if (op.state != "preparing") continue;
        if (op.wall_id == wall_id) return true;
    }
    return false;
}

int OpCoordinator::wall_committed_revision(const std::string& wall_id) const {
    int best = 0;
    for (const auto& kv : ops_) {
        const Operation& op = kv.second;
        if (op.wall_id != wall_id) continue;
        if (op.state != "committed" && op.state != "rolledback") continue;
        if (op.revision > best) best = op.revision;
    }
    return best;
}

std::vector<std::string> OpCoordinator::expire_sweep(uint64_t now_ns) {
    std::vector<std::string> out;
    for (auto& kv : ops_) {
        Operation& op = kv.second;
        if (op.state == "preparing" && op.deadline_ns != 0 && now_ns >= op.deadline_ns) {
            op.state = "blocked";
            persist(op);
            out.push_back(op.op_id);
        }
    }
    return out;
}

const Operation* OpCoordinator::find_committed(int revision) const {
    const Operation* best = nullptr;
    for (const auto& kv : ops_) {
        const Operation& op = kv.second;
        if (op.revision != revision) continue;
        if (op.state != "committed" && op.state != "rolledback") continue;
        if (op.windows_json.empty() || op.wall_w <= 0 || op.wall_h <= 0) continue;
        best = &op;  // std::map iterates in op_id order; keep the last match
    }
    return best;
}

std::vector<std::string> OpCoordinator::pending_rollbacks() const {
    std::vector<std::string> out;
    for (const auto& kv : ops_) {
        const Operation& op = kv.second;
        if (op.state != "blocked") continue;
        if (op.result.count("__rolledback__")) continue;
        out.push_back(op.op_id);
    }
    return out;
}

std::vector<std::string> OpCoordinator::gc(uint64_t now_ns, uint64_t ttl_ns) {
    std::vector<std::string> out;
    for (auto it = ops_.begin(); it != ops_.end();) {
        Operation& op = it->second;
        bool terminal = (op.state == "committed" || op.state == "rolledback" ||
                         op.state == "blocked");
        if (terminal && op.created_ns != 0 && now_ns > op.created_ns &&
            now_ns - op.created_ns > ttl_ns) {
            if (store_) store_->del_doc("op", op.op_id);
            out.push_back(op.op_id);
            it = ops_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}
