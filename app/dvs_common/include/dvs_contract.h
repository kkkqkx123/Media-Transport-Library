#pragma once
#include <json-c/json.h>

#include <map>
#include <string>

// DVS 平台-代理共享契约：信封形状与六类消息必填字段表。
// 与传输职责并列，不替代传输；两端同源编译，缺字段与类型错直接拒绝。
namespace dvs::contract {

// 通道消息类型（WS 承载与短连接语义一致）。
extern const char* kHello;           // agent -> platform 问候（注册）
extern const char* kWelcome;         // platform -> agent 握手回执（协商参数）
extern const char* kHeartbeat;       // agent -> platform 周期心跳
extern const char* kLayoutPrepare;   // platform -> agent 布局下发
extern const char* kBarrierAck;      // agent -> platform 屏障回执
extern const char* kConfigUpdate;    // platform -> agent 配置下发
extern const char* kConfigAck;       // agent -> platform 配置确认
extern const char* kUpgradeTask;     // platform -> agent 升级任务
extern const char* kUpgradeProgress;  // agent -> platform 升级进度
extern const char* kAck;             // 通用确认（msgId 幂等回执）
extern const char* kError;           // 通用错误

// 请求标识："<origin>-<seq>"，origin 为 "p"（平台）或 "a"（代理），
// 本进程内单调递增，不再允许一端固定值一端自增。
std::string next_request_id(const char* origin);
// 消息标识："<origin>-m<seq>"，用于幂等去重，重复投递返回首次结果。
std::string next_msg_id(const char* origin);

// 信封构造：调用方把 data 所有权交给函数（可为 nullptr）。
// 成功：{ok:true,status:200,code:"OK",msg,data,requestId}
// 失败：{ok:false,status,code,msg,data:{},requestId}
json_object* make_ok(json_object* data, const std::string& msg, const char* origin);
json_object* make_ok_with_id(json_object* data, const std::string& msg,
                             const std::string& request_id);
json_object* make_fail(const char* code, int status, const std::string& msg,
                       const char* origin);
json_object* make_fail_with_id(const char* code, int status, const std::string& msg,
                               const std::string& request_id);

// 鉴权入口统一判定：provided 为 Authorization 头值（"Bearer xxx" 或裸 token）
// 或裸 token；expected 为服务端期望的裸 token。空 expected 恒为真。
bool bearer_ok(const std::string& provided, const std::string& expected);
std::string extract_bearer(const std::map<std::string, std::string>& headers);
std::string strip_bearer(const std::string& v);

// 媒体面规范错误码（与 docs/req/06 第 4 节对齐）。通道 data 中出现
// reason/message 字段时优先使用这些码，调用方只做透传与审计。
namespace media_err {
inline constexpr const char* kDecodeNotReady = "DECODE_NOT_READY";
inline constexpr const char* kClockUnsynced = "CLOCK_UNSYNCED";
inline constexpr const char* kApplyAtTooSoon = "APPLY_AT_TOO_SOON";
inline constexpr const char* kFrameDeadlineMissed = "FRAME_DEADLINE_MISSED";
inline constexpr const char* kLayoutConflict = "LAYOUT_CONFLICT";
inline constexpr const char* kCapabilityExceeded = "CAPABILITY_EXCEEDED";
inline constexpr const char* kMediaSessionFailed = "MEDIA_SESSION_FAILED";
inline constexpr const char* kRollbackFailed = "ROLLBACK_FAILED";
}  // namespace media_err

// 全局帧号数学（平台与节点共用同一套取整）：frame(t) =
// floor((t - epoch) * fps / 1e9)。epoch 为墙级时间纪元（TAI 纳秒），
// fps 为墙输出帧率。apply_at 落在过去时返回第 0 帧，调用方此时应
// 视为立即提交。frame < 0 的起始时刻按第 0 帧计。
int64_t frame_index_for_apply_at(uint64_t epoch_ns, double fps,
                                  uint64_t apply_at_ns);
uint64_t frame_start_ns(uint64_t epoch_ns, double fps, int64_t frame);

// 六类消息校验：data 为通道消息的 data 对象。返回空串表示通过，
// 否则返回 "CODE:reason"（调用方据此直接拒绝，不做猜测补齐）。
std::string validate_hello(json_object* data);
std::string validate_heartbeat(json_object* data);
std::string validate_layout_prepare(json_object* data);
std::string validate_barrier_ack(json_object* data);
std::string validate_config_update(json_object* data);
std::string validate_config_ack(json_object* data);
std::string validate_upgrade_task(json_object* data);
std::string validate_upgrade_progress(json_object* data);

// 通道消息包装：{msgId,type,data,requestId}。data 所有权转移。
json_object* wrap(const std::string& msg_id, const std::string& type,
                 json_object* data, const std::string& request_id);
// 解析包装：成功返回 true，data_out 为新引用（调用方需 put），失败 data_out=null。
bool unwrap(json_object* msg, std::string& msg_id_out, std::string& type_out,
            json_object*& data_out, std::string& request_id_out);

// 环境协商缺省：与现状一致，空值即不校验/默认行为。
int heartbeat_interval_ms();  // DVS_HEARTBEAT_MS，缺省 3000
int barrier_timeout_ms();     // DVS_BARRIER_TIMEOUT_MS，缺省 10000
std::string transport_mode();  // DVS_TRANSPORT_MODE：auto|ws|short，缺省 auto
bool use_ws();                 // mode != short
bool ws_only();                // mode == ws

}  // namespace dvs::contract
