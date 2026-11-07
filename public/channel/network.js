// Routes the pages' remote XHRs through the native network stack (network.request /
// network.upload), which owns the session cookies. Local orpheus:// requests keep using the
// webview's own XMLHttpRequest. Loaded right after bridge.js.
(function () {
  "use strict";

  if (location.protocol !== "orpheus:" || ["orpheus", "native"].indexOf(location.hostname) < 0)
    return;

  var LocalXHR = window.XMLHttpRequest;
  var requestScope = crypto.randomUUID();
  var requestSequence = 0;
  var forwardedEvents = ["readystatechange", "loadstart", "progress", "load", "error", "abort",
                         "timeout", "loadend"];
  var mirroredState = ["readyState", "status", "statusText", "response", "responseURL"];

  // ---- helpers ---------------------------------------------------------------------------

  // Native EventTarget dispatch reports listener errors without unwinding the request.
  function fire(xhr, name) {
    var event = new Event(name);
    var handler = xhr["on" + name];
    if (typeof handler === "function") {
      try {
        handler.call(xhr, event);
      } catch (error) {
        if (typeof window.reportError === "function") window.reportError(error);
        else setTimeout(function () { throw error; }, 0);
      }
    }
    xhr._events.dispatchEvent(event);
  }

  function findHeader(headers, name) {
    var lower = name.toLowerCase();
    return headers.filter(function (h) { return h[0].toLowerCase() === lower; });
  }

  function toBytes(body) {
    if (body == null) return new Uint8Array();
    if (typeof body === "string") return new TextEncoder().encode(body);
    if (body instanceof ArrayBuffer) return new Uint8Array(body);
    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
    throw new TypeError("Unsupported native XHR body type");
  }

  function toBase64(bytes) {
    var binary = "";
    for (var offset = 0; offset < bytes.length; offset += 32768)
      binary += String.fromCharCode.apply(null, bytes.subarray(offset, offset + 32768));
    return btoa(binary);
  }

  function fromBase64(encoded) {
    var binary = atob(encoded || ""), bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }

  // orpheus://orpheus/storage/customrequest?url=...&data=...&customheader=<base64 JSON>
  // asks the client to perform a request with explicit headers.
  function parseCustomRequest(url, body) {
    var params = new URLSearchParams(new URL(url).search);
    if (typeof body === "string")
      new URLSearchParams(body).forEach(function (value, key) { params.set(key, value); });
    var headers;
    try {
      headers = JSON.parse(atob(params.get("customheader") || "e30="));
    } catch (_) {
      headers = {};
    }
    return {url: params.get("url"), body: params.get("data") || "", headers: headers};
  }

  // ---- NativeXHR -------------------------------------------------------------------------
  //
  // One request at a time, like XMLHttpRequest: open() and abort() drop the request under way,
  // whose late events then find it is no longer this._request (or this._local) and stop.

  function NativeXHR() {
    this._events = new EventTarget();
    this.upload = new EventTarget();
    this.readyState = 0;
    this.status = 0;
    this.statusText = "";
    this.responseText = "";
    this.response = null;
    this.responseType = "";
    this.responseURL = "";
    this.timeout = 0;
    this.withCredentials = false;
    this._headers = [];
    this._request = null;
    this._local = null;
  }

  ["UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"].forEach(function (name, i) {
    NativeXHR[name] = NativeXHR.prototype[name] = i;
  });

  NativeXHR.prototype.addEventListener = function () {
    this._events.addEventListener.apply(this._events, arguments);
  };

  NativeXHR.prototype.removeEventListener = function () {
    this._events.removeEventListener.apply(this._events, arguments);
  };

  NativeXHR.prototype.dispatchEvent = function () {
    return this._events.dispatchEvent.apply(this._events, arguments);
  };

  // Drops the remote request under way.
  NativeXHR.prototype._cancel = function () {
    if (this._request) channel.call("network.cancel", function () {}, [this._request.id]);
    this._request = null;
    clearTimeout(this._timer);
  };

  NativeXHR.prototype.open = function (method, url, async, user, password) {
    var target = new URL(url, location.href);
    var previous = this._local;
    this._local = null;
    if (previous) previous.abort();
    this._cancel();

    this._responseHeaders = [];
    this.response = null;
    this.responseText = "";
    this.status = 0;
    this._custom = target.protocol === "orpheus:" && target.hostname === "orpheus"
                   && target.pathname === "/storage/customrequest";
    this._upload = target.protocol === "orpheus:" && target.hostname === "upload";
    this._remote = /^https?:$/.test(target.protocol) || this._custom || this._upload;
    // Upper case, as XMLHttpRequest sends the standard methods.
    this._method = /^(delete|get|head|options|post|put)$/i.test(method) ? method.toUpperCase() : method;
    this._url = target.href;
    this._async = async !== false;
    this._sending = false;
    this._headers = [];

    if (this._remote) {
      if (!this._async)
        throw new DOMException("Remote network I/O must be asynchronous", "InvalidAccessError");
      this.readyState = 1;
      fire(this, "readystatechange");
      return;
    }

    var self = this, local = this._local = new LocalXHR();
    forwardedEvents.forEach(function (name) {
      local.addEventListener(name, function () {
        if (self._local !== local) return;
        mirroredState.forEach(function (key) {
          try { self[key] = local[key]; } catch (_) {}
        });
        try { self.responseText = local.responseText; } catch (_) {}
        fire(self, name);
      });
    });
    local.open(method, url, this._async, user, password);
  };

  NativeXHR.prototype.setRequestHeader = function (name, value) {
    if (this._local) return this._local.setRequestHeader(name, value);
    var existing = findHeader(this._headers, String(name))[0];
    if (existing) existing[1] += ", " + String(value);
    else this._headers.push([String(name), String(value)]);
  };

  NativeXHR.prototype.overrideMimeType = function (mime) {
    this._mime = mime;
    if (this._local) this._local.overrideMimeType(mime);
  };

  NativeXHR.prototype.getAllResponseHeaders = function () {
    if (this._local) return this._local.getAllResponseHeaders();
    return (this._responseHeaders || []).map(function (h) { return h[0] + ": " + h[1] + "\r\n"; }).join("");
  };

  NativeXHR.prototype.getResponseHeader = function (name) {
    if (this._local) return this._local.getResponseHeader(name);
    var values = findHeader(this._responseHeaders || [], name);
    return values.length ? values.map(function (h) { return h[1]; }).join(", ") : null;
  };

  NativeXHR.prototype.abort = function () {
    if (this._local) return this._local.abort();
    var active = this._sending;
    this._sending = false;
    this._cancel();
    this.readyState = 0;
    this.status = 0;
    if (active) {
      fire(this, "abort");
      fire(this, "loadend");
    }
  };

  NativeXHR.prototype.send = function (body) {
    if (this._local) {
      if (this._async) {
        this._local.timeout = this.timeout;
        this._local.responseType = this.responseType;
      }
      this._local.withCredentials = this.withCredentials;
      return this._local.send(body);
    }

    var self = this;
    var url = this._url, headers = this._headers.slice();
    if (this._custom) {
      var custom = parseCustomRequest(this._url, body);
      url = custom.url;
      body = custom.body;
      Object.keys(custom.headers).forEach(function (key) {
        headers.push([key === "userAgent" ? "User-Agent" : key, String(custom.headers[key])]);
      });
    }
    var bytes = toBytes(body);
    if (!findHeader(headers, "User-Agent").length) headers.push(["User-Agent", navigator.userAgent]);
    if (typeof body === "string" && !findHeader(headers, "Content-Type").length)
      headers.push(["Content-Type", "text/plain;charset=UTF-8"]);

    var request = this._request = {
      id: requestScope + ":" + (++requestSequence), method: this._method, url: url,
      headers: headers, bodyBase64: toBase64(bytes)
    };
    this._sending = true;
    fire(this, "loadstart");
    if (this._request !== request) return;
    if (this.timeout > 0)
      this._timer = setTimeout(function () {
        if (self._request !== request) return;
        self._cancel();
        self._sending = false;
        self.status = 0;
        self.readyState = 4;
        fire(self, "readystatechange");
        fire(self, "timeout");
        fire(self, "loadend");
      }, this.timeout);
    channel.call(this._upload ? "network.upload" : "network.request", function (result) {
      self._complete(request, result);
    }, [request]);
  };

  NativeXHR.prototype._complete = function (request, result) {
    if (this._request !== request) return;
    clearTimeout(this._timer);
    this.status = result.status || 0;
    this.statusText = result.statusText || "";
    this.responseURL = result.finalUrl || this._url;
    // The native jar keeps the cookies; pages never see them, as with real HttpOnly cookies.
    this._responseHeaders = (result.headers || []).filter(function (h) { return !/^set-cookie2?$/i.test(h[0]); });
    this.readyState = 2;
    fire(this, "readystatechange");
    if (this._request !== request) return;

    var data = fromBase64(result.bodyBase64);
    this.responseText = new TextDecoder("utf-8").decode(data);
    if (this._custom)
      this.responseText = JSON.stringify({
        body: this.responseText,
        header: "HTTP/1.1 " + this.status + " " + this.statusText + "\u0000"
                + this._responseHeaders.map(function (h) { return h[0] + ": " + h[1]; }).join("\u0000")
      });
    this.readyState = 3;
    fire(this, "readystatechange");
    if (this._request !== request) return;

    if (this.responseType === "arraybuffer") {
      this.response = data.buffer;
    } else if (this.responseType === "blob") {
      this.response = new Blob([data], {type: this.getResponseHeader("content-type") || ""});
    } else if (this.responseType === "json") {
      try { this.response = JSON.parse(this.responseText); } catch (_) { this.response = null; }
    } else if (this.responseType === "document") {
      this.response = new DOMParser().parseFromString(this.responseText, "text/xml");
    } else {
      this.response = this.responseText;
    }
    this._sending = false;
    this.readyState = 4;
    fire(this, "readystatechange");
    if (this._request !== request) return;
    fire(this, this.status ? "load" : "error");
    if (this._request !== request) return;
    this._request = null;
    fire(this, "loadend");
  };

  window.XMLHttpRequest = NativeXHR;
})();
