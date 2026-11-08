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
  /* Empty initial state: the browser holds no business truth. Real data
   * arrives via refreshFromServer; while the server is unreachable the
   * pages render empty instead of demo walls. Local edits stay drafts
   * until the owning page wires them to the matching REST call. */
  function seed() {
    return {
      version: 2,
      groups: [],
      walls: [],
      currentGroupId: null, currentWallId: null
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
  function serverItems(payload) {
    if (!payload || !payload.ok) return null;
    var d = payload.data;
    if (Array.isArray(d)) return d;
    if (d && Array.isArray(d.items)) return d.items;
    return null;
  }
  /* Adapt a server wall doc to the local wall view. The server stores
   * bindings/scenes/whitelist as separate doc kinds the list pages do not
   * join yet, so adapted walls carry a 1x1 placeholder canvas plus the raw
   * pixel size; detail pages show the served data instead of demo data. */
  function adaptServerWall(w) {
    w = w || {};
    var px = "";
    if (w.wall_px_w && w.wall_px_h) px = w.wall_px_w + "×" + w.wall_px_h;
    else if (w.w && w.h) px = w.w + "×" + w.h;
    return {
      id: w.id, groupId: w.groupId || w.group || null, name: w.name || w.id,
      mode: w.mode || "", type: w.type || "", rows: 1, cols: 1,
      res: px || "—", rowPixels: [], colPixels: [], firstRes: null,
      bindings: [[null]], whitelist: [], settings: w.settings || null,
      backdrop: w.backdrop || null, scenes: [],
      currentSceneId: w.currentSceneId || null, windows: [], operations: 0,
      layoutMode: w.layoutMode || "free", volume: w.volume == null ? 80 : w.volume,
      deletedAt: null, createdAt: w.createdAt || "", serverDoc: true
    };
  }
  /* Pull groups/walls from the server and replace the local state. Server
   * wins on conflict; unreachable keeps the local drafts. Never throws. */
  function refreshFromServer() {
    if (!global.DvsApi || typeof global.DvsApi.request !== "function") {
      return Promise.resolve(false);
    }
    return Promise.all([
      global.DvsApi.request("GET", "/api/v1/groups"),
      global.DvsApi.request("GET", "/api/v1/walls")
    ]).then(function (results) {
      var groups = serverItems(results[0]);
      var walls = serverItems(results[1]);
      if (!groups && !walls) return false;
      state = {
        version: 2,
        groups: (groups || []).map(function (g) {
          return { id: g.id, name: g.name || g.id, createdAt: g.createdAt || "" };
        }),
        walls: (walls || []).map(adaptServerWall),
        currentGroupId: null, currentWallId: null
      };
      if (state.groups.length) state.currentGroupId = state.groups[0].id;
      var firstWall = state.walls.filter(function (w) { return sameId(w.groupId, state.currentGroupId); })[0] || state.walls[0];
      if (firstWall) {
        state.currentWallId = firstWall.id;
        state.currentGroupId = firstWall.groupId;
      }
      persist();
      return true;
    }).catch(function (err) {
      if (global.console && global.console.warn) global.console.warn("platform store refresh failed", err);
      return false;
    });
  }
  if (global.DvsApi && typeof global.DvsApi.request === "function") refreshFromServer();
  global.DvsPlatformStore = {
    storageKey: STORAGE_KEY, getState: getState, reset: reset, subscribe: subscribe,
    refreshFromServer: refreshFromServer,
    listGroups: listGroups, getGroup: getGroup, createGroup: createGroup, renameGroup: renameGroup, deleteGroup: deleteGroup,
    listWalls: listWalls, getWall: getWall, createWall: createWall, updateWall: updateWall, deleteWall: deleteWall, restoreWall: restoreWall,
    currentGroupId: function () { return state.currentGroupId; }, currentWallId: function () { return state.currentWallId; },
    setCurrentGroup: setCurrentGroup, setCurrentWall: setCurrentWall,
    getCurrentGroup: function () { return getGroup(); }, getCurrentWall: function () { return getWall(); }
  };
})(window);
