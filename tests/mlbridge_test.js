// Host self-test for pkg/ui/mlbridge.js: node tests/mlbridge_test.js
var fs = require('fs'), vm = require('vm'), assert = require('assert');

var logs = [], timers = [], now = 0, tid = 1;
var domReady = null, remoteUp = false, hostDead = false, sentAt = 0, launches = 0, sent = [], listener = null, handled = [];
var win = { ML_TRACE: true, handleMessage: function (m) { handled.push(m); } };
var tizen = {
  ApplicationControl: function (op) { this.operation = op; },
  ApplicationControlData: function (k, v) { this.key = k; this.value = v; },
  application: {
    launchAppControl: function (ctl, id, ok) {
      launches++; assert.strictEqual(id, 'org.moonlight.host.app');
      assert.strictEqual(ctl.operation, 'http://tizen.org/appcontrol/operation/default');
      remoteUp = true; ok();
    },
    launch: function () { throw new Error('launch() must not be used when launchAppControl works'); }
  },
  messageport: {
    requestLocalMessagePort: function () { return { addMessagePortListener: function (f) { listener = f; } }; },
    requestRemoteMessagePort: function () {
      if (!remoteUp) { var e = new Error('x'); e.name = 'NotFoundError'; throw e; }
      return { sendMessage: function (m) { if (hostDead) throw new Error('gone'); sent.push(m); sentAt = now; } };
    }
  }
};
var sandbox = {
  window: win, tizen: tizen, Promise: Promise, Uint8Array: Uint8Array, ArrayBuffer: ArrayBuffer,
  document: { addEventListener: function (ev, f) { if (ev === 'DOMContentLoaded') domReady = f; } },
  console: { log: function () { logs.push([].join.call(arguments, ' ')); }, warn: function () {}, error: function () {} },
  setTimeout: function (f, ms) { timers.push({ id: tid, at: now + ms, f: f }); return tid++; },
  clearTimeout: function (id) { timers = timers.filter(function (t) { return t.id !== id; }); }
};
function advance(ms) {
  var end = now + ms;
  for (;;) {
    timers.sort(function (a, b) { return a.at - b.at; });
    if (!timers.length || timers[0].at > end) break;
    var t = timers.shift(); now = t.at; t.f();
  }
  now = end;
}
function tick() { return new Promise(function (r) { setImmediate(r); }); }
function fields(m) { var o = {}; m.forEach(function (kv) { o[kv.key] = kv.value; }); return o; }
function reply(id, o) {
  var d = [{ key: 'id', value: id }];
  for (var k in o) d.push({ key: k, value: o[k] });
  listener(d);
}

vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlbridge.js', 'utf8'), sandbox);
sandbox.window = win;
var tests = [];
function test(n, f) { tests.push([n, f]); }

test('queue, launch once, send after connect', async function () {
  var p = win.sendMessage('makeCert', []);
  assert.strictEqual(launches, 1);
  assert.strictEqual(sent.length, 0);
  advance(250);                       // retry connects (host is up after launch)
  assert.strictEqual(launches, 1);
  assert.strictEqual(sent.length, 1);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'makeCert');
  reply(f.id, { cert: 'CERTPEM', key: 'KEYPEM' });
  var c = await p;
  assert.strictEqual(c.cert, 'CERTPEM'); assert.strictEqual(c.privateKey, 'KEYPEM');
});

test('openUrl mapping and binary reply', async function () {
  sent.length = 0;
  var p = win.sendMessage('openUrl', ['https://h/serverinfo', 'PPKPEM', true, 5000]);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'openUrl'); assert.strictEqual(f.url, 'https://h/serverinfo');
  assert.strictEqual(f.ppk, 'PPKPEM'); assert.strictEqual(f.binary, 'true');
  reply(f.id, { data: [1, 2, 255] });
  var r = await p;
  assert.ok(r instanceof Uint8Array); assert.strictEqual(Array.prototype.join.call(r, ','), '1,2,255');
  var p2 = win.sendMessage('openUrl', ['u', 'k', false, 0]);
  reply(fields(sent[1]).id, { data: '<xml/>' });
  assert.strictEqual(await p2, '<xml/>');
});

