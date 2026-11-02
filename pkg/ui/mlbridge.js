/* eslint-disable */
/*
 * mlbridge.js - transport shim for the hybrid Moonlight Tizen 5.0 package.
 *
 * moonlight-tizen's UI (platform/messages.js) calls the native WASM module via
 * `Module.makeCert/httpInit/openUrl/pair/startStream/...`. On Tizen 5.0 there is
 * no WASM/EMSS; the native work runs in our service (org.moonlight.tizen5.engine)
 * reachable over the "moonlight" message port. This file replaces the Module-based
 * transport with message-port calls, keeping messages.js's sendMessage() contract.
 *
 * Load this AFTER platform/messages.js so it overrides SyncFunctions/AsyncFunctions
 * and sendMessage. UNTESTED on device (tizen.messageport availability + round-trips)
 * - validate once the hybrid package is installed.
 */
(function () {
  var ENGINE_APPID = 'org.moonlight.tizen5.engine';
  var ENGINE_PORT = 'moonlight';
  var LOCAL_PORT = 'moonlightui';

  var TRACE = !!window.ML_TRACE;   // set by mltrace.js in debug builds
  function trace() {
    if (!TRACE) return;
    var a = ['[mlbridge]'].concat(Array.prototype.slice.call(arguments));
    console.log.apply(console, a);
  }

  var localPort = null, remotePort = null, nextId = 1;
  var pending = {};   // id -> {resolve, reject, binary}

  function ensurePorts() {
    if (localPort && remotePort) return true;
    trace('ensurePorts: connecting', ENGINE_APPID, ENGINE_PORT);
    try {
      localPort = tizen.messageport.requestLocalMessagePort(LOCAL_PORT);
      localPort.addMessagePortListener(onReply);
      remotePort = tizen.messageport.requestRemoteMessagePort(ENGINE_APPID, ENGINE_PORT);
      trace('ensurePorts: ok');
      return true;
    } catch (e) {
      console.error('[mlbridge] message port setup failed:', e);
      return false;
    }
  }

  // Replies arrive as an array of {key,value}. We key everything by 'id'.
  function onReply(data /*, remoteMessagePort */) {
    var m = {};
    for (var i = 0; i < data.length; i++) m[data[i].key] = data[i].value;
    trace('reply id=' + m.id, 'keys=' + Object.keys(m).join(','), m.error ? 'error=' + m.error : '');
    if (m.event) {              // async push (streamTerminated/streamFailed)
      trace('event', m.event);
      if (typeof window.handleMessage === 'function')
        window.handleMessage({ data: { callbackId: -1, type: 'event', ret: m.event } });
      return;
    }
    var p = pending[m.id];
    if (!p) return;
    delete pending[m.id];
    if (m.error) p.reject(m.error);
    else p.resolve(m);
  }

  // Send {cmd,id,...fields} and resolve with the reply map.
  function bridgeCall(cmd, fields) {
    return new Promise(function (resolve, reject) {
      if (!ensurePorts()) { reject('no message port'); return; }
      var id = String(nextId++);
      pending[id] = { resolve: resolve, reject: reject };
      var msg = [{ key: 'cmd', value: cmd }, { key: 'id', value: id }];
      var names = [];
      for (var k in fields) if (fields[k] !== undefined && fields[k] !== null) {
        msg.push({ key: k, value: String(fields[k]) });
        names.push(k);   // names only: cert/key/pin values must not be logged
      }
      trace('bridgeCall cmd=' + cmd, 'id=' + id, 'fields=' + names.join(','));
      try { remotePort.sendMessage(msg, localPort); }
      catch (e) { delete pending[id]; reject(e); }
    });
  }

  // --- override the command maps used by messages.js sendMessage() ---
  // SyncFunctions returned {type,ret} synchronously in WASM; here they are async,
  // so we also override sendMessage below to await uniformly.
  window.SyncFunctions = {
    makeCert: function () { return bridgeCall('makeCert', {}).then(function (r) { return { cert: r.cert, privateKey: r.key }; }); },
    httpInit: function (cert, key, uniqueId) { return bridgeCall('httpInit', { cert: cert, key: key, uniqueId: uniqueId }); },
    startRequest: function () {
      var a = arguments; // host, httpPort, w, h, fps, bitrate, rikey, rikeyid, appver, gfever, rtspurl, codecMode, ...
      return bridgeCall('startStream', {
        host: a[0], w: a[2], h: a[3], fps: a[4], bitrate: a[5], rikey: a[6], rikeyid: a[7],
        appVer: a[8], gfeVer: a[9], rtspUrl: a[10], codecMode: a[11]
      });
    },
    stopRequest: function () { return bridgeCall('stopStream', {}); },
    cancelRequest: function () { return bridgeCall('cancelRequest', {}); },
    toggleStats: function () { return bridgeCall('toggleStats', {}); },
  };
  window.AsyncFunctions = {
    openUrl: function (id, url, ppk, binary) { return bridgeCall('openUrl', { url: url, ppk: ppk, binary: binary }); },
    STUN: function () { return bridgeCall('stun', {}); },
    pair: function (major, address, httpPort, pin) { return bridgeCall('pair', { major: major, addr: address, port: httpPort, pin: pin }); },
    wakeOnLan: function (mac) { return bridgeCall('wakeOnLan', { mac: mac }); },
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
      if (method === 'makeCert') return r;                 // {cert, privateKey}
      if (method === 'openUrl') return r.data;             // response bytes/string
      if (method === 'pair') return r.serverCert;
      return r.result !== undefined ? r.result : r;
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
        ensurePorts();   // connect to the native engine service (lazy is fine)
        trace('boot: moduleDidLoad found =', typeof window.moduleDidLoad === 'function');
        if (typeof window.moduleDidLoad === 'function') window.moduleDidLoad();
      };
    }
  }, false);

  console.log('[mlbridge] installed (message-port transport to ' + ENGINE_APPID + ')');
})();
