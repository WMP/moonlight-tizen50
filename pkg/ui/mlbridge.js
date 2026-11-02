/* eslint-disable */
/*
 * mlbridge.js - transport shim for the hybrid Moonlight Tizen 5.0 package.
 *
 * moonlight-tizen's UI (platform/messages.js) calls the native WASM module via
 * `Module.makeCert/httpInit/openUrl/pair/startStream/...`. On Tizen 5.0 there is
 * no WASM/EMSS; the native work runs in the .NET host service app
 * (org.moonlight.host.app), reachable over the "moonlight" message port. This
 * file replaces the Module-based transport with message-port calls, keeping
 * messages.js's sendMessage() contract. Wire format: see pkg/ui/PROTOCOL.md.
 *
 * Load this AFTER platform/messages.js so it overrides SyncFunctions/AsyncFunctions
 * and sendMessage. Note: needs validation on a device (tizen.messageport and
 * application.launch behaviour); host-side tests only cover tests/mlbridge_test.js.
 */
(function () {
  var ENGINE_APPID = 'org.moonlight.host.app';
  var ENGINE_PORT = 'moonlight';
  var LOCAL_PORT = 'moonlightui';

  var TRACE = !!window.ML_TRACE;   // set by mltrace.js in debug builds
  function trace() {
    if (!TRACE) return;
    var a = ['[mlbridge]'].concat(Array.prototype.slice.call(arguments));
    console.log.apply(console, a);
  }

  // index.js reads Samsung product/AV info at top level (index.js:2-29). One throwing
  // call there aborts the whole file (db, BUILD_TYPE never initialise) and the UI
  // resets to the splash. mlbridge.js loads before index.js, so wrap those calls
  // to log and return a safe default instead of throwing.
  function guard(obj, name, names, dflt) {
    if (!obj) return;
    names.forEach(function (n) {
      var f = obj[n];
      if (typeof f !== 'function') return;
      obj[n] = function () {
        try { return f.apply(obj, arguments); }
        catch (e) { console.warn('[mlbridge] ' + name + '.' + n + ' failed, using default:', e && (e.message || e.name || e)); return dflt; }
      };
    });
  }
  try {
    if (window.webapis) {
      guard(webapis.productinfo, 'productinfo', ['getModel', 'getRealModel', 'getModelCode', 'getFirmware', 'getDuid'], '');
      guard(webapis.productinfo, 'productinfo', ['isUdPanelSupported', 'is8KPanelSupported'], false);
      guard(webapis.avinfo, 'avinfo', ['isHdrTvSupport'], false);
    }
  } catch (e) { console.error('[mlbridge] webapis guard failed:', e); }

  var localPort = null, remotePort = null, nextId = 1;
  var pending = {};   // id -> {resolve, reject, timer}
  var queue = [];     // messages waiting for the remote port
  var launched = false, retryTimer = null, retryDelay = 250;
  var DEFAULT_TIMEOUT = 15000, PAIR_TIMEOUT = 120000, RETRY_MAX = 4000;

  function ensurePorts() {
    if (localPort && remotePort) return true;
    trace('ensurePorts: connecting', ENGINE_APPID, ENGINE_PORT);
    try {
      if (!localPort) {
        var lp = tizen.messageport.requestLocalMessagePort(LOCAL_PORT);
        lp.addMessagePortListener(onReply);
        localPort = lp;   // set only once the listener is attached
      }
    } catch (e) {
      console.error('[mlbridge] local message port setup failed:', e);
      return false;
    }
    try {
      remotePort = tizen.messageport.requestRemoteMessagePort(ENGINE_APPID, ENGINE_PORT);
      trace('ensurePorts: ok');
      return true;
    } catch (e) {
      trace('ensurePorts: remote port missing', e && (e.name || e.message || e));
      if (!launched) {   // start the host once; it creates the port
        launched = true;
        trace('launch', ENGINE_APPID);
        try { tizen.application.launch(ENGINE_APPID); }
        catch (e2) { console.error('[mlbridge] launch failed:', e2); }
      }
      return false;
    }
  }

  function rawSend(msg) {
    try { remotePort.sendMessage(msg, localPort); return null; }
    catch (e) { remotePort = null; launched = false; return e; }   // host gone: reconnect (and relaunch) on the next call
  }

  function flush() {
    trace('flush queued=' + queue.length);
    while (queue.length) {
      var q = queue.shift();
      var err = rawSend(q.msg);
      if (err) failPending(q.id, err);
    }
  }

  function failPending(id, err) {
    var p = pending[id];
    if (!p) return;
    delete pending[id];
    if (p.timer) clearTimeout(p.timer);
    p.reject(err);
  }

  // Connect now, or retry with backoff (250 ms doubling up to 4 s); one timer at a time.
  function connect() {
    if (retryTimer) return;
    if (ensurePorts()) { retryDelay = 250; flush(); return; }
    trace('retry in ' + retryDelay + ' ms');
    retryTimer = setTimeout(function () {
      retryTimer = null;
      retryDelay = Math.min(retryDelay * 2, RETRY_MAX);
      connect();
    }, retryDelay);
  }

  // Replies arrive as an array of {key,value}. We key everything by 'id'.
  function onReply(data /*, remoteMessagePort */) {
    var m = {};
    for (var i = 0; i < data.length; i++) m[data[i].key] = data[i].value;
    trace('reply id=' + m.id, 'keys=' + Object.keys(m).join(','), m.error ? 'error=' + m.error : '');
    if (m.event) {              // async push (streamTerminated/Connection Established)
      trace('event', m.event);
      if (typeof window.handleMessage === 'function') window.handleMessage(String(m.event));
      return;
    }
    var p = pending[m.id];
    if (!p) { trace('reply for unknown/timed-out id=' + m.id + ' ignored'); return; }
    delete pending[m.id];
    if (p.timer) clearTimeout(p.timer);
    if (m.error) p.reject(m.error);
    else p.resolve(m);
  }

  // Send {cmd,id,...fields} and resolve with the reply map. timeoutMs 0 = no timeout.
  // cancelOnTimeout: also send cancelRequest when the timeout fires (openUrl).
  function bridgeCall(cmd, fields, timeoutMs, cancelOnTimeout) {
    return new Promise(function (resolve, reject) {
      var id = String(nextId++);
      var msg = [{ key: 'cmd', value: cmd }, { key: 'id', value: id }];
      var names = [];
      for (var k in fields) if (fields[k] !== undefined && fields[k] !== null) {
        msg.push({ key: k, value: String(fields[k]) });
        names.push(k);   // names only: cert/key/pin values must not be logged
      }
      var p = pending[id] = { resolve: resolve, reject: reject, timer: null };
      if (timeoutMs > 0) p.timer = setTimeout(function () {
        if (!pending[id]) return;
        delete pending[id];
        trace('timeout cmd=' + cmd, 'id=' + id);
        if (cancelOnTimeout) bridgeCall('cancelRequest', {}, 0).then(null, function () {});
        reject('timeout');
      }, timeoutMs);
      if (remotePort) {
        trace('send cmd=' + cmd, 'id=' + id, 'fields=' + names.join(','));
        var err = rawSend(msg);
        if (err) failPending(id, err);
      } else {
        trace('queue cmd=' + cmd, 'id=' + id, 'fields=' + names.join(','));
        queue.push({ id: id, msg: msg });
        connect();
      }
    });
  }

  function call(cmd, fields) { return bridgeCall(cmd, fields, DEFAULT_TIMEOUT); }

  // --- override the command maps used by messages.js sendMessage() ---
  // SyncFunctions returned {type,ret} synchronously in WASM; here they are async,
  // so we also override sendMessage below to await uniformly.
  window.SyncFunctions = {
    makeCert: function () { return call('makeCert', {}); },
    httpInit: function (cert, key, uniqueId) { return call('httpInit', { cert: cert, key: key, uniqueId: uniqueId }); },
    startRequest: function () {
      var a = arguments; // host, httpPort, w, h, fps, bitrate, rikey, rikeyid, appVer, gfeVer, rtspUrl, codecMode, ... a[18] audioConfig, a[24] videoCodec, a[25] hdr
      return call('startStream', {
        host: a[0], w: a[2], h: a[3], fps: a[4], bitrate: a[5], rikey: a[6], rikeyid: a[7],
        appVer: a[8], gfeVer: a[9], rtspUrl: a[10], codecMode: a[11],
        audioConfig: a[18], videoCodec: a[24], hdr: String(!!a[25])
      });
    },
    stopRequest: function () { return call('stopStream', {}); },
    cancelRequest: function () { return call('cancelRequest', {}); },
    toggleStats: function () { return call('toggleStats', {}); },
  };
  window.AsyncFunctions = {
    openUrl: function (url, ppk, binary, timeout) {
      return bridgeCall('openUrl', { url: url, ppk: ppk, binary: String(!!binary) },
                        timeout > 0 ? timeout : DEFAULT_TIMEOUT, true);
    },
    STUN: function () { return call('stun', {}); },
    pair: function (major, address, httpPort, pin) {
      return bridgeCall('pair', { major: major, addr: address, port: httpPort, pin: pin }, PAIR_TIMEOUT);
    },
    wakeOnLan: function (mac) { return call('wakeOnLan', { mac: mac }); },
  };

  // Uniform async sendMessage over the bridge (replaces the WASM-era mix of
  // sync/async). Returns the reply's primary field for back-compat where callers
  // expect a value (openUrl -> data, pair -> serverCert, makeCert -> {cert,privateKey}).
  window.sendMessage = function (method, params) {
    params = params || [];
    var fn = window.SyncFunctions[method] || window.AsyncFunctions[method];
    trace('sendMessage', method);
    if (!fn) trace('unknown method', method);
    if (!fn) return Promise.reject('unknown method ' + method);
    return fn.apply(null, params).then(function (r) {
      if (method === 'makeCert') return { cert: r.cert, privateKey: r.key };
      if (method === 'openUrl') {
        var d = r.data;
        if (params[2] && typeof d !== 'string' && d != null) return new Uint8Array(d);   // bytes -> Uint8Array
        return d;
      }
      if (method === 'pair') return r.serverCert;
      return r.result;
    });
  };


  // Some UI code calls Module.<fn> directly (not via sendMessage), notably
  // Module.sendKeyboardEvent during streaming. Provide a shim so the UI needs no
  // WASM module. HEAPF* (Emscripten perf-stats heap access) is stubbed - the
  // in-stream stats overlay that reads WASM memory is not available in hybrid.
  window.Module = {
    sendKeyboardEvent: function (keyCode, keyAction, modifiers) {
      return bridgeCall('sendKeyboardEvent', { key: keyCode, action: keyAction, mods: modifiers });
    },
    stopStream: function () { return bridgeCall('stopStream', {}); },
    toggleStats: function () { return bridgeCall('toggleStats', {}); },
    HEAPF32: { buffer: new ArrayBuffer(0) },   // stub; perf-stats overlay disabled
    HEAPF64: { buffer: new ArrayBuffer(0) },
  };

  // Boot without the WASM engine. The original starts the UI from the compiled
  // module: common.js on DOMContentLoaded calls createWASMModule() -> loads
  // moonlight-wasm.js -> (on WASM ready) window.moduleDidLoad(), which starts the
  // app. We ship NO moonlight-wasm.js (the engine is the native service), so that
  // chain would never fire moduleDidLoad and the UI would hang. mlbridge.js is parsed
  // before common.js, so this DOMContentLoaded listener runs first and replaces
  // common.domContentLoaded to skip the WASM load and fire moduleDidLoad() ourselves.
  document.addEventListener('DOMContentLoaded', function () {
    trace('DOMContentLoaded: common.domContentLoaded exists =',
          !!(window.common && typeof window.common.domContentLoaded === 'function'));
    if (window.common && typeof window.common.domContentLoaded === 'function') {
      window.common.domContentLoaded = function () {
        try { if (window.common.attachDefaultListeners) window.common.attachDefaultListeners(); } catch (e) {}
        connect();   // start/connect the host service (commands queue until connected)
        trace('boot: moduleDidLoad found =', typeof window.moduleDidLoad === 'function');
        if (typeof window.moduleDidLoad === 'function') window.moduleDidLoad();
      };
    }
  }, false);

  console.log('[mlbridge] installed (message-port transport to ' + ENGINE_APPID + ')');
})();
