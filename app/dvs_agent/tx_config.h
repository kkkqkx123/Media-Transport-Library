#pragma once
#include <json-c/json.h>
#include <string>

/* TX parameter model for the input scope (encode + single target).
 *
 * Responsibility split: this module owns parsing, defaults and range checks.
 * main.cpp owns the config state machine and the tx_start forwarding.
 * Ceilings come from the frozen SoC spec, narrowed per-field by a reported
 * capability.tx object when present (reported wins, never masked).
 */
struct TxEncode {
    std::string codec = "h264";  // "h264" | "h265"
    int width = 1920;            // 640..cap.maxWidth
    int height = 1080;           // 480..cap.maxHeight
    int fps = 30;                // 24..cap.maxFps
    int bitrate = 4000;          // 500..cap.maxBitrateKbps (kbps)
};

struct TxTarget {
    std::string sip;  // IPv4
    int udpPort = 0;  // 1..65535
};

struct TxParams {
    std::string port;  // capture port id, e.g. "HDMI1"
    TxEncode enc;
    TxTarget tgt;
    bool hasEncode = false;
    bool hasTarget = false;
};

struct TxCaps {
    std::string codecs = "h264,h265";
    int maxW = 3840;
    int maxH = 2160;
    int maxFps = 60;
    int maxBr = 20000;
};

/* Read capabilities.tx when present; otherwise keep the frozen ceilings. */
TxCaps tx_caps_from_node(json_object* capabilities);

/* Parse desiredValue {port, encode{codec,width,height,fps,bitrateKbps},
 * target{sip,udpPort}} with flat fallbacks. Missing encode fields take
 * defaults; a missing encode/target object means "not requested". */
bool tx_parse(json_object* val, TxParams& out, std::string& err);

/* Range checks against caps (port presence is checked by the caller against
 * inputPorts). hasEncode/hasTarget false skips the respective block. */
bool tx_validate(const TxParams& p, const TxCaps& caps, std::string& err);

/* Build the tx_start payload (new ref). */
json_object* tx_start_payload(const TxParams& p);
