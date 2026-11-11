// window.channel for the legacy pages, carried over saucer's webview message port.
// Injected at document creation into the main frame of every bridged window.
(function () {
  "use strict";

  if (location.protocol !== "orpheus:" || ["orpheus", "native"].indexOf(location.hostname) < 0)
    return;

  var postToHost = window.saucer.internal.message;
  var LocalXHR = window.XMLHttpRequest;
  var windowId = window.__ncmWindowId || "main";
  var documentId = crypto.randomUUID();
  var listeners = {};
  var pending = {};
  var sequence = 0;

  // Fire-and-forget telemetry and login plumbing the port does not implement.
  var ignoredCalls = new Set([
    "app.log", "app.statis", "app.daEvent", "app.sendStatis", "app.sendstatis", "app.login",
    "app.initConfig", "app.initUrls", "app.initCDNs"
  ]);

  // ---- transport -------------------------------------------------------------------------

  function deliver(name, args) {
    (listeners[name] || []).slice().forEach(function (listener) {
      try {
        listener.apply(null, args);
      } catch (error) {
        reportError(error);
      }
    });
  }

  function receive(message) {
    if (message.kind === "completed") {
      if (message.doc !== documentId) return;
      var callback = pending[message.seq];
      delete pending[message.seq];
      if (callback) callback.apply(null, message.args);
    } else if (message.kind === "event") {
      var target = message.target;
      if (target && target !== windowId && target !== windowId + ":" + documentId) return;
      deliver(message.name, message.args);
    }
  }

  Object.defineProperty(window, "__ncmHost", {value: receive});

  // The original client's synchronous native helpers, answered by the orpheus:// handler.
  function pureHelper(name, args) {
    var xhr = new LocalXHR();
    xhr.open("POST", "orpheus://orpheus/__native/pure/" + name, false);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.send(JSON.stringify(args));
    if (xhr.status !== 200) throw new Error("Native helper failed: " + name + " " + xhr.status);
    var result = JSON.parse(xhr.responseText);
    if (result.error) throw new Error(result.error);
    return result.value;
  }

  // ---- window.channel --------------------------------------------------------------------

  // Calls whose JS signature differs from the native handler's.
  function adapt(name, args) {
    if (name === "app.exit")
      name = args.length === 1 ? "app.exitWithMode" : args.length === 4 ? "app.exitWithMove" : name;
    if (name === "os.checkNativeSupportFonts") args = [args];
    if (name === "storage.clearCache" && args[0] === null) args = [[]];
    return {name: name, args: args};
  }

  function post(call, callback) {
    var seq = ++sequence;
    if (typeof callback === "function") pending[seq] = callback;
    postToHost(JSON.stringify({
      ncm: "call", doc: documentId, seq: seq, method: call.name, args: call.args
    }));
  }

  function forgetSignInWindow(call) {
    if (call.name !== "winhelper.launchWindow" || !/\/login\.html\b/.test(call.args[0])) return;
    try {
      var open = JSON.parse(localStorage.getItem("NM_PAGE_OPEN_STATE"));
      if (open && open.login) {
        delete open.login;
        localStorage.setItem("NM_PAGE_OPEN_STATE", JSON.stringify(open));
      }
    } catch (error) {
      reportError(error);
    }
  }

  // GTK raises contextmenu while the right button is still down and keeps the pointer until it
  // comes up, so a native menu opened before then cannot take the pointer: it would not close on
  // a click elsewhere. Menus asked for meanwhile open on release.
  var rightButtonDown = false;
  var heldMenus = [];

  function releaseMenus() {
    rightButtonDown = false;
    heldMenus.splice(0).forEach(function (open) { open(); });
  }

  window.addEventListener("mousedown", function (event) {
    if (event.button === 2) rightButtonDown = true;
  }, true);
  window.addEventListener("mouseup", function (event) {
    if (event.button === 2) releaseMenus();
  }, true);
  window.addEventListener("blur", releaseMenus);

  var channel = window.channel = {
    registerCall: function (name, listener) {
      if (typeof listener === "function") (listeners[name] || (listeners[name] = [])).push(listener);
    },
    call: function (name, callback, args) {
      if (ignoredCalls.has(name)) {
        if (typeof callback === "function") queueMicrotask(callback);
        return;
      }
      var call = adapt(name, (args || []).map(function (v) { return v === undefined ? {} : v; }));
      forgetSignInWindow(call);
      if (call.name === "winhelper.popupMenu" && rightButtonDown)
        heldMenus.push(function () { post(call, callback); });
      else
        post(call, callback);
    }
  };

  ["encodeAnonymousId", "encodeAnonymousId2", "encryptId", "serialData", "serialData2"]
    .forEach(function (name) {
      channel[name] = function () {
        return pureHelper(name, Array.prototype.slice.call(arguments));
      };
    });

  if (windowId !== "main") return;

  channel.registerCall("player.onmprisplayback", function (playing) {
    var player = window.ctl && ctl.player && ctl.player.bHr();
    if (!player) return;
    if (playing)
      player.biR(); // Start a stopped queue or resume the current track.
    else
      player.bvY("pause");
  });

  // ---- imported sessions -----------------------------------------------------------------

  document.addEventListener("load", function attachSession(event) {
    if (event.target.tagName !== "SCRIPT" || !event.target.src) return;
    var script = new URL(event.target.src), core = new URL("./core.js", location.href);
    if (script.protocol !== core.protocol || script.host !== core.host || script.pathname !== core.pathname)
      return;
    if (!window.ctl || !ctl.host) return;
    document.removeEventListener("load", attachSession, true);

    var host = ctl.host, api = NEJ.P("nm.x"), storage = NEJ.P("nej.j");
    var keyDomain = APP_CONF.domain.replace(/^https:/, "http:");

    function imported() {
      return localCache.ce("host.importedSession") && localCache.ce("host.profile.userId");
    }

    var legacyGuest = api.VH;
    host.VH = api.VH = function () { return !imported() && legacyGuest.call(this); };

    function applySession(result) {
      APP_CONF.createAnonimous = false;
      delete APP_CONF.anonimousToken;
      storage.bdp(NEJ.F);
      var account = result.account;
      account.username = (account.userName || "").replace(account.type + "_", "");
      account.remember = true;
      var profile = {account: account, profile: result.profile, importedSession: true};
      // The pages' own copy of a signed-in session, which earlier runs may have left.
      storage.Gc("NM_AUTO_" + keyDomain);
      storage.Gc("NM_AUTO_COOKIE_" + keyDomain);
      localCache.pK("host", profile);
      storage.Dh("NM_HOST", profile);
      host.cJZ();
      host.ccl();
    }
    channel.registerCall("app.onimportedsession", applySession);

    var start = host.Yr;
    host.Yr = function () {
      host.Yr = function () {};
      channel.call("app.getimportedsession", function (result) {
        if (!result || !result.profile || !result.profile.userId) {
          start.call(host);
          return;
        }
        // Reuse the legacy device setup without clearing the restored cookie jar.
        var restore = host.ccg;
        host.ccg = function () {
          host.ccg = restore;
          if (!imported()) applySession(result);
        };
        host.cKo({});
      });
    };
  }, true);
})();
