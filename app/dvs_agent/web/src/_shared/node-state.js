/*
 * DVS 节点注册 / 心跳状态桥接（原型用）。
 *
 * 生产实现应由平台 API /api/v1/nodes 与 /api/v1/nodes/heartbeat 提供同一份数据；
 * 本原型用 localStorage 模拟跨页面的注册快照与节点回传，页面不得再维护自己的节点示例数组。
 */
(function (global) {
  "use strict";

  var STORAGE_KEY = "dvs.node.state.v1";
  var SOURCE = "节点注册 / 心跳回传";
  var RUNTIME_STATES = {
    online: "在线",
    offline: "离线",
    registering: "注册中",
    register_failed: "注册失败",
    network_reconfiguring: "网络切换中",
    rebooting: "重启中",
    factory_resetting: "恢复出厂中",
    upgrading: "升级中",
    upgrade_failed: "升级失败"
  };

  var seed = [
    {
      id: "d1", name: "输出盒-D1", type: "output", role: "output", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0051", mac: "AA:BB:CC:00:00:51", ip: "192.168.10.51", fw: "V1.0.0-20261019",
      online: true, lastHeartbeat: "2026-10-20 10:06", registeredAt: "2026-09-12 10:20", lastRegisterAt: "2026-10-20 10:06",
      registerMode: "auto", registerId: "SN202610190001", registerState: "registered", registerMessage: "已注册",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      hardwareProfile: "rk3588-reference-hdmi",
      capabilities: { inputPorts: [{ id: "HDMI1", label: "HDMI 输入 1", kind: "HDMI 2.0 RX", maxWidth: 3840, maxHeight: 2160, maxRefreshHz: 60 }], outputPorts: [{ id: "HDMI-A", label: "HDMI 输出 A", kind: "HDMI 2.1 TX", maxWidth: 7680, maxHeight: 4320, maxRefreshHz: 60 }] },
      output: { width: 2560, height: 1440, refreshHz: 60, mode: "force", connector: "HDMI-A", capability: { maxWidth: 7680, maxHeight: 4320, maxRefreshHz: 60 } },
      input: { port: "HDMI1", signal: null },
      sip: { enabled: true, state: "connected", server: "192.168.10.210", port: 5060, deviceId: "SN202610190001", user: "node-d1", lastAt: "2026-10-20 10:05" },
      currentPlayback: { taskId: "PUB-20261010-018", name: "午间主题", status: "playing", startedAt: "2026-10-20 10:05", wall: "展厅主屏" },
      exception: null, bind: "展厅主屏 · 4 格位", lifecycle: "active", uptime: "27 天 6 小时",
      metrics: { cpu: "23%", memory: "41%", temperature: "46℃" }, configStates: { role: "applied", input: "idle", output: "applied", network: "applied", registration: "applied", sip: "applied" }
    },
    {
      id: "d2", name: "输出盒-D2", type: "output", role: "output", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0052", mac: "AA:BB:CC:00:00:52", ip: "192.168.10.52", fw: "V1.0.0-20261019",
      online: false, lastHeartbeat: "2026-10-19 09:41", registeredAt: "2026-09-12 10:22", lastRegisterAt: "2026-10-19 09:41",
      registerMode: "auto", registerId: "SN202610190002", registerState: "failed", registerMessage: "连接失败（最后心跳超时）",
      network: { mode: "static", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      hardwareProfile: "rk3588-reference-hdmi",
      capabilities: { inputPorts: [{ id: "HDMI1", label: "HDMI 输入 1", kind: "HDMI 2.0 RX", maxWidth: 3840, maxHeight: 2160, maxRefreshHz: 60 }], outputPorts: [{ id: "HDMI-A", label: "HDMI 输出 A", kind: "HDMI 2.1 TX", maxWidth: 7680, maxHeight: 4320, maxRefreshHz: 60 }] },
      output: { width: 3840, height: 2160, refreshHz: 60, mode: "force", connector: "HDMI-A", capability: { maxWidth: 7680, maxHeight: 4320, maxRefreshHz: 60 } },
      input: { port: null, signal: null }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: { code: "NODE_OFFLINE", reason: "节点离线，最后心跳超时", at: "2026-10-19 09:41" },
      bind: "展厅主屏 · 2 格位", lifecycle: "active", uptime: "—", metrics: { cpu: "—", memory: "—", temperature: "—" },
      configStates: { role: "applied", input: "idle", output: "applied", network: "applied", registration: "failed", sip: "idle" }
    },
    {
      id: "e1", name: "输入盒-东1", type: "input", role: "input", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0061", mac: "AA:BB:CC:00:00:61", ip: "192.168.10.61", fw: "V1.0.0-20261019",
      online: true, lastHeartbeat: "2026-10-20 10:02", registeredAt: "2026-09-13 09:02", lastRegisterAt: "2026-10-20 10:02",
      registerMode: "auto", registerId: "SN202610190003", registerState: "registered", registerMessage: "已注册",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      output: null, input: { port: "HDMI1", signal: "有信号 · 1080p60" }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: null, bind: "白名单：展厅主屏", lifecycle: "active", uptime: "26 天 3 小时",
      metrics: { cpu: "18%", memory: "36%", temperature: "43℃" }, configStates: { role: "applied", input: "applied", output: "idle", network: "applied", registration: "applied", sip: "idle" }
    },
    {
      id: "e2", name: "输入盒-东2", type: "input", role: "input", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0062", mac: "AA:BB:CC:00:00:62", ip: "192.168.10.62", fw: "V1.0.0-20261019",
      online: true, lastHeartbeat: "2026-10-20 10:02", registeredAt: "2026-09-13 09:03", lastRegisterAt: "2026-10-20 10:02",
      registerMode: "auto", registerId: "SN202610190004", registerState: "registered", registerMessage: "已注册",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      output: null, input: { port: "HDMI1", signal: "有信号 · 1080p60" }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: null, bind: "白名单：展厅主屏", lifecycle: "active", uptime: "26 天 3 小时",
      metrics: { cpu: "21%", memory: "38%", temperature: "44℃" }, configStates: { role: "applied", input: "applied", output: "idle", network: "applied", registration: "applied", sip: "idle" }
    },
    {
      id: "m1", name: "会议终端-01", type: "input", role: "input", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0063", mac: "AA:BB:CC:00:00:63", ip: "192.168.10.63", fw: "V1.0.0-20261019",
      online: true, lastHeartbeat: "2026-10-20 10:01", registeredAt: "2026-09-13 09:10", lastRegisterAt: "2026-10-20 10:01",
      registerMode: "auto", registerId: "SN202610190005", registerState: "registered", registerMessage: "已注册",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      output: null, input: { port: "HDMI1", signal: "有信号 · 1080p60" }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: null, bind: "白名单：展厅主屏", lifecycle: "active", uptime: "19 天 8 小时",
      metrics: { cpu: "15%", memory: "33%", temperature: "41℃" }, configStates: { role: "applied", input: "applied", output: "idle", network: "applied", registration: "applied", sip: "idle" }
    },
    {
      id: "t3", name: "投屏器-西3", type: "input", role: "input", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0064", mac: "AA:BB:CC:00:00:64", ip: "192.168.10.64", fw: "V1.0.0-20261019",
      online: true, lastHeartbeat: "2026-10-20 10:00", registeredAt: "2026-09-14 14:30", lastRegisterAt: "2026-10-20 10:00",
      registerMode: "auto", registerId: "SN202610190006", registerState: "registered", registerMessage: "已注册",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      output: null, input: { port: "HDMI1", signal: "有信号 · 4K30" }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: null, bind: "未入白名单", lifecycle: "active", uptime: "5 天 2 小时",
      metrics: { cpu: "12%", memory: "29%", temperature: "40℃" }, configStates: { role: "applied", input: "applied", output: "idle", network: "applied", registration: "applied", sip: "idle" }
    },
    {
      id: "p1", name: "播放器-北1", type: "input", role: "input", model: "DVS-NODE-R1", board: "RK-3588",
      sn: "RK3588-2210-0065", mac: "AA:BB:CC:00:00:65", ip: "192.168.10.65", fw: "V1.0.0-20261019",
      online: false, lastHeartbeat: "2026-10-18 22:03", registeredAt: "2026-09-14 14:32", lastRegisterAt: "2026-10-18 22:03",
      registerMode: "auto", registerId: "SN202610190007", registerState: "failed", registerMessage: "连接失败（最后心跳超时）",
      network: { mode: "dhcp", mask: "255.255.255.0", gateway: "192.168.10.1", dns: "192.168.10.1", server: "192.168.10.200:8080" },
      output: null, input: { port: "HDMI1", signal: "—" }, sip: { enabled: false, state: "disabled", server: "", port: 5060, deviceId: "", user: "", lastAt: null },
      currentPlayback: null, exception: { code: "NODE_OFFLINE", reason: "节点离线，最后心跳超时", at: "2026-10-18 22:03" },
      bind: "未入白名单", lifecycle: "active", uptime: "—", metrics: { cpu: "—", memory: "—", temperature: "—" },
      configStates: { role: "applied", input: "idle", output: "idle", network: "applied", registration: "failed", sip: "idle" }
    }
  ];

  /* 平台种子墙已引用 C1–C4；同时保留一台未绑定输出盒供新墙完成端口绑定。 */
  ["C1", "C2", "C3", "C4", "A3"].forEach(function (label, index) {
    var node = JSON.parse(JSON.stringify(seed[0]));
    var serial = 71 + index;
    node.id = label.toLowerCase();
    node.name = "输出盒-" + label;
    node.sn = "RK3588-2210-00" + serial;
    node.mac = "AA:BB:CC:00:00:" + serial;
    node.ip = "192.168.10." + serial;
    node.registerId = "SN2026101900" + serial;
    node.sip.deviceId = node.registerId;
    node.sip.user = "node-" + node.id;
    node.sip.enabled = false;
    node.sip.state = "disabled";
    node.currentPlayback = null;
    node.bind = index < 4 ? "指挥拼接屏 · 1 格位" : "未绑定";
    node.output.width = 1920;
    node.output.height = 1080;
    node.output.mode = "hotplug";
    seed.push(node);
  });

  function clone(value) { return JSON.parse(JSON.stringify(value)); }
  function merge(base, patch) {
    Object.keys(patch || {}).forEach(function (key) {
      if (patch[key] && typeof patch[key] === "object" && !Array.isArray(patch[key])) {
        base[key] = merge(base[key] && typeof base[key] === "object" ? base[key] : {}, patch[key]);
      } else {
        base[key] = patch[key];
      }
    });
    return base;
  }
  function runtimeMessage(node) {
    if (node.runtimeMessage) return node.runtimeMessage;
    if (node.runtimeState === "offline") return "节点离线，等待心跳恢复";
    if (node.runtimeState === "registering") return "正在提交注册请求";
    if (node.runtimeState === "register_failed") return "注册失败，可修改地址后重试";
    if (node.runtimeState === "network_reconfiguring") return "网络参数应用中，节点可能短暂断连";
    if (node.runtimeState === "rebooting") return "节点正在重启，暂时无法访问";
    if (node.runtimeState === "factory_resetting") return "正在恢复出厂，网络设置将保留";
    if (node.runtimeState === "upgrading") return "正在升级，请勿断电或关闭页面";
    if (node.runtimeState === "upgrade_failed") return "升级失败，已保留旧版本，可重试";
    if (node.registerState === "registering") return "正在提交注册请求";
    if (node.registerState === "failed") return node.registerMessage || "注册失败，可修改地址后重试";
    if (node.online === false) return "节点离线，等待心跳恢复";
    return "运行正常";
  }
  function normalizeNode(node) {
    if (!node.hardwareProfile && node.board === "RK-3588") node.hardwareProfile = "rk3588-reference-hdmi";
    /* 旧原型曾写入 120/240Hz 示例值；清理为 RK3588 官方 HDMI 基线，实际能力以后由节点回读覆盖。 */
    if (node.output && node.output.connector === "HDMI-A" && node.output.capability) {
      node.output.capability.maxWidth = Math.min(Number(node.output.capability.maxWidth) || 7680, 7680);
      node.output.capability.maxHeight = Math.min(Number(node.output.capability.maxHeight) || 4320, 4320);
      node.output.capability.maxRefreshHz = Math.min(Number(node.output.capability.maxRefreshHz) || 60, 60);
    }
    if (!node.runtimeState) {
      if (node.online === false) node.runtimeState = "offline";
      else if (node.registerState === "registering") node.runtimeState = "registering";
      else if (node.registerState === "failed") node.runtimeState = "register_failed";
      else node.runtimeState = "online";
    }
    node.runtimeMessage = runtimeMessage(node);
    return node;
  }
  function normalizeAll(nodes) {
    return (nodes || []).map(normalizeNode);
  }
  function read() {
    var raw = null;
    try { raw = global.localStorage.getItem(STORAGE_KEY); } catch (e) { raw = null; }
    if (!raw) {
      var initial = normalizeAll(clone(seed));
      try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(initial)); } catch (e2) { /* no-op */ }
      return initial;
    }
    try {
      var saved = JSON.parse(raw);
      if (!Array.isArray(saved)) return normalizeAll(clone(seed));
      var added = false;
      seed.slice(7).forEach(function (item) {
        if (!saved.some(function (node) { return node.id === item.id; })) {
          saved.push(clone(item));
          added = true;
        }
      });
      if (added) try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(saved)); } catch (e3) { /* in-memory fallback */ }
      return normalizeAll(saved);
    } catch (e4) { return normalizeAll(clone(seed)); }
  }
  function write(nodes) {
    try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(nodes)); } catch (e) { /* no-op */ }
    try { global.dispatchEvent(new CustomEvent("dvs-node-state", { detail: nodes })); } catch (e2) { /* no-op */ }
    return nodes;
  }
  function now() { return new Date().toISOString(); }
  function update(id, patch) {
    var nodes = read();
    var node = nodes.filter(function (item) { return item.id === id; })[0];
    if (!node) return null;
    merge(node, patch || {});
    node.lastUpdatedAt = now();
    write(nodes);
    return clone(node);
  }
  function heartbeat(id, patch) {
    var data = merge({ online: true, lastHeartbeat: now() }, patch || {});
    return update(id, data);
  }
  function reset() { return write(normalizeAll(clone(seed))); }

  global.DvsNodeState = {
    source: SOURCE,
    storageKey: STORAGE_KEY,
    list: function () { return clone(read()); },
    get: function (id) { var node = read().filter(function (item) { return item.id === id; })[0]; return node ? clone(node) : null; },
    update: update,
    heartbeat: heartbeat,
    reset: reset,
    runtimeStates: RUNTIME_STATES,
    subscribe: function (handler) {
      var listener = function (event) { handler(event.detail || read()); };
      global.addEventListener("dvs-node-state", listener);
      global.addEventListener("storage", function (event) { if (event.key === STORAGE_KEY) listener({ detail: read() }); });
      return function () { global.removeEventListener("dvs-node-state", listener); };
    }
  };
})(window);
