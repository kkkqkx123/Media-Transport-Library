#include "audio_owner.h"

#include <stdlib.h>
#include <string.h>

static int is_digits(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

static int valid_ip4(const char* s) {
    if (!s || !s[0]) return 0;
    int parts = 0;
    const char* p = s;
    while (1) {
        if (*p < '0' || *p > '9') return 0;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) return 0;
        }
        if (v > 255) return 0;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return 0;
        p++;
        if (*p == '\0') return 0;
    }
    return parts == 4;
}

dvs_audio_owner_kind_t dvs_audio_owner_classify(const char* owner) {
    if (!owner || !owner[0]) return DVS_OWNER_MUTE;
    if (strncmp(owner, "lane", 4) == 0) {
        const char* num = owner + 4;
        if (is_digits(num)) {
            long v = strtol(num, NULL, 10);
            if (v >= 0 && v < 256) return DVS_OWNER_LANE;
        }
        return DVS_OWNER_INVALID;
    }
    if (is_digits(owner)) return DVS_OWNER_INDEX;
    const char* colon = strchr(owner, ':');
    if (colon && colon != owner && colon[1] && !strchr(colon + 1, ':')) {
        char ip[64] = {0};
        size_t ilen = (size_t)(colon - owner);
        if (ilen > 0 && ilen < sizeof(ip)) {
            memcpy(ip, owner, ilen);
            if (valid_ip4(ip) && is_digits(colon + 1)) {
                long pv = strtol(colon + 1, NULL, 10);
                if (pv > 0 && pv < 65536) return DVS_OWNER_ENDPOINT;
            }
        }
        return DVS_OWNER_INVALID;
    }
    if (colon) return DVS_OWNER_INVALID;
    if (valid_ip4(owner)) return DVS_OWNER_SIP;
    return DVS_OWNER_INVALID;
}

int dvs_audio_owner_lane_index(const char* owner, int* lane_out) {
    if (!owner || !lane_out) return -1;
    dvs_audio_owner_kind_t k = dvs_audio_owner_classify(owner);
    if (k == DVS_OWNER_LANE) {
        long v = strtol(owner + 4, NULL, 10);
        if (v < 0 || v >= 256) return -1;
        *lane_out = (int)v;
        return 0;
    }
    if (k == DVS_OWNER_INDEX) {
        long v = strtol(owner, NULL, 10);
        if (v < 0 || v >= 256) return -1;
        *lane_out = (int)v;
        return 0;
    }
    return -1;
}
