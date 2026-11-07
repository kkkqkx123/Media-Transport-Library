/* Shared, persistent platform state for the DVS prototype. */
(function (global) {
  "use strict";

  /* v2 drops the pre-unification device scheme: whitelist entries are keyed
   * by signal id (s*), the same ids scenes and windows reference. v1 state
   * is intentionally not migrated (no backward compatibility). */
  var STORAGE_KEY = "dvs.platform.state.v2";
  var listeners = [];

  function copy(value) { return value === undefined ? undefined : JSON.parse(JSON.stringify(value)); }
  function sameId(a, b) { return String(a) === String(b); }
  function nameKey(value) { return String(value || "").trim().toLocaleLowerCase(); }
  function stamp() {
    var d = new Date();
    function pad(n) { return String(n).padStart(2, "0"); }
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
  }
  function uniqueId(prefix) { return prefix + "-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 7); }
  function emptyBindings(rows, cols) {
    return Array.from({ length: rows }, function () { return Array.from({ length: cols }, function () { return null; }); });
  }
  function defaultSettings() {
    return {
      colorFormat: "4:2:0 转 4:4:4", quality: "中", buffer: 4,
      timeServerMode: "auto", timeServerAddress: "", syncMax: 120, syncMin: 60, syncStep: 5,
      image: { brightness: 50, contrast: 50, saturation: 50, hue: 180 }
    };
  }
  function baseWall(id, groupId, name, mode, type, rows, cols, res, createdAt) {
    return {
      id: id, groupId: groupId, name: name, mode: mode, type: type,
      rows: rows, cols: cols, res: res, createdAt: createdAt,
      firstRes: "1920×1080", rowPixels: Array.from({ length: rows }, function () { return 1080; }),
      colPixels: Array.from({ length: cols }, function () { return 1920; }),
      bindings: emptyBindings(rows, cols), whitelist: [], settings: defaultSettings(),
      backdrop: null, scenes: [{ id: "scene-" + id + "-default", name: "默认场景", isDefault: true, volume: 80, layout: [] }],
      currentSceneId: "scene-" + id + "-default", windows: [], operations: 0, layoutMode: "free", volume: 80,
      deletedAt: null
    };
  }
  function seed() {
    var main = baseWall("wall-main", "g1", "展厅主屏", "LED", "", 2, 4, "7680×2160（逐行逐列）", "2026-10-12 09:45");
    main.bindings = [
      [{ node: "D1", port: 1 }, { node: "D1", port: 2 }, { node: "D1", port: 3 }, { node: "D1", port: 4 }],
      [{ node: "D2", port: 1 }, { node: "D2", port: 2 }, null, null]
    ];
    main.whitelist = [
      { id: "s1", name: "输入盒-东1", ip: "192.168.10.61", transMode: "单播" },
      { id: "s2", name: "输入盒-东2", ip: "192.168.10.62", transMode: "组播" },
      { id: "s3", name: "会议终端-01", ip: "192.168.10.63", transMode: "单播" }
    ];
    main.scenes = [
      { id: "scene-wall-main-default", name: "默认场景", isDefault: true, volume: 80, layout: [{ sig: "s1", c: 0, r: 0, cs: 2, rs: 1 }, { sig: "s3", c: 0, r: 1, cs: 1, rs: 1 }] },
      { id: "scene-wall-main-meeting", name: "会议模式", isDefault: false, volume: 80, layout: [{ sig: "s3", c: 0, r: 0, cs: 4, rs: 1 }] },
      { id: "scene-wall-main-presentation", name: "宣讲模式", isDefault: false, volume: 60, layout: [{ sig: "s1", c: 0, r: 0, cs: 1, rs: 1 }, { sig: "s2", c: 2, r: 0, cs: 1, rs: 1 }, { sig: "s3", c: 1, r: 1, cs: 1, rs: 1 }] }
    ];
    main.windows = [
      { id: "w1", sig: "s1", x: 0, y: 0, w: 260, h: 90, z: 1, muted: false },
      { id: "w2", sig: "s3", x: 0, y: 90, w: 130, h: 90, z: 2, muted: true },
      { id: "w3", sig: "s2", x: 260, y: 0, w: 130, h: 90, z: 3, muted: true }
    ];
    main.rowPixels = [1080, 1080];

    var meeting = baseWall("wall-meeting", "g1", "会议室横屏", "LCD", "标准", 1, 2, "3840×1080", "2026-10-11 15:22");
    meeting.scenes[0].layout = [];
    var command = baseWall("wall-command", "g1", "指挥拼接屏", "LCD", "非标准", 2, 2, "3840×2160（逐行逐列）", "2026-10-10 13:15");
    command.bindings = [[{ node: "C1", port: 1 }, { node: "C2", port: 1 }], [{ node: "C3", port: 1 }, { node: "C4", port: 1 }]];
    command.scenes.push({ id: "scene-wall-command-2", name: "指挥模式", isDefault: false, volume: 80, layout: [] });
    var report = baseWall("wall-report", "g2", "报告厅主屏", "LCD", "标准", 1, 2, "3840×1080", "2026-10-11 15:21");
    var commandMain = baseWall("wall-command-main", "g4", "指挥中心主屏", "LED", "", 2, 3, "5760×2160（逐行逐列）", "2026-10-08 16:35");
    var commandBackup = baseWall("wall-command-backup", "g4", "指挥中心备屏", "LCD", "标准", 1, 2, "3840×1080", "2026-10-08 16:34");
    return {
      version: 2,
      groups: [
        { id: "g1", name: "展厅 A 区", createdAt: "2026-10-12 09:41" },
        { id: "g2", name: "会议报告厅", createdAt: "2026-10-11 15:20" },
        { id: "g3", name: "运维测试组", createdAt: "2026-10-09 11:05" },
        { id: "g4", name: "指挥中心", createdAt: "2026-10-08 16:32" },
        { id: "g5", name: "备用空组", createdAt: "2026-09-30 10:00" }
      ],
      walls: [main, meeting, command, report, commandMain, commandBackup],
      currentGroupId: "g1", currentWallId: "wall-main"
    };
  }

  function fromStorage() {
    try {
      var raw = global.localStorage.getItem(STORAGE_KEY);
      if (raw) {
        var parsed = JSON.parse(raw);
        if (parsed && parsed.version === 2 && Array.isArray(parsed.groups) && Array.isArray(parsed.walls)) return parsed;
      }
    } catch (e) { /* Storage may be unavailable in a local file preview. */ }
    return null;
  }
  var state = fromStorage() || seed();
  function notify() {
    listeners.slice().forEach(function (listener) { try { listener(getState()); } catch (e) { /* keep other subscribers running */ } });
  }
  function persist() {
    try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(state)); } catch (e) { /* in-memory fallback */ }
    notify();
  }
  function mutate(fn) {
    state = fromStorage() || state;
    var result = fn(state);
    persist();
    return copy(result);
  }
  function activeWalls(source) { return source.walls.filter(function (wall) { return !wall.deletedAt; }); }
  function groupOrThrow(source, id) {
    var group = source.groups.find(function (item) { return sameId(item.id, id); });
    if (!group) throw new Error("分组不存在或已删除");
    return group;
  }
  function wallOrThrow(source, id) {
    var wall = activeWalls(source).find(function (item) { return sameId(item.id, id); });
    if (!wall) throw new Error("拼墙不存在或已删除");
    return wall;
  }
  function boundCount(wall) {
    if (!Array.isArray(wall.bindings)) return Number(wall.boxes) || 0;
    return wall.bindings.reduce(function (sum, row) { return sum + (Array.isArray(row) ? row.filter(Boolean).length : 0); }, 0);
  }
  function windowCount(wall) { return Array.isArray(wall.windows) ? wall.windows.length : (Number(wall.windows) || 0); }
  function sceneCount(wall) { return Array.isArray(wall.scenes) ? wall.scenes.length : (Number(wall.scenesCount) || 0); }
  function describeWall(source, wall) {
    var result = copy(wall);
    var group = source.groups.find(function (item) { return sameId(item.id, wall.groupId); });
    result.groupName = group ? group.name : "—";
    result.boundCount = boundCount(wall);
    result.totalCells = Number(wall.rows) * Number(wall.cols);
    result.bind = result.boundCount + "/" + result.totalCells;
    result.boxes = result.boundCount;
    result.windowCount = windowCount(wall);
    result.sceneCount = sceneCount(wall);
    return result;
  }
  function describeGroup(source, group) {
    var result = copy(group);
    result.wallCount = activeWalls(source).filter(function (wall) { return sameId(wall.groupId, group.id); }).length;
    return result;
  }
  function checkedName(value, label) {
    var name = String(value || "").trim();
    if (!name) throw new Error("请输入" + label);
    if (name.length > 20) throw new Error(label + "不能超过 20 个字符");
    return name;
  }
  function checkGroupName(source, name, exceptId) {
    if (source.groups.some(function (group) { return !sameId(group.id, exceptId) && nameKey(group.name) === nameKey(name); })) throw new Error("该分组名称已存在");
  }
  function checkWallName(source, groupId, name, exceptId) {
    if (activeWalls(source).some(function (wall) { return sameId(wall.groupId, groupId) && !sameId(wall.id, exceptId) && nameKey(wall.name) === nameKey(name); })) throw new Error("该分组下已存在同名拼墙");
  }
  function checkBindings(source, wall, bindings) {
    if (!Array.isArray(bindings) || bindings.length !== Number(wall.rows) || bindings.some(function (row) { return !Array.isArray(row) || row.length !== Number(wall.cols); })) {
      throw new Error("格位矩阵与拼墙行列不一致");
    }
    var seenPorts = {};
    bindings.forEach(function (row) { row.forEach(function (cell) {
      if (!cell) return;
      var id = String(cell.node || "").toLowerCase();
      var port = Number(cell.port);
      if (!id || !Number.isInteger(port) || port < 1) throw new Error("输出盒或输出口无效");
      if (seenPorts[id + ":" + port]) throw new Error("同一输出口不能绑定多个格位");
      seenPorts[id + ":" + port] = true;
      var other = activeWalls(source).find(function (item) {
        return item.id !== wall.id && (item.bindings || []).some(function (cells) {
          return Array.isArray(cells) && cells.some(function (binding) { return binding && String(binding.node).toLowerCase() === id; });
        });
      });
      if (other) throw new Error("输出盒已绑定拼墙「" + other.name + "」");
      if (global.DvsNodeState) {
        var node = global.DvsNodeState.get(id);
        if (!node || node.lifecycle !== "active" || node.type !== "output") throw new Error("输出盒已退役或未注册");
      }
    }); });
  }
  function getState() { return copy(state); }
  function listGroups() { return state.groups.map(function (group) { return describeGroup(state, group); }).sort(function (a, b) { return b.createdAt.localeCompare(a.createdAt); }); }
  function getGroup(id) {
    var group = state.groups.find(function (item) { return sameId(item.id, id == null ? state.currentGroupId : id); });
    return group ? describeGroup(state, group) : null;
  }
  function createGroup(name) {
    return mutate(function (source) {
      name = checkedName(name, "分组名称");
      checkGroupName(source, name, null);
      var group = { id: uniqueId("g"), name: name, createdAt: stamp() };
      source.groups.unshift(group);
      return describeGroup(source, group);
    });
  }
  function renameGroup(id, name) {
    return mutate(function (source) {
      var group = groupOrThrow(source, id);
      name = checkedName(name, "分组名称");
      checkGroupName(source, name, id);
      group.name = name;
      return describeGroup(source, group);
    });
  }
  function deleteGroup(id) {
    return mutate(function (source) {
      var group = groupOrThrow(source, id);
      if (activeWalls(source).some(function (wall) { return sameId(wall.groupId, id); })) throw new Error("该分组下还有拼墙，请先移走或删除");
      source.groups = source.groups.filter(function (item) { return !sameId(item.id, id); });
      if (sameId(source.currentGroupId, id)) {
        source.currentGroupId = source.groups.length ? source.groups[0].id : null;
        source.currentWallId = null;
      }
      return group;
    });
  }
  function listWalls(groupId) {
    var walls = activeWalls(state);
    if (groupId != null) walls = walls.filter(function (wall) { return sameId(wall.groupId, groupId); });
    return walls.map(function (wall) { return describeWall(state, wall); });
  }
  function getWall(id) {
    var wall = activeWalls(state).find(function (item) { return sameId(item.id, id == null ? state.currentWallId : id); });
    return wall ? describeWall(state, wall) : null;
  }
  function createWall(input) {
    return mutate(function (source) {
      input = input || {};
      var groupId = input.groupId == null ? source.currentGroupId : String(input.groupId);
      groupOrThrow(source, groupId);
      var name = checkedName(input.name, "拼墙名称");
      checkWallName(source, groupId, name, null);
      var rows = Number(input.rows), cols = Number(input.cols);
      if (!Number.isInteger(rows) || rows < 1 || rows > 6) throw new Error("行数取值 1~6");
      if (!Number.isInteger(cols) || cols < 1 || cols > 15) throw new Error("列数取值 1~15");
      var mode = input.mode === "LED" ? "LED" : "LCD";
      var type = mode === "LED" ? "" : (input.type === "非标准" ? "非标准" : "标准");
      var custom = mode === "LED" || type === "非标准";
      var rowPixels = custom ? copy(input.rowPixels || []) : [];
      var colPixels = custom ? copy(input.colPixels || []) : [];
      var firstRes = input.firstRes || "1920×1080";
      var width, height;
      if (custom) {
        if (rowPixels.length !== rows || colPixels.length !== cols || rowPixels.concat(colPixels).some(function (px) { return !Number.isInteger(Number(px)) || Number(px) < 64 || Number(px) > 7680; })) throw new Error("单格像素取值 64~7680");
        width = colPixels.reduce(function (sum, px) { return sum + Number(px); }, 0);
        height = rowPixels.reduce(function (sum, px) { return sum + Number(px); }, 0);
      } else {
        var parts = String(firstRes).split("×").map(Number);
        if (parts.length !== 2 || parts.some(function (px) { return !Number.isInteger(px) || px <= 0; })) throw new Error("请选择首格分辨率");
        width = parts[0] * cols;
        height = parts[1] * rows;
      }
      var id = uniqueId("wall");
      var wall = baseWall(id, groupId, name, mode, type, rows, cols, width + "×" + height + (custom ? "（逐行逐列）" : ""), stamp());
      wall.firstRes = custom ? null : firstRes;
      wall.rowPixels = custom ? rowPixels.map(Number) : Array.from({ length: rows }, function () { return Number(String(firstRes).split("×")[1]); });
      wall.colPixels = custom ? colPixels.map(Number) : Array.from({ length: cols }, function () { return Number(String(firstRes).split("×")[0]); });
      source.walls.unshift(wall);
      return describeWall(source, wall);
    });
  }
  function updateWall(id, patch) {
    return mutate(function (source) {
      var wall = wallOrThrow(source, id);
      patch = patch || {};
      var nextGroup = patch.groupId == null ? wall.groupId : String(patch.groupId);
      groupOrThrow(source, nextGroup);
      var nextName = patch.name == null ? wall.name : checkedName(patch.name, "拼墙名称");
      checkWallName(source, nextGroup, nextName, wall.id);
      if (patch.bindings !== undefined) checkBindings(source, wall, patch.bindings);
      Object.keys(patch).forEach(function (key) {
        if (patch[key] !== undefined && key !== "id" && key !== "deletedAt" && key !== "createdAt" && key !== "groupName" && key !== "boundCount" && key !== "totalCells" && key !== "sceneCount" && key !== "windowCount" && key !== "bind" && key !== "boxes") wall[key] = copy(patch[key]);
      });
      wall.groupId = nextGroup;
      wall.name = nextName;
      return describeWall(source, wall);
    });
  }
  function deleteWall(id) {
    return mutate(function (source) {
      var wall = wallOrThrow(source, id);
      if (boundCount(wall) || windowCount(wall) || Number(wall.operations)) throw new Error("请先清屏、解绑并等待操作结束");
      wall.deletedAt = new Date().toISOString();
      if (sameId(source.currentWallId, id)) {
        var replacement = activeWalls(source).find(function (item) { return sameId(item.groupId, wall.groupId); });
        source.currentWallId = replacement ? replacement.id : null;
      }
      return describeWall(source, wall);
    });
  }
  function restoreWall(id) {
    return mutate(function (source) {
      var wall = source.walls.find(function (item) { return sameId(item.id, id); });
      if (!wall || !wall.deletedAt) throw new Error("未找到可恢复的拼墙");
      groupOrThrow(source, wall.groupId);
      checkWallName(source, wall.groupId, wall.name, wall.id);
      wall.deletedAt = null;
      wall.bindings = emptyBindings(wall.rows, wall.cols);
      return describeWall(source, wall);
    });
  }
  function setCurrentGroup(id) {
    return mutate(function (source) {
      var group = groupOrThrow(source, id);
      source.currentGroupId = group.id;
      var first = activeWalls(source).find(function (wall) { return sameId(wall.groupId, group.id); });
      source.currentWallId = first ? first.id : null;
      return describeGroup(source, group);
    });
  }
  function setCurrentWall(id) {
    return mutate(function (source) {
      var wall = wallOrThrow(source, id);
      source.currentWallId = wall.id;
      source.currentGroupId = wall.groupId;
      return describeWall(source, wall);
    });
  }
  function reset() { state = seed(); persist(); return getState(); }
  function subscribe(fn) {
    if (typeof fn !== "function") return function () {};
    listeners.push(fn);
    return function () { listeners = listeners.filter(function (item) { return item !== fn; }); };
  }
  if (global.addEventListener) global.addEventListener("storage", function (event) {
    if (event.key === STORAGE_KEY) { state = fromStorage() || seed(); notify(); }
  });
  global.DvsPlatformStore = {
    storageKey: STORAGE_KEY, getState: getState, reset: reset, subscribe: subscribe,
    listGroups: listGroups, getGroup: getGroup, createGroup: createGroup, renameGroup: renameGroup, deleteGroup: deleteGroup,
    listWalls: listWalls, getWall: getWall, createWall: createWall, updateWall: updateWall, deleteWall: deleteWall, restoreWall: restoreWall,
    currentGroupId: function () { return state.currentGroupId; }, currentWallId: function () { return state.currentWallId; },
    setCurrentGroup: setCurrentGroup, setCurrentWall: setCurrentWall,
    getCurrentGroup: function () { return getGroup(); }, getCurrentWall: function () { return getWall(); }
  };
})(window);
