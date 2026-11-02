/*
 * DVS 播放任务闭环（原型用）。
 *
 * 当前播放、任务列表和播放记录必须从同一份任务数据读取。
 * 生产实现对应控制服务的 /api/v1/playback-tasks；节点 currentPlayback
 * 只是该任务在节点上的运行态投影，不应再由页面单独维护。
 */
(function (global) {
  "use strict";

  var STORAGE_KEY = "dvs.playback.tasks.v1";
  var EVENT_NAME = "dvs-playback-tasks";
  var SOURCE = "控制服务播放任务";
  var STATUS_LABELS = {
    pending_check: "待检查",
    pending_execution: "待执行",
    executing: "执行中",
    playing: "播放中",
    success: "成功",
    failed: "失败",
    cancelled: "取消",
    expired: "过期"
  };
  var TERMINAL = { success: true, failed: true, cancelled: true, expired: true };
  var ALLOWED = {
    pending_check: { pending_execution: true, failed: true, cancelled: true, expired: true },
    pending_execution: { executing: true, cancelled: true, expired: true },
    executing: { playing: true, failed: true, cancelled: true, expired: true },
    playing: { success: true, failed: true, cancelled: true, expired: true }
  };
  var seed = [
    {
      id: "PUB-20261010-018", name: "午间主题", sceneId: "SCN-20261010-017", sceneName: "午间主题",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "playing",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: "2026-10-20 10:05",
      finishedAt: null, expiresAt: null, errorCode: null, errorReason: null, createdAt: "2026-10-20 09:58"
    },
    {
      id: "PUB-20261010-021", name: "前台欢迎", sceneId: "SCN-20261010-019", sceneName: "前台欢迎",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "pending_execution",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: null,
      finishedAt: null, expiresAt: "2026-10-20 23:59", errorCode: null, errorReason: null, createdAt: "2026-10-20 09:40"
    },
    {
      id: "PUB-20261010-022", name: "会议模式", sceneId: "SCN-20261010-020", sceneName: "会议模式",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "pending_check",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: null,
      finishedAt: null, expiresAt: "2026-10-21 12:00", errorCode: null, errorReason: null, createdAt: "2026-10-20 09:20"
    },
    {
      id: "PUB-20261010-017", name: "开场欢迎", sceneId: "SCN-20261010-016", sceneName: "开场欢迎",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "success",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: "2026-10-20 08:30",
      finishedAt: "2026-10-20 09:00", expiresAt: null, errorCode: null, errorReason: null, createdAt: "2026-10-20 08:20"
    },
    {
      id: "PUB-20261010-016", name: "会议回放", sceneId: "SCN-20261010-015", sceneName: "会议回放",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "failed",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: "2026-10-19 15:00",
      finishedAt: "2026-10-19 15:01", expiresAt: null, errorCode: "MEDIA_UNAVAILABLE", errorReason: "素材流不可用",
      createdAt: "2026-10-19 14:50"
    },
    {
      id: "PUB-20261010-015", name: "晚间信息", sceneId: "SCN-20261010-014", sceneName: "晚间信息",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "cancelled",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: null,
      finishedAt: "2026-10-19 17:30", expiresAt: null, errorCode: "USER_CANCELLED", errorReason: "操作员取消",
      createdAt: "2026-10-19 16:30"
    },
    {
      id: "PUB-20261010-014", name: "临时通知", sceneId: "SCN-20261010-013", sceneName: "临时通知",
      wallId: "wall-main", wallName: "展厅主屏", nodeIds: ["d1"], status: "expired",
      priority: null, playMode: "immediate", scheduleAt: null, startedAt: null,
      finishedAt: "2026-10-19 12:00", expiresAt: "2026-10-19 12:00", errorCode: "TASK_EXPIRED", errorReason: "超过结束时间",
      createdAt: "2026-10-19 09:00"
    }
  ];

  function clone(value) { return JSON.parse(JSON.stringify(value)); }
  function read() {
    var raw = null;
    try { raw = global.localStorage.getItem(STORAGE_KEY); } catch (e) { raw = null; }
    if (!raw) {
      var initial = clone(seed);
      try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(initial)); } catch (e2) { /* no-op */ }
      return initial;
    }
    try {
      var stored = JSON.parse(raw);
      // 原型页需要稳定展示播放任务；空值、旧格式或损坏行不应让任务区报错/清空。
      if (!Array.isArray(stored) || !stored.length) return clone(seed);
      var valid = stored.filter(function (task) { return task && typeof task === "object" && !Array.isArray(task); });
      return valid.length ? valid : clone(seed);
    } catch (e3) { return clone(seed); }
  }
  function write(tasks) {
    try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(tasks)); } catch (e) { /* no-op */ }
    try { global.dispatchEvent(new CustomEvent(EVENT_NAME, { detail: tasks })); } catch (e2) { /* no-op */ }
    return tasks;
  }
  function nowLabel() {
    var d = new Date();
    var pad = function (n) { return String(n).padStart(2, "0"); };
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
  }
  function targetNodes(task) {
    if (!global.DvsNodeState || typeof global.DvsNodeState.get !== "function") return [];
    return (task.nodeIds || []).map(function (id) { return global.DvsNodeState.get(id); }).filter(Boolean);
  }
  function syncNodeProjection(task, oldStatus) {
    if (!global.DvsNodeState || typeof global.DvsNodeState.update !== "function") return;
    (task.nodeIds || []).forEach(function (nodeId) {
      var current = global.DvsNodeState.get(nodeId);
      if (!current) return;
      var playing = task.status === "playing";
      var currentTask = current.currentPlayback;
      if (playing) {
        global.DvsNodeState.update(nodeId, {
          currentPlayback: { taskId: task.id, name: task.name, status: "playing", startedAt: task.startedAt || nowLabel(), wall: task.wallName }
        });
      } else if (currentTask && currentTask.taskId === task.id) {
        global.DvsNodeState.update(nodeId, { currentPlayback: null });
      } else if (oldStatus === "playing" && currentTask && currentTask.taskId === task.id) {
        global.DvsNodeState.update(nodeId, { currentPlayback: null });
      }
    });
  }
  function allNodesReady(task) {
    var nodes = targetNodes(task);
    return nodes.length === (task.nodeIds || []).length && nodes.every(function (node) {
      return node.online !== false && node.registerState === "registered" && node.lifecycle === "active";
    });
  }
  function getTask(id) {
    var task = read().filter(function (item) { return item.id === id; })[0];
    return task ? clone(task) : null;
  }
  function update(id, patch) {
    var tasks = read();
    var index = tasks.findIndex(function (item) { return item.id === id; });
    if (index < 0) return null;
    var old = clone(tasks[index]);
    Object.keys(patch || {}).forEach(function (key) { tasks[index][key] = patch[key]; });
    tasks[index].updatedAt = nowLabel();
    write(tasks);
    syncNodeProjection(tasks[index], old.status);
    return clone(tasks[index]);
  }
  function create(input) {
    input = input || {};
    var name = String(input.name || "").trim();
    var sceneId = String(input.sceneId || "").trim();
    var sceneName = String(input.sceneName || "").trim();
    var wallId = String(input.wallId || "").trim();
    var wallName = String(input.wallName || "").trim();
    var nodeIds = Array.isArray(input.nodeIds) ? input.nodeIds.filter(Boolean) : [];
    if (!name) return { ok: false, reason: "请输入任务名称" };
    if (!sceneId || !sceneName) return { ok: false, reason: "请选择场景" };
    if (!wallId || !wallName || !nodeIds.length) return { ok: false, reason: "缺少目标拼墙或节点" };
    var stamp = new Date();
    var pad = function (n) { return String(n).padStart(2, "0"); };
    var date = stamp.getFullYear() + pad(stamp.getMonth() + 1) + pad(stamp.getDate());
    var suffix = pad(stamp.getHours()) + pad(stamp.getMinutes()) + pad(stamp.getSeconds());
    var id = "PUB-" + date + "-" + suffix;
    var tasks = read();
    var seq = 1;
    while (tasks.some(function (item) { return item.id === id; })) id = "PUB-" + date + "-" + suffix + "-" + (seq++);
    var task = {
      id: id,
      name: name,
      sceneId: sceneId,
      sceneName: sceneName,
      wallId: wallId,
      wallName: wallName,
      nodeIds: nodeIds,
      status: "pending_check",
      priority: null,
      playMode: "immediate",
      scheduleAt: null,
      startedAt: null,
      finishedAt: null,
      expiresAt: input.expiresAt || null,
      errorCode: null,
      errorReason: null,
      createdAt: nowLabel()
    };
    tasks.unshift(task);
    write(tasks);
    return { ok: true, task: clone(task) };
  }
  function transition(id, nextStatus, patch) {
    var task = getTask(id);
    if (!task) return { ok: false, reason: "任务不存在" };
    if (task.status === nextStatus) return { ok: true, task: task };
    if (!ALLOWED[task.status] || !ALLOWED[task.status][nextStatus]) {
      return { ok: false, reason: STATUS_LABELS[task.status] + "不能转为" + STATUS_LABELS[nextStatus] };
    }
    if (nextStatus === "pending_execution" && !allNodesReady(task)) {
      return update(id, { status: "failed", finishedAt: nowLabel(), errorCode: "NODE_UNAVAILABLE", errorReason: "目标节点离线或未注册" })
        ? { ok: false, reason: "目标节点离线或未注册，任务已失败", task: getTask(id) } : { ok: false, reason: "目标节点离线或未注册" };
    }
    if ((nextStatus === "executing" || nextStatus === "playing") && !allNodesReady(task)) {
      return update(id, { status: "failed", finishedAt: nowLabel(), errorCode: "NODE_UNAVAILABLE", errorReason: "目标节点离线或未注册" })
        ? { ok: false, reason: "目标节点离线或未注册，任务已失败", task: getTask(id) } : { ok: false, reason: "目标节点离线或未注册" };
    }
    if (nextStatus === "playing") {
      var conflict = list({ nodeId: task.nodeIds && task.nodeIds[0] }).filter(function (item) {
        return item.id !== task.id && item.status === "playing" && (task.nodeIds || []).some(function (nodeId) {
          return (item.nodeIds || []).indexOf(nodeId) >= 0;
        });
      })[0];
      if (conflict) return { ok: false, reason: "节点已有播放中任务：" + conflict.id, task: task };
    }
    var data = Object.assign({}, patch || {}, { status: nextStatus });
    if (nextStatus === "executing") data.startedAt = data.startedAt || nowLabel();
    if (TERMINAL[nextStatus]) data.finishedAt = data.finishedAt || nowLabel();
    var result = update(id, data);
    return { ok: !!result, task: result };
  }
  function check(id) { return transition(id, "pending_execution"); }
  function start(id) {
    var first = transition(id, "executing");
    if (!first.ok) return first;
    global.setTimeout(function () {
      var current = getTask(id);
      if (current && current.status === "executing") {
        var result = transition(id, "playing", { startedAt: current.startedAt || nowLabel() });
        if (!result.ok) update(id, { status: "failed", finishedAt: nowLabel(), errorCode: "TASK_CONFLICT", errorReason: result.reason || "节点已有播放中任务" });
      }
    }, 450);
    return first;
  }
  function finish(id) { return transition(id, "success"); }
  function cancel(id) { return transition(id, "cancelled", { errorCode: "USER_CANCELLED", errorReason: "操作员取消" }); }
  function retry(id) {
    var task = getTask(id);
    if (!task || (task.status !== "failed" && task.status !== "expired" && task.status !== "cancelled")) return { ok: false, reason: "当前状态不可重试" };
    return update(id, { status: "pending_execution", errorCode: null, errorReason: null, finishedAt: null, startedAt: null });
  }
  function list(filter) {
    var tasks = read();
    if (filter && filter.nodeId) tasks = tasks.filter(function (task) {
      return Array.isArray(task.nodeIds) && task.nodeIds.indexOf(filter.nodeId) >= 0;
    });
    if (filter && filter.status) tasks = tasks.filter(function (task) { return task.status === filter.status; });
    return clone(tasks);
  }
  function currentForNode(nodeId) {
    var current = list({ nodeId: nodeId }).filter(function (task) { return task.status === "playing"; });
    current.sort(function (a, b) { return String(b.startedAt || "").localeCompare(String(a.startedAt || "")); });
    return current[0] || null;
  }
  function reset() {
    var tasks = write(clone(seed));
    clone(seed).filter(function (task) { return task.status === "playing"; }).forEach(function (task) { syncNodeProjection(task, null); });
    return tasks;
  }
  function restorePrototypeTasksForNode(nodeId) {
    if (!nodeId) return false;
    var tasks = read();
    var byId = {};
    tasks.forEach(function (task) { if (task.id) byId[task.id] = task; });

    // 每次进入节点后台都按任务 ID 幂等补齐演示数据；旧版“该节点已有任意任务”
    // 的判断会漏掉部分数据，且一次性标记会让修复后续无法重试。
    var changed = false;
    var restored = [];
    clone(seed).filter(function (task) { return task.nodeIds.indexOf(nodeId) >= 0; }).forEach(function (sample) {
      var existing = byId[sample.id];
      if (!existing) {
        tasks.push(sample);
        byId[sample.id] = sample;
        restored.push(sample);
        changed = true;
        return;
      }
      if (!Array.isArray(existing.nodeIds)) {
        existing.nodeIds = [];
        changed = true;
      }
      if (existing.nodeIds.indexOf(nodeId) < 0) {
        existing.nodeIds.push(nodeId);
        changed = true;
      }
      restored.push(existing);
    });
    if (changed) write(tasks);
    restored.filter(function (task) { return task.status === "playing"; }).forEach(function (task) { syncNodeProjection(task, null); });
    return changed;
  }

  global.DvsPlaybackTasks = {
    source: SOURCE,
    storageKey: STORAGE_KEY,
    statuses: STATUS_LABELS,
    terminal: TERMINAL,
    list: list,
    get: getTask,
    currentForNode: currentForNode,
    transition: transition,
    check: check,
    start: start,
    finish: finish,
    cancel: cancel,
    retry: retry,
    update: update,
    create: create,
    reset: reset,
    restorePrototypeTasksForNode: restorePrototypeTasksForNode,
    subscribe: function (handler) {
      var listener = function (event) { handler(event.detail || read()); };
      global.addEventListener(EVENT_NAME, listener);
      global.addEventListener("storage", function (event) { if (event.key === STORAGE_KEY) listener({ detail: read() }); });
      return function () { global.removeEventListener(EVENT_NAME, listener); };
    }
  };
  /* 首次加载也把任务真源投影到节点运行态，避免旧页面快照继续冒充当前播放。 */
  if (global.DvsNodeState && typeof global.DvsNodeState.list === "function") {
    global.DvsNodeState.list().forEach(function (node) {
      var current = currentForNode(node.id);
      if (current) syncNodeProjection(current, null);
      else if (node.currentPlayback) global.DvsNodeState.update(node.id, { currentPlayback: null });
    });
  }
})(window);