test('startRequest mapping', async function () {
  sent.length = 0;
  var a = []; a[0] = 'host'; a[1] = 47989; a[2] = 1920; a[3] = 1080; a[4] = 60; a[5] = 20000; a[6] = 'abcd';
  a[7] = 7; a[8] = '7.1'; a[9] = '3.2'; a[10] = 'rtsp://x'; a[11] = 1; a[18] = '51Surround'; a[24] = 'HEVC'; a[25] = true;
  var p = win.sendMessage('startRequest', a);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'startStream');
  var want = { host: 'host', w: '1920', h: '1080', fps: '60', bitrate: '20000', rikey: 'abcd', rikeyid: '7',
    appVer: '7.1', gfeVer: '3.2', rtspUrl: 'rtsp://x', codecMode: '1', audioConfig: '51Surround', videoCodec: 'HEVC', hdr: 'true' };
  for (var k in want) assert.strictEqual(f[k], want[k], k);
  reply(f.id, {});
  assert.strictEqual(await p, undefined);
});

test('pair mapping and serverCert', async function () {
  sent.length = 0;
  var p = win.sendMessage('pair', [7, '1.2.3.4', 47989, '1234', 'uid']);
  var f = fields(sent[0]);
  assert.strictEqual(f.major, '7'); assert.strictEqual(f.addr, '1.2.3.4');
  assert.strictEqual(f.port, '47989'); assert.strictEqual(f.pin, '1234');
  reply(f.id, { serverCert: 'SC' });
  assert.strictEqual(await p, 'SC');
});

test('timeouts and late reply', async function () {
  sent.length = 0;
  var p = win.sendMessage('openUrl', ['u', 'k', false, 3000]);
  var id = fields(sent[0]).id;
  advance(3000);
  await assert.rejects(p, function (e) { return e === 'timeout'; });
  assert.strictEqual(sent.length, 2);
  assert.strictEqual(fields(sent[1]).cmd, 'cancelRequest');
  reply(id, { data: 'late' });        // ignored, must not throw
  var q = win.sendMessage('httpInit', ['c', 'k', 'u']);
  advance(15000);
  await assert.rejects(q, function (e) { return e === 'timeout'; });
  assert.strictEqual(sent.length, 3);   // no cancelRequest for non-openUrl
});

test('timeout starts at send, queue re-sends on reconnect, pending reply resolves', async function () {
  sent.length = 0;
  hostDead = true; remoteUp = false;           // host died: next send fails, port is gone
  var done = null;
  var p = win.sendMessage('httpInit', ['c', 'k', 'u']);
  p.then(function () { done = 'ok'; }, function (e) { done = 'err ' + e; });
  assert.strictEqual(sent.length, 0);
  advance(60000);                              // much longer than 15 s, but never sent
  await tick();
  assert.strictEqual(done, null, 'must not time out while queued');
  hostDead = false; remoteUp = true;           // host is back
  advance(4000);                               // next retry reconnects and flushes
  assert.strictEqual(sent.length, 1);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'httpInit');
  advance(sentAt + 14000 - now);               // 14 s after the send: still pending
  await tick();
  assert.strictEqual(done, null);
  reply(f.id, {});
  await tick();
  assert.strictEqual(done, 'ok', 'late but still pending reply resolves');
  var q = win.sendMessage('httpInit', ['c', 'k', 'u']);
  q.then(null, function () {});
  assert.strictEqual(sentAt, now);             // connected: sent at once
  advance(15000);                              // armed at send: 15 s after it -> timeout
  await assert.rejects(q, function (e) { return e === 'timeout'; });
});

test('upstream update check is disabled', async function () {
  var called = [];
  ['checkForAppUpdates', 'checkForAppUpdatesAtStartup', 'updateAppButton', 'updateAppDialog', 'fetchLatestRelease']
    .forEach(function (n) { win[n] = function () { called.push(n); }; });
  domReady();
  win.checkForAppUpdates(); win.checkForAppUpdatesAtStartup(); win.updateAppButton('9'); win.updateAppDialog('9', '');
  await assert.rejects(win.fetchLatestRelease());
  assert.strictEqual(called.length, 0, 'original update functions must not run: ' + called);
});

test('event passes string', async function () {
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  assert.strictEqual(handled.join('|'), 'streamTerminated: 0');
});

test('trace hides secrets', async function () {
  var all = logs.join('\n');
  assert.ok(logs.length > 0);
  ['CERTPEM', 'KEYPEM', 'PPKPEM', '1234'].forEach(function (s) { assert.strictEqual(all.indexOf(s), -1, s); });
});

(async function () {
  var fail = 0;
  for (var i = 0; i < tests.length; i++) {
    try { await tests[i][1](); console.log('ok   ' + tests[i][0]); }
    catch (e) { fail++; console.log('FAIL ' + tests[i][0] + ': ' + (e && e.stack || e)); }
  }
  process.exit(fail ? 1 : 0);
})();
