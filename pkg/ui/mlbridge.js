/* eslint-disable */
/*
 * mlbridge.js - transport shim for the Moonlight Tizen 5.0 web UI.
 *
 * moonlight-tizen's UI (platform/messages.js) calls the native WASM module via
 * `Module.makeCert/httpInit/openUrl/pair/startStream/...`. On Tizen 5.0 there is
 * no WASM/EMSS; the native work runs in the .NET host service app
 * (org.moonlight.host.app), reachable over the "moonlight" message port. The host
 * (a separate .tpk) is started by the TV at boot; the UI only connects and retries.
 * This file replaces the Module-based transport with message-port calls, keeping
 * messages.js's sendMessage() contract. Wire format: see pkg/ui/PROTOCOL.md.
 *
 * Load this AFTER platform/messages.js so it overrides SyncFunctions/AsyncFunctions
 * and sendMessage. Note: needs validation on a device (tizen.messageport
 * behaviour); host-side tests only cover tests/mlbridge_test.js.
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

  // index.js logs stored values, including the pairing private key ("cert" in the DB).
  // Mask PEM private keys in every console line (inspector, debug relay to dlog).
  // mlbridge.js loads before index.js, so this wrapper sees all of its logging.
  var PEM_KEY = /-----BEGIN ([A-Z ]*PRIVATE KEY)-----[\s\S]*?-----END \1-----/g;
  function redact(v) { return typeof v === 'string' ? v.replace(PEM_KEY, '-----BEGIN $1----- [redacted] -----END $1-----') : v; }
  ['log', 'info', 'warn', 'error', 'debug'].forEach(function (lvl) {
    var orig = console[lvl];
    if (typeof orig !== 'function') return;
    console[lvl] = function () {
      var a = [];
      for (var i = 0; i < arguments.length; i++) a.push(redact(arguments[i]));
      return orig.apply(console, a);
    };
  });

  var localPort = null, remotePort = null, nextId = 1;
  var pending = {};   // id -> {resolve, reject, timer, msg, cmd, timeoutMs, cancelOnTimeout}
  var queue = [];     // ids waiting for the remote port, in send order
  var retryTimer = null, retryDelay = 250;
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
      return false;
    }
  }

  // Tizen message ports reject a message over ~4 KB, so a large message is sent as
  // ordered parts {id, seq, total, field, chunk} (pkg/ui/PROTOCOL.md). The limits
  // leave room for the Bundle encoding overhead.
  var MAX_SINGLE = 1536, CHUNK_CHARS = 512;   // 512 chars <= 1536 UTF-8 bytes
  function utf8Len(v) { try { return unescape(encodeURIComponent(v)).length; } catch (e) { return v.length * 3; } }

  // msg: [{key,value}] with cmd and id first. Returns the list of messages to send.
  function split(msg) {
    var size = 0, i;
    for (i = 0; i < msg.length; i++) size += msg[i].key.length + utf8Len(msg[i].value);
    if (size <= MAX_SINGLE) return [msg];
    var parts = [];
    for (i = 2; i < msg.length; i++) {
      var v = msg[i].value, off = 0;
      do { parts.push({ field: msg[i].key, chunk: v.substr(off, CHUNK_CHARS) }); off += CHUNK_CHARS; } while (off < v.length);
    }
    return parts.map(function (pt, seq) {
      return [msg[0], msg[1], { key: 'seq', value: String(seq) }, { key: 'total', value: String(parts.length) },
              { key: 'field', value: pt.field }, { key: 'chunk', value: pt.chunk }];
    });
  }

  // Collects the parts of a chunked reply; returns the joined map once all arrived.
  var partial = {};   // id -> {total, n, seqs: {seq: {field, chunk}}}
  function join(m) {
    var a = partial[m.id] || (partial[m.id] = { total: +m.total, n: 0, seqs: {} });
    if (!a.seqs[m.seq]) { a.seqs[m.seq] = { field: m.field, chunk: m.chunk }; a.n++; }
    if (a.n < a.total) return null;
    delete partial[m.id];
    var out = { id: m.id };
    for (var i = 0; i < a.total; i++) {
      var pt = a.seqs[i], prev = out[pt.field];
      if (prev === undefined) out[pt.field] = pt.chunk;
      else if (typeof prev === 'string') out[pt.field] = prev + pt.chunk;
      else out[pt.field] = prev.concat(Array.prototype.slice.call(pt.chunk));   // byte chunks
    }
    trace('joined id=' + m.id, 'parts=' + a.total);
    return out;
  }

  // Send one pending command. Its timeout is armed only after sendMessage succeeded,
  // so time spent waiting for the host to start does not count against it.
  function sendNow(id) {
    var p = pending[id];
    if (!p) return;
    try { for (var i = 0; i < p.msgs.length; i++) remotePort.sendMessage(p.msgs[i], localPort); }
    catch (e) {   // host gone: forget the port, keep the command first in the queue, reconnect
      trace('send failed id=' + id, e && (e.name || e.message || e));
      remotePort = null;
      queue.unshift(id);
      scheduleRetry();
      return;
    }
    trace('sent cmd=' + p.cmd, 'id=' + id, p.msgs.length > 1 ? 'parts=' + p.msgs.length : '');
    if (p.timeoutMs > 0) p.timer = setTimeout(function () { onTimeout(id); }, p.timeoutMs);
  }

  function onTimeout(id) {
    var p = pending[id];
    if (!p) return;
    delete pending[id];
    trace('timeout cmd=' + p.cmd, 'id=' + id);
    if (p.cancelOnTimeout) bridgeCall('cancelRequest', {}, 0).then(null, function () {});
    p.reject('timeout');
  }

  function flush() {
    trace('flush queued=' + queue.length);
    while (queue.length && remotePort) sendNow(queue.shift());
  }

  // Connect now, or retry with backoff (250 ms doubling up to 4 s); one timer at a time.
  function connect() {
    if (retryTimer) return;
    if (ensurePorts()) { retryDelay = 250; flush(); return; }
    scheduleRetry();
  }

  function scheduleRetry() {
    if (retryTimer) return;
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
    if (m.total !== undefined) { m = join(m); if (!m) return; }
    trace('reply id=' + m.id, 'keys=' + Object.keys(m).join(','), m.error ? 'error=' + m.error : '');
    if (m.log !== undefined) {   // forwarded host/engine log line (on demand, see mlDebug)
      console.log('[' + (m.tag || 'host') + '] ' + m.log);
      return;
    }
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
      pending[id] = { resolve: resolve, reject: reject, timer: null, msgs: split(msg), cmd: cmd,
                      timeoutMs: timeoutMs, cancelOnTimeout: cancelOnTimeout };
      trace('call cmd=' + cmd, 'id=' + id, 'fields=' + names.join(','));
      if (remotePort && !queue.length) sendNow(id);
      else { trace('queue id=' + id); queue.push(id); connect(); }
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
  // in-stream stats overlay that reads WASM memory is not available here.
  window.Module = {
    sendKeyboardEvent: function (keyCode, keyAction, modifiers) {
      return bridgeCall('sendKeyboardEvent', { key: keyCode, action: keyAction, mods: modifiers });
    },
    stopStream: function () { return bridgeCall('stopStream', {}); },
    toggleStats: function () { return bridgeCall('toggleStats', {}); },
    HEAPF32: { buffer: new ArrayBuffer(0) },   // stub; perf-stats overlay disabled
    HEAPF64: { buffer: new ArrayBuffer(0) },
  };

  // Update check: moonlight-tizen's index.js asks GitHub for ITS latest release and
  // offers that package as an update. Those packages do not fit this client, so the
  // check is off. The functions stay in index.js; to point them at this project's
  // releases later, set __REPO_OWNER__/__REPO_NAME__ in pkg/build-package.sh (index.js
  // reads them in fetchLatestRelease), keep config.xml's version in step with the
  // release tags (checkVersionUpdate compares against it) and set UPDATE_CHECK = true.
  var UPDATE_CHECK = false;
  function disableUpdateCheck() {
    if (UPDATE_CHECK) return;
    ['checkForAppUpdates', 'checkForAppUpdatesAtStartup', 'updateAppButton', 'updateAppDialog'].forEach(function (n) {
      window[n] = function () { trace('update check disabled:', n); };
    });
    window.fetchLatestRelease = function () { return Promise.reject(new Error('update check disabled')); };
  }

  // Settings > About shows "Moonlight v<version> (<build>)": which fork, variant,
  // commit and base UI version (build-package.sh fills ML_BUILD). index.js would show
  // only the version, or the base UI's commit in debug.
  var ML_BUILD = '__ML_BUILD__';
  function showBuild() {
    window.getBuildVersion = function (version) {
      return version + ' (' + (ML_BUILD.indexOf('__') === 0 ? 'tizen50-core dev' : ML_BUILD) + ')';
    };
  }

  // On-demand host/engine log in this console (for example Apps2Samsung's debug
  // console): mlDebug(true) / mlDebug(false). Remembered across restarts. The dlog
  // tags (MLHOST, MLENGINE, MLUI) are written as before either way.
  var DEBUG_KEY = 'mlDebug';
  // On by default in the debug packages (mltrace.js sets ML_TRACE); an explicit
  // mlDebug(false) is remembered and wins.
  function debugStored() {
    var v = null;
    try { v = localStorage.getItem(DEBUG_KEY); } catch (e) {}
    return v === '1' || (v === null && TRACE);
  }
  window.mlDebug = function (on) {
    on = !!on;
    try { localStorage.setItem(DEBUG_KEY, on ? '1' : '0'); } catch (e) {}
    call('setLog', { on: String(on) }).then(null, function (e) { console.warn('[mlbridge] setLog failed:', e); });
    return 'host/engine log in this console: ' + (on ? 'on' : 'off');
  };

  // Boot without the WASM engine. The original starts the UI from the compiled
  // module: common.js on DOMContentLoaded calls createWASMModule() -> loads
  // moonlight-wasm.js -> (on WASM ready) window.moduleDidLoad(), which starts the
  // app. We ship NO moonlight-wasm.js (the engine is the native service), so that
  // chain would never fire moduleDidLoad and the UI would hang. mlbridge.js is parsed
  // before common.js, so this DOMContentLoaded listener runs first and replaces
  // common.domContentLoaded to skip the WASM load and fire moduleDidLoad() ourselves.
  document.addEventListener('DOMContentLoaded', function () {
    disableUpdateCheck();   // index.js is loaded now; runs before attachListeners binds the button
    showBuild();
    trace('DOMContentLoaded: common.domContentLoaded exists =',
          !!(window.common && typeof window.common.domContentLoaded === 'function'));
    if (window.common && typeof window.common.domContentLoaded === 'function') {
      window.common.domContentLoaded = function () {
        try { if (window.common.attachDefaultListeners) window.common.attachDefaultListeners(); } catch (e) {}
        connect();   // start/connect the host service (commands queue until connected)
        if (debugStored()) window.mlDebug(true);
        trace('boot: moduleDidLoad found =', typeof window.moduleDidLoad === 'function');
        if (typeof window.moduleDidLoad === 'function') window.moduleDidLoad();
      };
    }
  }, false);

  console.log('[mlbridge] installed (message-port transport to ' + ENGINE_APPID + ')');
})();
