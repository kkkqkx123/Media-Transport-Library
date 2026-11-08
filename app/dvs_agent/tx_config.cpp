#include "tx_config.h"

#include <arpa/inet.h>

#include <cstdlib>

static bool valid_ip4(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    struct in_addr a;
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

static std::string trim_str(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

static int int_field(json_object* o, const char* key, int def) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_int(v);
    return def;
}

static std::string str_field(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}

TxCaps tx_caps_from_node(json_object* capabilities) {
    TxCaps c;
    json_object* tx = nullptr;
    if (!capabilities ||
        !json_object_object_get_ex(capabilities, "tx", &tx) || !tx)
        return c;
    json_object* arr = nullptr;
    if (json_object_object_get_ex(tx, "codecs", &arr) && arr) {
        if (json_object_is_type(arr, json_type_array) &&
            json_object_array_length(arr) > 0) {
            std::string joined;
            for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
                json_object* e = json_object_array_get_idx(arr, (int)i);
                if (!e || !json_object_is_type(e, json_type_string)) continue;
                if (!joined.empty()) joined.push_back(',');
                joined += json_object_get_string(e);
            }
            if (!joined.empty() && joined.size() < 64) c.codecs = joined;
        } else if (json_object_is_type(arr, json_type_string)) {
            std::string s = trim_str(json_object_get_string(arr));
            if (!s.empty() && s.size() < 64) c.codecs = s;
        }
    }
    int v = int_field(tx, "maxWidth", 0);
    if (v > 0) c.maxW = v;
    v = int_field(tx, "maxHeight", 0);
    if (v > 0) c.maxH = v;
    v = int_field(tx, "maxFps", 0);
    if (v > 0) c.maxFps = v;
    v = int_field(tx, "maxBitrateKbps", 0);
    if (v > 0) c.maxBr = v;
    return c;
}

bool tx_parse(json_object* val, TxParams& out, std::string& err) {
    out = TxParams();
    if (!val || !json_object_is_type(val, json_type_object)) {
        err = "bad input value";
        return false;
    }
    out.port = trim_str(str_field(val, "port"));
    if (out.port.empty() || out.port.size() >= 32) {
        err = "采集口不能为空";
        return false;
    }
    json_object* enc = nullptr;
    if (json_object_object_get_ex(val, "encode", &enc) && enc &&
        json_object_is_type(enc, json_type_object)) {
        out.hasEncode = true;
        std::string codec = trim_str(str_field(enc, "codec"));
        if (!codec.empty()) out.enc.codec = codec;
        out.enc.width = int_field(enc, "width", out.enc.width);
        out.enc.height = int_field(enc, "height", out.enc.height);
        out.enc.fps = int_field(enc, "fps", out.enc.fps);
        out.enc.bitrate = int_field(enc, "bitrateKbps", out.enc.bitrate);
        out.enc.gopMs = int_field(enc, "gopMs", out.enc.gopMs);
    }
    json_object* tgt = nullptr;
    if (json_object_object_get_ex(val, "target", &tgt) && tgt &&
        json_object_is_type(tgt, json_type_object)) {
        out.hasTarget = true;
        out.tgt.sip = trim_str(str_field(tgt, "sip"));
        out.tgt.udpPort = int_field(tgt, "udpPort", out.tgt.udpPort);
        out.tgt.audioUdpPort = int_field(tgt, "audioUdpPort", out.tgt.audioUdpPort);
    }
    return true;
}

bool tx_validate(const TxParams& p, const TxCaps& caps, std::string& err) {
    if (p.hasEncode) {
        bool codec_ok = false;
        size_t start = 0;
        for (size_t i = 0; i <= caps.codecs.size(); i++) {
            if (i == caps.codecs.size() || caps.codecs[i] == ',') {
                if (caps.codecs.substr(start, i - start) == p.enc.codec) {
                    codec_ok = true;
                    break;
                }
                start = i + 1;
            }
        }
        if (!codec_ok) {
            err = "编码格式超出本机能力";
            return false;
        }
        if (p.enc.width < 640 || p.enc.width > caps.maxW ||
            p.enc.height < 480 || p.enc.height > caps.maxH) {
            err = "编码分辨率超出本机能力范围";
            return false;
        }
        if (p.enc.fps < 24 || p.enc.fps > caps.maxFps) {
            err = "编码帧率超出本机能力范围";
            return false;
        }
        if (p.enc.bitrate < 500 || p.enc.bitrate > caps.maxBr) {
            err = "编码码率超出本机能力范围";
            return false;
        }
        // GOP 间隔：0 取编码器默认（跟随帧率），显式值 250..4000ms。
        // 短间隔换场景切换速度，长间隔省带宽，由平台按墙策略下发。
        if (p.enc.gopMs != 0 && (p.enc.gopMs < 250 || p.enc.gopMs > 4000)) {
            err = "编码 GOP 间隔超出范围 (250..4000ms, 0=默认)";
            return false;
        }
    }
    if (p.hasTarget) {
        if (!valid_ip4(p.tgt.sip)) {
            err = "发送目标地址非法";
            return false;
        }
        if (p.tgt.udpPort < 1 || p.tgt.udpPort > 65535) {
            err = "发送目标端口非法";
            return false;
        }
        if (p.tgt.audioUdpPort != 0 &&
            (p.tgt.audioUdpPort < 1 || p.tgt.audioUdpPort > 65535)) {
            err = "发送音频端口非法";
            return false;
        }
    }
    return true;
}

json_object* tx_start_payload(const TxParams& p) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "port", json_object_new_string(p.port.c_str()));
    json_object* enc = json_object_new_object();
    json_object_object_add(enc, "codec",
                           json_object_new_string(p.enc.codec.c_str()));
    json_object_object_add(enc, "width", json_object_new_int(p.enc.width));
    json_object_object_add(enc, "height", json_object_new_int(p.enc.height));
    json_object_object_add(enc, "fps", json_object_new_int(p.enc.fps));
    json_object_object_add(enc, "bitrateKbps", json_object_new_int(p.enc.bitrate));
    if (p.enc.gopMs != 0)
        json_object_object_add(enc, "gopMs", json_object_new_int(p.enc.gopMs));
    json_object_object_add(o, "encode", enc);
    json_object* tgt = json_object_new_object();
    json_object_object_add(tgt, "sip", json_object_new_string(p.tgt.sip.c_str()));
    json_object_object_add(tgt, "udpPort", json_object_new_int(p.tgt.udpPort));
    if (p.tgt.audioUdpPort != 0)
        json_object_object_add(tgt, "audioUdpPort",
                               json_object_new_int(p.tgt.audioUdpPort));
    json_object_object_add(o, "target", tgt);
    return o;
}
