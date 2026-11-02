/*
 * DVS mock API adapter（原型用，不是生产服务）。
 *
 * 目的：让平台页和节点页使用同一套“请求/响应”形状，避免页面直接
 * 拼装示例数据。当前适配 localStorage 中的 DvsNodeState 与
 * DvsPlaybackTasks；替换为真实服务时，页面只需要替换本适配层。
 * 契约依据：05-tech-links/apis.md、state-machines.md、deletion-rules.md。
 */
(function (global) {
  "use strict";

  var AUDIT_KEY = "dvs.audit.events.v1";
  var IDEMPOTENCY_KEY = "dvs.mock.idempotency.v1";
  var VERSION = "mock-v1";

  function clone(value) {
    return value == null ? value : JSON.parse(JSON.stringify(value));
  }

  function requestId() {
    return "mock-req-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 7);
  }

  function ok(data, msg) {
    return { ok: true, status: 200, code: "OK", msg: msg || "", data: clone(data), requestId: requestId() };
  }

  function fail(code, status, msg, data) {
    return { ok: false, status: status || 400, code: code, msg: msg || code, data: clone(data || {}), requestId: requestId() };
  }

  function readAudit() {
    var raw = null;
    try { raw = global.localStorage.getItem(AUDIT_KEY); } catch (e) { raw = null; }
    if (!raw) return [];
    try { return JSON.parse(raw) || []; } catch (e2) { return []; }
  }

  function writeAudit(event) {
    var events = readAudit();
    events.unshift(event);
    try { global.localStorage.setItem(AUDIT_KEY, JSON.stringify(events.slice(0, 100))); } catch (e) { /* 原型环境不可写时忽略 */ }
  }

  function readIdempotency() {
    var raw = null;
    try { raw = global.localStorage.getItem(IDEMPOTENCY_KEY); } catch (e) { raw = null; }
    if (!raw) return {};
    try { return JSON.parse(raw) || {}; } catch (e2) { return {}; }
  }

  function writeIdempotency(store) {
    try { global.localStorage.setItem(IDEMPOTENCY_KEY, JSON.stringify(store)); } catch (e) { /* no-op */ }
  }

  function audit(type, objectType, objectId, result, reason, extra) {
    writeAudit({
      id: "AUD-" + Date.now().toString(36),
      eventType: type,
      objectType: objectType,
      objectId: objectId || null,
      result: result || "success",
      reason: reason || null,
      occurredAt: new Date().toISOString(),
      metadata: extra || {}
    });
  }

  function parsePath(path) {
    var raw = String(path || "");
    var parts = raw.split("?");
    var pathname = parts[0].replace(/\/$/, "") || "/";
    var query = {};
    (parts[1] || "").split("&").forEach(function (item) {
      if (!item) return;
      var pair = item.split("=");
      query[decodeURIComponent(pair[0])] = decodeURIComponent(pair.slice(1).join("=") || "");
    });
    return { pathname: pathname, query: query };
  }

  function nodeApiReady() {
    return global.DvsNodeState && typeof global.DvsNodeState.list === "function";
  }

  function taskApiReady() {
    return global.DvsPlaybackTasks && typeof global.DvsPlaybackTasks.list === "function";
  }

  function nodeInfo(nodeId) {
    if (!nodeApiReady()) return null;
    return global.DvsNodeState.get(nodeId || global.DvsProtoNodeId || "d1");
  }

  function nodeConfig(node, scope) {
    if (!node) return null;
    var state = node.configStates && node.configStates[scope] || "idle";
    var value = scope === "role" ? { role: node.role } : scope === "input" ? node.input : scope === "output" ? node.output : scope === "network" ? node.network : scope === "registration" ? { mode: node.registerMode, server: node.network && node.network.server } : scope === "sip" ? node.sip : {};
    return {
      nodeId: node.id,
      scope: scope,
      desiredRevision: node.configRevision && node.configRevision[scope] && node.configRevision[scope].desired || 1,
      appliedRevision: node.configRevision && node.configRevision[scope] && node.configRevision[scope].applied || (state === "applied" ? 1 : 0),
      state: state,
      desiredValue: clone(value),
      appliedValue: clone(value),
      operationId: node.configOperationId || null,
      errorCode: node.configErrorCode || null,
      errorReason: node.configErrorReason || null,
      updatedAt: node.lastUpdatedAt || node.lastHeartbeat || null
    };
  }

  function taskResult(result) {
    if (result && result.ok) return ok(result.task || result, "操作已受理");
    return fail("VALIDATION_ERROR", 422, result && result.reason || "操作未完成", result && result.task ? { task: result.task } : {});
  }

  function handleTask(method, pathname, query, body) {
    if (!taskApiReady()) return fail("SERVICE_NOT_READY", 503, "播放任务 mock 服务尚未加载");
    var match = pathname.match(/^\/api\/v1\/playback-tasks\/([^/]+)(?:\/([^/]+))?$/);
    if (method === "GET" && pathname === "/api/v1/playback-tasks") {
      return ok(global.DvsPlaybackTasks.list(query.nodeId ? { nodeId: query.nodeId } : null));
    }
    if (method === "POST" && pathname === "/api/v1/playback-tasks") {
      var created = global.DvsPlaybackTasks.create({
        name: body.name,
        sceneId: body.sceneId,
        sceneName: body.sceneName,
        wallId: body.wallId,
        wallName: body.wallName,
        nodeIds: body.targetNodeIds || body.nodeIds || [],
        priority: null,
        playMode: "immediate",
        scheduleAt: null,
        expiresAt: body.expiresAt || null
      });
      if (!created.ok) return fail("VALIDATION_ERROR", 422, created.reason);
      audit("playback_task_created", "playback_task", created.task.id, "success");
      return ok(created.task, "任务已创建，等待检查");
    }
    if (!match) return fail("NOT_FOUND", 404, "播放任务不存在");
    var taskId = decodeURIComponent(match[1]);
    var action = match[2] || "";
    if (method === "GET" && !action) {
      var task = global.DvsPlaybackTasks.get(taskId);
      return task ? ok(task) : fail("NOT_FOUND", 404, "播放任务不存在");
    }
    if (method !== "POST") return fail("INVALID_ARGUMENT", 400, "该任务操作只支持 POST");
    var result;
    if (action === "check") result = global.DvsPlaybackTasks.check(taskId);
    else if (action === "start") result = global.DvsPlaybackTasks.start(taskId);
    else if (action === "finish") result = global.DvsPlaybackTasks.finish(taskId);
    else if (action === "cancel") result = global.DvsPlaybackTasks.cancel(taskId);
    else if (action === "retry") result = global.DvsPlaybackTasks.retry(taskId);
    else return fail("NOT_FOUND", 404, "未定义的任务操作");
    if (result && result.ok) audit("playback_task_" + action, "playback_task", taskId, "success");
    else audit("playback_task_" + action, "playback_task", taskId, "failed", result && result.reason);
    return taskResult(result);
  }

  function dispatch(method, path, body, options) {
    method = String(method || "GET").toUpperCase();
    body = body || {};
    options = options || {};
    var parsed = parsePath(path);
    var pathname = parsed.pathname;
    var query = parsed.query;
    if (pathname.indexOf("/api/v1/playback-tasks") === 0) return handleTask(method, pathname, query, body);

    if (method === "GET" && pathname === "/api/v1/nodes") {
      if (!nodeApiReady()) return fail("SERVICE_NOT_READY", 503, "节点 mock 服务尚未加载");
      var nodes = global.DvsNodeState.list();
      if (query.role) nodes = nodes.filter(function (node) { return node.role === query.role; });
      if (query.online) nodes = nodes.filter(function (node) { return String(node.online) === query.online; });
      return ok({ items: nodes, page: Number(query.page || 1), pageSize: Number(query.pageSize || nodes.length || 20), total: nodes.length });
    }

    if (method === "POST" && pathname === "/api/v1/nodes/upgrade") {
      if (!nodeApiReady()) return fail("SERVICE_NOT_READY", 503, "节点 mock 服务尚未加载");
      var upgradeIds = body.targetNodeIds || body.nodeIds || [];
      if (!upgradeIds.length) return fail("INVALID_ARGUMENT", 400, "未选择升级节点");
      upgradeIds.forEach(function (id) {
        global.DvsNodeState.update(id, { fw: body.fwVersion || "V1.0.1", runtimeState: "online", runtimeMessage: "运行正常", exception: null });
        audit("node_upgrade", "node", id, "success", null, { version: body.fwVersion || "V1.0.1" });
      });
      return ok({ nodeIds: upgradeIds, status: "applied" }, "升级结果已写入 mock 状态");
    }
    if (method === "POST" && pathname === "/api/v1/nodes/heartbeat") {
      if (!nodeApiReady()) return fail("SERVICE_NOT_READY", 503, "节点 mock 服务尚未加载");
      var heartbeatId = body.nodeId || body.id;
      var heartbeat = global.DvsNodeState.heartbeat(heartbeatId, body.patch || body);
      if (!heartbeat) return fail("NOT_FOUND", 404, "节点不存在");
      audit("node_heartbeat", "node", heartbeatId, "success");
      return ok(heartbeat, "心跳已接收");
    }

    var nodeMatch = pathname.match(/^\/api\/v1\/nodes\/([^/]+)(?:\/([^/]+))?$/);
    if (nodeMatch) {
      var nodeId = decodeURIComponent(nodeMatch[1]);
      var nodeAction = nodeMatch[2] || "";
      if (method === "GET" && !nodeAction) {
        var node = nodeInfo(nodeId);
        return node ? ok(node) : fail("NOT_FOUND", 404, "节点不存在");
      }
      if (method === "GET" && nodeAction === "current-task") {
        if (!taskApiReady()) return fail("SERVICE_NOT_READY", 503, "播放任务 mock 服务尚未加载");
        return ok(global.DvsPlaybackTasks.currentForNode(nodeId));
      }
      if (method === "PUT" && !nodeAction) {
        var renamed = global.DvsNodeState.update(nodeId, { name: String(body.name || "").trim() });
        if (!renamed) return fail("NOT_FOUND", 404, "节点不存在");
        audit("node_renamed", "node", nodeId, "success");
        return ok(renamed, "节点别名已更新");
      }
      if (method === "POST" && nodeAction === "retire") {
        var retiring = nodeInfo(nodeId);
        if (!retiring) return fail("NOT_FOUND", 404, "节点不存在");
        var lifecycle = retiring.online ? "retired" : "retire_pending";
        var retired = global.DvsNodeState.update(nodeId, {
          lifecycle: lifecycle,
          online: false,
          bind: "—",
          exception: lifecycle === "retire_pending" ? { code: "RETIRE_PENDING", reason: "节点离线，退役待执行", at: new Date().toISOString() } : null
        });
        audit("node_retire", "node", nodeId, "success", null, { lifecycle: lifecycle });
        return ok(retired, lifecycle === "retired" ? "退役完成" : "已进入退役待执行");
      }
      if (method === "POST" && nodeAction === "heartbeat") {
        if (!nodeApiReady()) return fail("SERVICE_NOT_READY", 503, "节点 mock 服务尚未加载");
        var updated = global.DvsNodeState.heartbeat(nodeId, body.patch || body);
        if (!updated) return fail("NOT_FOUND", 404, "节点不存在");
        audit("node_heartbeat", "node", nodeId, "success");
        return ok(updated, "心跳已接收");
      }
    }

    var configMatch = pathname.match(/^\/api\/v1\/node\/config\/([^/]+)(?:\/([^/]+))?$/);
    if (configMatch) {
      var configScope = decodeURIComponent(configMatch[1]);
      var configAction = configMatch[2] || "";
      var configNode = nodeInfo(options.nodeId || body.nodeId);
      if (!configNode) return fail("NOT_FOUND", 404, "节点不存在");
      if (["role", "input", "output", "network", "registration", "sip"].indexOf(configScope) < 0) return fail("INVALID_ARGUMENT", 400, "不支持的配置范围");
      if (method === "GET" && !configAction) return ok(nodeConfig(configNode, configScope));
      if (method === "POST" && configAction === "retry") {
        var configPatch = { configStates: {} };
        configPatch.configStates[configScope] = "pending_apply";
        var retried = global.DvsNodeState.update(configNode.id, configPatch);
        audit("node_config_retry", "node_config", configNode.id, "success", null, { scope: configScope });
        return ok(nodeConfig(retried, configScope), "配置已重新提交，等待应用");
      }
      return fail("NOT_FOUND", 404, "未定义的节点配置操作");
    }

    if (method === "POST" && pathname === "/api/v1/node/login") {
      if (body.username === "admin" && body.password === "admin123") return ok({ session: "mock-session", expiresIn: 3600 }, "登录成功");
      return fail("UNAUTHORIZED", 401, "账号或密码错误");
    }
    if (method === "GET" && pathname === "/api/v1/node/info") {
      var info = nodeInfo(options.nodeId || body.nodeId);
      return info ? ok(info) : fail("NOT_FOUND", 404, "节点不存在");
    }
    if (method === "GET" && pathname === "/api/v1/node/role") {
      var roleNode = nodeInfo(options.nodeId || body.nodeId);
      return roleNode ? ok({ role: roleNode.role, pendingRole: roleNode.pendingRole || null, revision: roleNode.revision || 1 }) : fail("NOT_FOUND", 404, "节点不存在");
    }
    if (method === "GET" && pathname === "/api/v1/audit-events") {
      return ok({ items: readAudit(), page: 1, pageSize: 100, total: readAudit().length });
    }
    return fail("NOT_FOUND", 404, "未定义的 mock API：" + method + " " + pathname);
  }

  function request(method, path, body, options) {
    options = options || {};
    var headers = options.headers || {};
    var idem = options.idempotencyKey || headers["Idempotency-Key"] || headers["idempotency-key"] || "";
    var key = idem ? String(method || "GET").toUpperCase() + " " + String(path || "") + " #" + idem : "";
    if (key) {
      var store = readIdempotency();
      if (store[key]) return clone(store[key]);
      var first = dispatch(method, path, body, options);
      if (first && first.status < 500) {
        store[key] = first;
        writeIdempotency(store);
      }
      return first;
    }
    return dispatch(method, path, body, options);
  }

  function reset() {
    if (global.DvsNodeState && typeof global.DvsNodeState.reset === "function") global.DvsNodeState.reset();
    if (global.DvsPlaybackTasks && typeof global.DvsPlaybackTasks.reset === "function") global.DvsPlaybackTasks.reset();
    try { global.localStorage.removeItem(AUDIT_KEY); } catch (e) { /* no-op */ }
    try { global.localStorage.removeItem(IDEMPOTENCY_KEY); } catch (e2) { /* no-op */ }
    return ok({ reset: true }, "mock 数据已恢复默认");
  }

  global.DvsMockApi = {
    version: VERSION,
    source: "localStorage mock adapter",
    request: request,
    requestAsync: function (method, path, body, options) { return Promise.resolve(request(method, path, body, options)); },
    auditEvents: function () { return clone(readAudit()); },
    reset: reset
  };
})(window);
