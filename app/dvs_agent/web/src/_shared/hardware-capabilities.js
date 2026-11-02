/*
 * DVS RK3588 硬件能力桥接（原型用）。
 *
 * 芯片级能力来自 Rockchip RK3588 官方 Brief Datasheet；具体底板的物理
 * 接口数量必须由节点注册回传的 capabilities 覆盖。页面只展示 resolve()
 * 返回的实际可用端口，不把 SoC 的可选接口当成每台设备都存在。
 */
(function (global) {
  "use strict";

  var OFFICIAL_SOURCE = "Rockchip RK3588 Brief Datasheet";
  var PROFILES = {
    "rk3588-reference-hdmi": {
      id: "rk3588-reference-hdmi",
      name: "RK3588 · HDMI 单口节点基线",
      source: OFFICIAL_SOURCE,
      sourceUrl: "https://www.rock-chips.com/uploads/pdf/2022.8.26/191/RK3588%20Brief%20Datasheet.pdf",
      physicalPorts: {
        input: [
          { id: "HDMI1", label: "HDMI 输入 1", kind: "HDMI 2.0 RX", maxWidth: 3840, maxHeight: 2160, maxRefreshHz: 60 }
        ],
        output: [
          { id: "HDMI-A", label: "HDMI 输出 A", kind: "HDMI 2.1 TX", maxWidth: 7680, maxHeight: 4320, maxRefreshHz: 60 }
        ]
      },
      silicon: {
        input: "HDMI 2.0 输入最高 4K@60；另有 MIPI CSI-2 / DVP 输入能力，是否引出由底板决定",
        output: "多显示引擎最高 8K@60；HDMI 2.1/eDP Combo、DP 1.3、双路 MIPI-DSI",
        sync: "芯片资料未承诺跨节点同帧；需控制服务与统一时钟方案验证"
      },
      safeModes: [
        { id: "1920x1080@60", label: "1920×1080 · 60Hz", width: 1920, height: 1080, refreshHz: 60 },
        { id: "2560x1440@60", label: "2560×1440 · 60Hz", width: 2560, height: 1440, refreshHz: 60 },
        { id: "3840x2160@30", label: "3840×2160 · 30Hz", width: 3840, height: 2160, refreshHz: 30 },
        { id: "3840x2160@60", label: "3840×2160 · 60Hz", width: 3840, height: 2160, refreshHz: 60 },
        { id: "7680x4320@60", label: "7680×4320 · 60Hz", width: 7680, height: 4320, refreshHz: 60 }
      ],
      customRange: { minWidth: 640, maxWidth: 7680, minHeight: 480, maxHeight: 4320, minRefreshHz: 24, maxRefreshHz: 60 },
      limits: [
        "HDMI 输入超过 3840×2160@60 时拒绝采集",
        "输出自定义刷新率最高按端口能力限制为 60Hz；不展示 120/240Hz",
        "跨节点同帧不是 RK3588 芯片级保证，需平台时钟与控制服务联调",
        "多路输出并发、色深、HDCP、EDID 和实际接口数量以节点回读及底板测试为准"
      ]
    }
  };

  function clone(value) {
    return JSON.parse(JSON.stringify(value));
  }

  function getProfile(id) {
    return clone(PROFILES[id] || PROFILES["rk3588-reference-hdmi"]);
  }

  function portLimit(port, fallback) {
    return {
      maxWidth: Number(port && port.maxWidth) || fallback.maxWidth,
      maxHeight: Number(port && port.maxHeight) || fallback.maxHeight,
      maxRefreshHz: Number(port && port.maxRefreshHz) || fallback.maxRefreshHz
    };
  }

  function enrichPorts(ports, fallback) {
    return (ports || []).map(function (port) {
      var limit = portLimit(port, fallback);
      return {
        id: port.id,
        label: port.label || port.id,
        kind: port.kind || "未标注接口",
        maxWidth: limit.maxWidth,
        maxHeight: limit.maxHeight,
        maxRefreshHz: limit.maxRefreshHz
      };
    });
  }

  function resolve(node) {
    var profile = getProfile(node && node.hardwareProfile);
    var reported = node && node.capabilities || {};
    var inputPorts = enrichPorts(Array.isArray(reported.inputPorts) && reported.inputPorts.length
      ? reported.inputPorts : profile.physicalPorts.input, { maxWidth: 3840, maxHeight: 2160, maxRefreshHz: 60 });
    var outputPorts = enrichPorts(Array.isArray(reported.outputPorts) && reported.outputPorts.length
      ? reported.outputPorts : profile.physicalPorts.output, profile.customRange);
    var outputId = node && node.output && node.output.connector;
    var selectedOutput = outputPorts.find(function (port) { return port.id === outputId; }) || outputPorts[0];
    var outputLimit = portLimit(selectedOutput, profile.customRange);
    var safeModes = profile.safeModes.filter(function (mode) {
      return mode.width <= outputLimit.maxWidth && mode.height <= outputLimit.maxHeight && mode.refreshHz <= outputLimit.maxRefreshHz;
    });
    return {
      profile: profile,
      inputPorts: inputPorts,
      outputPorts: outputPorts,
      selectedOutput: selectedOutput,
      outputLimit: outputLimit,
      safeModes: safeModes,
      customRange: {
        minWidth: profile.customRange.minWidth,
        maxWidth: outputLimit.maxWidth,
        minHeight: profile.customRange.minHeight,
        maxHeight: outputLimit.maxHeight,
        minRefreshHz: profile.customRange.minRefreshHz,
        maxRefreshHz: outputLimit.maxRefreshHz
      },
      sync: profile.silicon.sync,
      limits: profile.limits
    };
  }

  global.DvsHardwareCapabilities = {
    officialSource: OFFICIAL_SOURCE,
    profiles: PROFILES,
    getProfile: getProfile,
    resolve: resolve
  };
})(window);
