/* Platform shell adapter. Business handlers and the prototype's data contracts stay intact. */
(function () {
  var titles = {
    "group-list": "分组管理",
    "wall-list": "拼墙管理",
    "wall-basic": "基本设置",
    "port-bind": "端口绑定",
    "wall-image": "图像调节",
    "wall-transfer": "传输配置",
    "wall-backdrop": "高清底图",
    "screen-schedule": "画面调度",
    "device-list": "设备列表"
  };

  function icon(name) {
    return '<i class="t-icon t-icon-' + name + '" aria-hidden="true"></i>';
  }

  function replaceCloseButtons(root) {
    if (!root.querySelectorAll) return;
    if (root.matches && root.matches("button") && !root.children.length && root.textContent.trim() === "×") {
      root.innerHTML = icon("close");
      root.dataset.itcIcon = "close";
      if (!root.getAttribute("aria-label")) root.setAttribute("aria-label", "关闭");
    }
    root.querySelectorAll("button").forEach(function (button) {
      if (button.dataset.itcIcon === "close" || button.children.length || button.textContent.trim() !== "×") return;
      button.innerHTML = icon("close");
      button.dataset.itcIcon = "close";
      if (!button.getAttribute("aria-label")) button.setAttribute("aria-label", "关闭");
    });
  }

  function init() {
    var shell = document.querySelector(".proto-shell");
    if (!shell || document.body.dataset.protoPage === "node-admin") return;
    var key = location.pathname.split("/").pop().replace(/\.html$/, "");
    var title = titles[key] || "视频墙控制";
    document.title = title + " | 分布式视频管理";
    document.body.classList.add("itc-platform");
    document.body.dataset.itcPage = key;

    var logo = shell.querySelector(".proto-side-logo-mark");
    if (logo) logo.innerHTML = '<img src="../../assets/brand/ITC Design-logo-write.svg" alt="DVS" />';
    var menu = shell.querySelector(".proto-side-menu");
    if (menu) {
      menu.innerHTML =
        '<a class="proto-side-item is-active" href="group-list.html" title="信号调度">' + icon("video-filled") + '<span>信号调度</span></a>';
    }

    var brand = shell.querySelector(".proto-header-brand");
    if (brand) {
      brand.href = "group-list.html";
      brand.title = "返回分组管理";
      brand.innerHTML = icon("component-radio") + '<span class="proto-brand-name">视频墙控制</span>';
    }
    var fold = document.getElementById("proto-fold-sub");
    if (fold) {
      fold.innerHTML = icon("view-list");
      fold.setAttribute("aria-label", "收起或展开页面菜单");
      fold.addEventListener("click", function () {
        fold.innerHTML = icon(shell.classList.contains("is-sub-collapsed") ? "menu-unfold" : "view-list");
      });
    }
    shell.querySelectorAll('.proto-breadcrumb a[href="hub.html"]').forEach(function (a) {
      a.href = "group-list.html";
      a.textContent = "信号调度";
    });
    shell.querySelectorAll(".proto-breadcrumb").forEach(function (crumb) {
      var first = crumb.querySelector("a");
      var firstSlash = first && first.nextElementSibling;
      var second = firstSlash && firstSlash.nextElementSibling;
      var secondSlash = second && second.nextElementSibling;
      if (first && second && second.textContent.trim() === first.textContent.trim()) {
        second.remove();
        if (secondSlash) secondSlash.remove();
      }
    });
    shell.querySelectorAll('.proto-breadcrumb .current').forEach(function (el) {
      if (el.textContent.indexOf("原型") !== -1) el.textContent = title;
    });
    var user = shell.querySelector(".proto-header-user");
    if (user) user.innerHTML = icon("user-circle") + '<span>' + (user.textContent.trim() || "管理员") + '</span>';

    var headerRight = shell.querySelector(".proto-header-right");
    if (headerRight && !headerRight.querySelector(".itc-header-actions")) {
      var actions = document.createElement("div");
      actions.className = "itc-header-actions";
      actions.innerHTML =
        '<button type="button" class="itc-header-action" aria-label="通知" title="通知">' + icon("notification") + '</button>' +
        '<button type="button" class="itc-header-action" aria-label="退出登录" title="退出登录">' + icon("logout") + '</button>';
      var logout = actions.lastElementChild;
      if (user) {
        headerRight.insertBefore(actions, user);
        actions.appendChild(user);
        actions.appendChild(logout);
      } else {
        headerRight.appendChild(actions);
      }
    }

    shell.querySelectorAll(".proto-side-sub-group").forEach(function (group) { group.classList.add("is-open"); });
    shell.querySelectorAll(".proto-side-sub-group").forEach(function (group) {
      var label = group.querySelector(".proto-menu-label");
      var iconHolder = group.querySelector(".proto-menu-ico");
      var caret = group.querySelector(".proto-menu-caret");
      if (!label || !iconHolder) return;
      iconHolder.innerHTML = icon(label.textContent.trim() === "设备管理" ? "device-filled" : "control-platform-filled");
      iconHolder.setAttribute("aria-hidden", "true");
      if (caret) {
        var syncCaret = function () {
          caret.innerHTML = icon(group.classList.contains("is-open") ? "chevron-up" : "chevron-down");
        };
        syncCaret();
        group.querySelector(".proto-side-sub-parent").addEventListener("click", syncCaret);
      }
    });
    shell.querySelectorAll(".proto-back-ico").forEach(function (el) {
      el.className = "t-icon t-icon-arrow-left";
    });
    replaceCloseButtons(document);
    var observer = new MutationObserver(function (records) {
      records.forEach(function (record) {
        record.addedNodes.forEach(function (node) {
          if (node.nodeType === 1) replaceCloseButtons(node);
        });
      });
      if (user && !user.querySelector(".t-icon")) {
        var label = user.textContent.trim() || "管理员";
        user.innerHTML = icon("user-circle") + '<span>' + label + '</span>';
      }
    });
    observer.observe(document.body, { childList: true, subtree: true });

    var content = shell.querySelector(".proto-content");
    if (content && !content.querySelector(".itc-footer")) {
      var footer = document.createElement("footer");
      footer.className = "itc-footer";
      footer.textContent = "Copyright © 2025-2026 DVS. All Rights Reserved";
      content.appendChild(footer);
    }
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
