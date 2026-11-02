/* eslint-disable */
/*
 * mltrace.js - debug (trace) builds only. Forwards the page console to the host app
 * over the Tizen message port "mllog"; the host writes it to dlog tag MLUI.
 * The original console still runs (web inspector). Never throws.
 */
(function () {
  window.ML_TRACE = true;
  var HOST_APPID = 'org.moonlight.host.app', HOST_PORT = 'mllog', CAP = 500;
  var buf = [], remote = null;

  function fmt(a) {
    try {
      if (a instanceof Error) return a.stack || a.message;
      if (a !== null && typeof a === 'object') return JSON.stringify(a);
      return String(a);
    } catch (e) { return '[unprintable]'; }
  }

  function flush() {
    try {
      if (!remote) {
        if (typeof tizen === 'undefined' || !tizen.messageport) return;
        remote = tizen.messageport.requestRemoteMessagePort(HOST_APPID, HOST_PORT);
      }
      while (buf.length) {
        var m = buf[0];
        remote.sendMessage([{ key: 'level', value: m[0] }, { key: 'msg', value: m[1] }]);
        buf.shift();
      }
    } catch (e) { remote = null; }   // host not running yet: keep buffer, retry
  }

  function push(lvl, args) {
    try {
      var parts = [];
      for (var i = 0; i < args.length; i++) parts.push(fmt(args[i]));
      buf.push([lvl, parts.join(' ')]);
      if (buf.length > CAP) buf.shift();
      if (remote) flush();
    } catch (e) {}
  }

  ['log', 'info', 'warn', 'error', 'debug'].forEach(function (lvl) {
    var orig = console[lvl];
    console[lvl] = function () {
      push(lvl, arguments);
      try { if (orig) return orig.apply(console, arguments); } catch (e) {}
    };
  });

  window.addEventListener('error', function (e) {
    console.error('window.onerror:', e.message, e.filename + ':' + e.lineno);
  });
  window.addEventListener('unhandledrejection', function (e) {
    console.error('unhandledrejection:', e.reason);
  });

  setInterval(flush, 1000);
  console.log('[mltrace] trace build, console -> dlog tag MLUI');
  flush();
})();
