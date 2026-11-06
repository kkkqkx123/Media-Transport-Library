#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <json-c/json.h>

class PlatformStore;  // optional persistence backend, see set_store()

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
    // 持久化/回退辅助：created_ns 用于 gc TTL（begin 时由调用方写入，0 = 未知）；
    // 窗口快照供 execute_rollback 对 revision-1 重发（见 find_committed）。
    uint64_t created_ns = 0;
    std::string windows_json;
    int wall_w = 0;
    int wall_h = 0;
};

class OpCoordinator {
public:
    // Optional persistence backend (kind "op" docs). Null keeps the
    // coordinator memory-only (unit tests). Set once at startup, before any
    // begin(); callers serialize access with g_ops_mu as before.
    void set_store(PlatformStore* store) { store_ = store; }
    // Crash recovery: reinsert a persisted operation without re-persisting.
    void restore(const Operation& op) { ops_[op.op_id] = op; }
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
    // Marks the op with result["__rolledback__"] so the rollback is replayable
    // and idempotent across restarts (see pending_rollbacks).
    std::string rollback_op(const std::string& op_id, int* to_revision);
    const Operation* find(const std::string& op_id) const;
    // Mutable access for callers holding their own coordinator lock (the
    // HTTP layer stamps the fanout snapshot / created_ns after begin()).
    Operation* mutable_find(const std::string& op_id);
    // 超时回收：把已过 deadline 仍 preparing 的屏障置为 blocked 并返回其 id。
    std::vector<std::string> expire_sweep(uint64_t now_ns);
    // The latest settled (committed/rolledback) op with this revision, for
    // rollback reissue; nullptr when no snapshot exists.
    const Operation* find_committed(int revision) const;
    // Blocked ops that have not been rolled back yet (no __rolledback__
    // marker). The sweeper replays these after a crash that landed between
    // the blocked persist and the rollback execution.
    std::vector<std::string> pending_rollbacks() const;
    // Erase terminal-state (committed/rolledback/blocked) ops whose
    // created_ns is older than ttl_ns, deleting their persisted docs.
    // Preparing ops are never gc'd — expire_sweep() owns them.
    std::vector<std::string> gc(uint64_t now_ns, uint64_t ttl_ns);

private:
    // Persist one operation as a docs("op", op_id) record (no-op when no
    // store is set). Called at the end of every state-changing method.
    void persist(const Operation& op);

    std::map<std::string, Operation> ops_;
    PlatformStore* store_ = nullptr;
};

// Wire format for persisting one operation (docs kind "op", id = op_id).
// windowsJson carries the fanout snapshot verbatim for rollback reissue.
json_object* operation_to_json(const Operation& op);
// Inverse of operation_to_json; returns false (out untouched) when required
// fields (opId/state) are missing or empty — callers drop such docs.
bool operation_from_json(json_object* o, Operation* out);
