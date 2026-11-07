#pragma once
#include <atomic>
#include <functional>
#include <json-c/json.h>
#include <mutex>
#include <string>
#include <thread>

#include "http_client.h"

class NodeStore;
class MediaIpcClient;

/* 注册与心跳：优先 WS 长连接（同端口 /api/v1/channel/ws），失败回退短连接。
 * - 空 platform URL => 本地自治：不拨出，仅维持在线。
 * - DVS_TRANSPORT_MODE=short => 只用短连接；auto（缺省）优先 WS，ws 仅 WS。
 * 首包 hello 携带 token，经共享契约校验；后续心跳为 WS 上行消息。
 * 下行 layout.prepare / config.update / upgrade.task 经回调交由 main 处理，
 * 回执经 uplink_* 由 WS 上行（无 WS 时回退短连接 commit）。
 * 传输失败永不抛异常、不干扰媒体。 */
class Registration {
public:
    // 下行分发：type 为通道类型，data 为其 data 对象（借用，回调内如需保留请 get）。
    using DownlinkHandler =
        std::function<void(const std::string& type, json_object* data,
                           const std::string& msg_id)>;
    Registration(NodeStore* store, MediaIpcClient* media,
                 const std::string& platform_url, int interval_ms = 3000,
                 const std::string& agent_base = "");
    ~Registration();

    void start();
    void stop();

    bool online() const { return online_; }
    std::string register_state() const;
    std::string transport() const;
    bool trigger_once();
    void trigger_async();

    void set_downlink_handler(DownlinkHandler h);

    json_object* build_heartbeat();
    json_object* build_register();

    // 上行回执：优先 WS，WS 不可用时回退短连接。返回 true 表示已送达传输层。
    bool uplink_barrier_ack(const std::string& opid, const std::string& node,
                            const std::string& status, json_object* lanes);
    bool uplink_config_ack(const std::string& node, int applied_revision);
    bool uplink_upgrade_progress(const std::string& task_id, const std::string& node,
                                 const std::string& status,
                                 const std::string& detail = "");

private:
    void loop();
    bool register_once();
    bool heartbeat_once();

    // WS 路径
    bool ws_loop_once();  // 一次建链 + 会话直至断开；返回是否曾注册成功
    bool ws_send_wrapped(const std::string& type, json_object* data,
                         std::string* reply_out = nullptr);
    std::string ws_channel_url() const;

    NodeStore* store_;
    MediaIpcClient* media_;
    std::string platform_url_;
    std::string agent_base_;
    int interval_ms_;

    std::atomic<bool> running_{false};
    std::atomic<bool> online_{true};
    mutable std::mutex state_mu_;
    std::string register_state_;
    std::string transport_ = "short";  // ws | short
    std::thread thread_;
    int fail_streak_ = 0;

    WsClient ws_;       // 持久化通道，会话存续期由 loop 线程管理
    std::mutex ws_mu_;  // 串行化 WS 发送（心跳线程与回执线程共用）
    std::mutex dl_mu_;
    DownlinkHandler downlink_;

    time_t start_time_;
};
