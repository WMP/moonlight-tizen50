/*
 * MoonlightHost - Tizen .NET host for the dev-cert path.
 *
 * On a retail (unrooted) Samsung TV only Web (.wgt) and .NET (.tpk) apps install, so
 * the installable unit is a dev-signed .NET .tpk; our native streaming engine ships
 * inside it as libmlengine.so and is driven over the stable C ABI (mlengine.h).
 *
 * The engine is loaded from memory rather than from a file in the app folder: we
 * read libmlengine.so's bytes and load them from an anonymous in-memory fd
 * (memfd_create -> write -> dlopen /proc/self/fd/N). This is also how a downloaded
 * engine update is applied without rewriting the app folder. Only OUR .so comes from
 * memory; its dependencies (esplusplayer, ssl, curl, ecore...) resolve normally from
 * the TV's /usr/lib.
 *
 * The host is a background service app: a second UI app would be closed when the
 * web UI comes to the foreground. It serves the UI command port "moonlight" (see
 * pkg/ui/PROTOCOL.md): commands from the web UI are run on the engine and answered
 * on the UI port "moonlightui".
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using Tizen.Applications;

namespace MoonlightHost
{
    // Verbose host trace: compiled out of release builds ([Conditional] drops the calls).
    internal static class Log
    {
        [System.Diagnostics.Conditional("DEBUG")]
        public static void Trace(string m) => Tizen.Log.Debug("MLHOST", m);
    }

    // Loads a shared object from memory (lets downloaded engine updates apply
    // without rewriting the app folder).
    internal static class MemLoader
    {
        private const uint MFD_CLOEXEC = 0x0001u;
        private const int  RTLD_NOW    = 0x0002;
        // Tizen 4/5 libc does NOT export memfd_create, so create the anonymous fd by
        // raw syscall. ARM (32-bit) __NR_memfd_create = 385.
        private const int  SYS_memfd_create_arm = 385;

        [DllImport("libc", EntryPoint = "syscall", SetLastError = true)]
        // number is C long = 32-bit on ARM; a C# long (64-bit) shifts name/flags into the wrong registers -> EINVAL.
        private static extern int syscall_memfd(int number, string name, uint flags);
        [DllImport("libc", SetLastError = true)]  private static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
        [DllImport("libdl")]                       private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl")]                       public  static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl")]                       private static extern IntPtr dlerror();

        public static IntPtr LoadFromMemory(byte[] so)
        {
            // Route 1 (preferred): memfd - load the engine image from an anonymous fd.
            // The fd must stay open for process life.
            // Route 2 (fallback): a C# ELF32-ARM loader for targets where memfd is
            // refused. NOT implemented here yet; add if a target returns an mmap error.
            int fd = syscall_memfd(SYS_memfd_create_arm, "mlengine", MFD_CLOEXEC);
            Log.Trace("memfd fd=" + fd);
            if (fd < 0) throw new InvalidOperationException("memfd_create(syscall 385) failed: " + Marshal.GetLastWin32Error());

            var pin = GCHandle.Alloc(so, GCHandleType.Pinned);
            try
            {
                IntPtr basePtr = pin.AddrOfPinnedObject();
                long total = 0;
                while (total < so.Length)
                {
                    IntPtr n = write(fd, basePtr + (int)total, (UIntPtr)(so.Length - total));
                    long w = n.ToInt64();
                    if (w <= 0) throw new IOException("write to memfd failed: " + Marshal.GetLastWin32Error());
                    total += w;
                }
            }
            finally { pin.Free(); }
            Log.Trace("wrote " + so.Length + " bytes to memfd " + fd);

            IntPtr h = dlopen("/proc/self/fd/" + fd, RTLD_NOW);
            if (h == IntPtr.Zero)
            {
                IntPtr err = dlerror();
                throw new InvalidOperationException("dlopen(memfd) failed: " + (err != IntPtr.Zero ? Marshal.PtrToStringAnsi(err) : "?"));
            }
            Log.Trace("dlopen handle=0x" + h.ToInt64().ToString("x"));
            return h;
        }

        public static T GetDelegate<T>(IntPtr handle, string symbol) where T : class
        {
            IntPtr p = dlsym(handle, symbol);
            if (p == IntPtr.Zero) throw new EntryPointNotFoundException("dlsym: " + symbol);
            Log.Trace("dlsym " + symbol + " -> 0x" + p.ToInt64().ToString("x"));
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }
    }

    // The engine C ABI (mlengine.h), bound at runtime from the in-memory .so.
    internal static class Engine
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  StartDelegate(string host, int appId, string pin,
                                           int width, int height, int fps, int bitrateKbps, string datadir);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  RunningDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void WaitDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void StopDelegate();

        public static StartDelegate   Start;
        public static RunningDelegate Running;
        public static WaitDelegate    Wait;
        public static StopDelegate    Stop;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  MakeCertDelegate(out IntPtr certPem, out IntPtr keyPem);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  HttpInitDelegate(string certPem, string keyPem, string uniqueId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  OpenUrlDelegate(string url, string ppk, out IntPtr data, out int len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  PairDelegate(int serverMajor, string address, int httpPort, string pin, out IntPtr serverCertPem);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CancelRequestDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  StartSessionDelegate(string host, int width, int height, int fps, int bitrateKbps,
                                                  string rikeyHex, int rikeyId, string appVersion,
                                                  string gfeVersion, string rtspUrl,
                                                  int serverCodecModeSupport, string videoCodec, int hdr,
                                                  string audioConfig);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SendKeyDelegate(int keyCode, int keyAction, int modifiers);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void FreeDelegate(IntPtr p);

        public static MakeCertDelegate      MakeCert;
        public static HttpInitDelegate      HttpInit;
        public static OpenUrlDelegate       OpenUrl;
        public static PairDelegate          Pair;
        public static CancelRequestDelegate CancelRequest;
        public static StartSessionDelegate  StartSession;
        public static SendKeyDelegate       SendKey;
        public static FreeDelegate          Free;
        public static volatile bool         Loaded;

        public static void Load(string soPath)
        {
            byte[] bytes = File.ReadAllBytes(soPath);
            Log.Trace("engine .so " + soPath + " (" + bytes.Length + " bytes)");
            IntPtr h = MemLoader.LoadFromMemory(bytes);
            Start   = MemLoader.GetDelegate<StartDelegate>(h,   "ml_engine_start");
            Running = MemLoader.GetDelegate<RunningDelegate>(h, "ml_engine_running");
            Wait    = MemLoader.GetDelegate<WaitDelegate>(h,    "ml_engine_wait");
            Stop    = MemLoader.GetDelegate<StopDelegate>(h,    "ml_engine_stop");
            MakeCert      = MemLoader.GetDelegate<MakeCertDelegate>(h,      "ml_engine_make_cert");
            HttpInit      = MemLoader.GetDelegate<HttpInitDelegate>(h,      "ml_engine_http_init");
            OpenUrl       = MemLoader.GetDelegate<OpenUrlDelegate>(h,       "ml_engine_open_url");
            Pair          = MemLoader.GetDelegate<PairDelegate>(h,          "ml_engine_pair");
            CancelRequest = MemLoader.GetDelegate<CancelRequestDelegate>(h, "ml_engine_cancel_request");
            StartSession  = MemLoader.GetDelegate<StartSessionDelegate>(h,  "ml_engine_start_session");
            SendKey       = MemLoader.GetDelegate<SendKeyDelegate>(h,       "ml_engine_send_key");
            Free          = MemLoader.GetDelegate<FreeDelegate>(h,          "ml_engine_free");
            Loaded = true;
        }
    }

    // UI command port "moonlight" (pkg/ui/PROTOCOL.md): runs commands, sends replies and events.
    internal static class CommandPort
    {
        private const string DefaultApp  = "Moonlight5.ui";
        private const string DefaultPort = "moonlightui";

        private static Tizen.Applications.Messages.MessagePort s_port;  // kept for process life
        private static System.Threading.Timer s_monitor;
        private static readonly object s_httpLock = new object();  // libgamestream has one global cancel flag
        private static volatile bool s_active;
        private static string s_uiApp = DefaultApp;
        private static string s_uiPort = DefaultPort;

        public static void Start()
        {
            s_port = new Tizen.Applications.Messages.MessagePort("moonlight", false);
            s_port.MessageReceived += OnMessage;
            s_port.Listen();
            s_monitor = new System.Threading.Timer(_ => Poll(), null, 500, 500);
            Log.Trace("command port 'moonlight' listening");
        }

        private static void Poll()
        {
            try
            {
                if (s_active && Engine.Loaded && Engine.Running() == 0)
                {
                    s_active = false;
                    SendEvent("streamTerminated: 0");
                }
            }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "monitor failed: " + ex.Message); }
        }

        private static string Item(Tizen.Applications.Bundle b, string k)
            => b.Contains(k) ? b.GetItem<string>(k) : null;

        private static void OnMessage(object sender, Tizen.Applications.Messages.MessageReceivedEventArgs e)
        {
            try
            {
                var b = e.Message;
                string app = e.Remote?.AppId, port = e.Remote?.PortName;
                if (!string.IsNullOrEmpty(app)) s_uiApp = app;
                if (!string.IsNullOrEmpty(port)) s_uiPort = port;
                string id = Item(b, "id");
                string cmd = Item(b, "cmd");
                if (id == null) { Tizen.Log.Error("MLHOST", "command without id ignored: " + cmd); return; }
                if (Item(b, "total") != null) { b = Join(id, b); if (b == null) return; }   // not all parts yet
                Log.Trace("cmd " + cmd + " id=" + id);
                string replyApp = s_uiApp, replyPort = s_uiPort;
                Task.Run(() => Run(b, cmd ?? "", id, replyApp, replyPort));
            }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "command receive failed: " + ex.Message); }
        }

        private static void Run(Tizen.Applications.Bundle b, string cmd, string id, string app, string port)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var reply = new Dictionary<string, object>();
            string err;
            try { err = Execute(b, cmd, reply); }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "cmd " + cmd + " failed: " + ex); err = "exception"; }
            if (err != null) { reply.Clear(); reply["error"] = err; }   // drop partial results
            SendReply(id, reply, app, port);
            Log.Trace("reply " + id + (err == null ? " ok " : " err ") + sw.ElapsedMilliseconds + "ms");

            if (cmd == "startStream")
            {
                if (err == null) { s_active = true; SendEvent("Connection Established"); }
                else SendEvent("streamTerminated: -1");
            }
        }

        private static bool Send(Tizen.Applications.Bundle msg, string app, string port)
        {
            try { s_port.Send(msg, app, port); return true; }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "send failed: " + ex.Message); return false; }
        }

        // A message port message is limited to ~4 KB, so a large message travels as
        // ordered parts {id, seq, total, field, chunk} (pkg/ui/PROTOCOL.md). The sizes
        // leave room for the Bundle encoding overhead.
        private const int MaxSingle = 1536, ChunkBytes = 1536, ChunkChars = 512;   // 512 chars <= 1536 UTF-8 bytes

        private static void SendReply(string id, Dictionary<string, object> r, string app, string port)
        {
            int size = 0;
            foreach (var kv in r)
                size += kv.Key.Length + (kv.Value is byte[] ? ((byte[])kv.Value).Length : Encoding.UTF8.GetByteCount((string)kv.Value));
            if (size <= MaxSingle)
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("id", id);
                foreach (var kv in r)
                    if (kv.Value is byte[]) m.AddItem(kv.Key, (byte[])kv.Value); else m.AddItem(kv.Key, (string)kv.Value);
                Send(m, app, port);
                return;
            }
            var parts = new List<KeyValuePair<string, object>>();
            foreach (var kv in r)
            {
                var bytes = kv.Value as byte[];
                int off = 0;
                if (bytes != null)
                    do
                    {
                        int n = Math.Min(ChunkBytes, bytes.Length - off);
                        var c = new byte[n];
                        Array.Copy(bytes, off, c, 0, n);
                        parts.Add(new KeyValuePair<string, object>(kv.Key, c));
                        off += n;
                    } while (off < bytes.Length);
                else
                {
                    string str = (string)kv.Value;
                    do
                    {
                        int n = Math.Min(ChunkChars, str.Length - off);
                        if (n > 1 && off + n < str.Length && char.IsHighSurrogate(str[off + n - 1])) n--;   // keep pairs whole
                        parts.Add(new KeyValuePair<string, object>(kv.Key, str.Substring(off, n)));
                        off += n;
                    } while (off < str.Length);
                }
            }
            for (int seq = 0; seq < parts.Count; seq++)
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("id", id);
                m.AddItem("seq", seq.ToString());
                m.AddItem("total", parts.Count.ToString());
                m.AddItem("field", parts[seq].Key);
                if (parts[seq].Value is byte[]) m.AddItem("chunk", (byte[])parts[seq].Value);
                else m.AddItem("chunk", (string)parts[seq].Value);
                if (!Send(m, app, port)) return;   // the UI times out the command
            }
            Log.Trace("reply " + id + " sent in " + parts.Count + " parts");
        }

        private sealed class Partial
        {
            public int Total;
            public readonly Dictionary<int, KeyValuePair<string, string>> Seqs = new Dictionary<int, KeyValuePair<string, string>>();
        }
        private static readonly Dictionary<string, Partial> s_partial = new Dictionary<string, Partial>();

        // Collects the parts of a chunked command; returns the joined command once all arrived.
        private static Tizen.Applications.Bundle Join(string id, Tizen.Applications.Bundle b)
        {
            int seq, total;
            if (!int.TryParse(Item(b, "seq"), out seq) || !int.TryParse(Item(b, "total"), out total) ||
                seq < 0 || seq >= total || Item(b, "field") == null)
            {
                Tizen.Log.Error("MLHOST", "bad command part for id=" + id);
                return null;
            }
            Partial a;
            lock (s_partial)
            {
                if (!s_partial.TryGetValue(id, out a)) { a = new Partial { Total = total }; s_partial[id] = a; }
                if (!a.Seqs.ContainsKey(seq)) a.Seqs[seq] = new KeyValuePair<string, string>(Item(b, "field"), Item(b, "chunk") ?? "");
                if (a.Seqs.Count < a.Total) return null;
                s_partial.Remove(id);
            }
            var fieldsJoined = new Dictionary<string, StringBuilder>();
            for (int i = 0; i < a.Total; i++)
            {
                KeyValuePair<string, string> pt;
                if (!a.Seqs.TryGetValue(i, out pt)) { Tizen.Log.Error("MLHOST", "command part missing for id=" + id); return null; }
                StringBuilder sb;
                if (!fieldsJoined.TryGetValue(pt.Key, out sb)) { sb = new StringBuilder(); fieldsJoined[pt.Key] = sb; }
                sb.Append(pt.Value);
            }
            var joined = new Tizen.Applications.Bundle();
            joined.AddItem("cmd", Item(b, "cmd") ?? "");
            joined.AddItem("id", id);
            foreach (var kv in fieldsJoined) joined.AddItem(kv.Key, kv.Value.ToString());
            Log.Trace("joined id=" + id + " parts=" + a.Total);
            return joined;
        }

        private static void SendEvent(string ev)
        {
            var m = new Tizen.Applications.Bundle();
            m.AddItem("event", ev);
            Send(m, s_uiApp, s_uiPort);
        }

        private static string Take(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            string s = Marshal.PtrToStringAnsi(p);
            Engine.Free(p);
            return s;
        }

        private static string Rc(int rc) => rc == 0 ? null : rc.ToString();

        // Returns null on success (result fields in r) or the error string. Never logs secrets.
        private static string Execute(Tizen.Applications.Bundle b, string cmd, Dictionary<string, object> r)
        {
            if (!Engine.Loaded) return "engine not loaded";
            switch (cmd)
            {
                case "makeCert":
                    lock (s_httpLock)
                    {
                        IntPtr c, k;
                        int rc = Engine.MakeCert(out c, out k);
                        string cert = Take(c), key = Take(k);
                        if (rc != 0) return rc.ToString();
                        r.Add("cert", cert ?? ""); r.Add("key", key ?? "");
                        return null;
                    }
                case "httpInit":
                    lock (s_httpLock)
                        return Rc(Engine.HttpInit(Item(b, "cert"), Item(b, "key"), Item(b, "uniqueId")));
                case "openUrl":
                    lock (s_httpLock)
                    {
                        IntPtr p; int len;
                        int rc = Engine.OpenUrl(Item(b, "url"), Item(b, "ppk"), out p, out len);
                        byte[] data = new byte[0];
                        if (p != IntPtr.Zero)
                        {
                            if (len > 0) { data = new byte[len]; Marshal.Copy(p, data, 0, len); }
                            Engine.Free(p);
                        }
                        if (rc != 0) return rc.ToString();
                        if (Item(b, "binary") == "true") r.Add("data", data);
                        else r.Add("data", Encoding.UTF8.GetString(data));
                        return null;
                    }
                case "pair":
                    {
                        int major, port;
                        if (!int.TryParse(Item(b, "major"), out major) || !int.TryParse(Item(b, "port"), out port)) return "bad args";
                        lock (s_httpLock)
                        {
                            IntPtr c;
                            int rc = Engine.Pair(major, Item(b, "addr"), port, Item(b, "pin"), out c);
                            string cert = Take(c);
                            if (rc != 0) return rc.ToString();
                            r.Add("serverCert", cert ?? "");
                            return null;
                        }
                    }
                case "cancelRequest": Engine.CancelRequest(); return null;
                case "startStream":
                    {
                        int w, h, fps, br, rikeyid, codec;
                        if (!int.TryParse(Item(b, "w"), out w) || !int.TryParse(Item(b, "h"), out h) ||
                            !int.TryParse(Item(b, "fps"), out fps) || !int.TryParse(Item(b, "bitrate"), out br) ||
                            !int.TryParse(Item(b, "rikeyid"), out rikeyid))
                            return "bad args";
                        if (!int.TryParse(Item(b, "codecMode"), out codec)) codec = 0;   // optional: engine defaults to H.264
                        return Rc(Engine.StartSession(Item(b, "host"), w, h, fps, br, Item(b, "rikey"), rikeyid,
                                                      Item(b, "appVer"), Item(b, "gfeVer"), Item(b, "rtspUrl"),
                                                      codec, Item(b, "videoCodec"), Item(b, "hdr") == "true" ? 1 : 0,
                                                      Item(b, "audioConfig")));
                    }
                case "stopStream": Engine.Stop(); return null;
                case "sendKeyboardEvent":
                    {
                        int key, action, mods;
                        if (!int.TryParse(Item(b, "key"), out key) || !int.TryParse(Item(b, "action"), out action) ||
                            !int.TryParse(Item(b, "mods"), out mods)) return "bad args";
                        Engine.SendKey(key, action, mods);
                        return null;
                    }
                case "toggleStats": return null;
                case "stun": r.Add("result", ""); return null;
                case "wakeOnLan": return "not supported";
                default: return "unknown cmd " + cmd;
            }
        }
    }

    internal sealed class App : ServiceApplication
    {
#if DEBUG
        // Relays the web UI console (mltrace.js) to dlog tag MLUI. Kept for process life.
        private static Tizen.Applications.Messages.MessagePort s_uiLog;

        private static void StartUiLog()
        {
            s_uiLog = new Tizen.Applications.Messages.MessagePort("mllog", false);
            s_uiLog.MessageReceived += (sender, e) =>
            {
                try
                {
                    var b = e.Message;
                    string level = b.Contains("level") ? b.GetItem<string>("level") : null;
                    string msg = b.Contains("msg") ? b.GetItem<string>("msg") : "";
                    if (level == "error") Tizen.Log.Error("MLUI", msg);
                    else if (level == "warn") Tizen.Log.Warn("MLUI", msg);
                    else Tizen.Log.Info("MLUI", msg);
                }
                catch (Exception ex) { Tizen.Log.Error("MLHOST", "ui log message failed: " + ex.Message); }
            };
            s_uiLog.Listen();
            Log.Trace("ui log port 'mllog' listening");
        }
#endif

        protected override void OnCreate()
        {
            base.OnCreate();
#if DEBUG
            try { StartUiLog(); }
            catch (Exception e) { Tizen.Log.Error("MLHOST", "ui log listener failed: " + e); }
#endif
            Log.Trace("OnCreate start");
            try
            {
                // The SDK packs the csproj Link "res/lib/..." next to the assembly (bin/res/lib/),
                // not under the app's res/ directory, so resolve it from the assembly folder.
                string soPath = Path.Combine(Path.GetDirectoryName(typeof(App).Assembly.Location),
                                             "res", "lib", "libmlengine.so");
                Engine.Load(soPath);
                // No session starts here: the web UI drives the engine over the message port.
                Tizen.Log.Info("MLHOST", "engine loaded from memory: " + soPath);
            }
            catch (Exception e)
            {
                Tizen.Log.Error("MLHOST", "engine load failed: " + e);
            }
            // Also after a failed load: every command then answers "engine not loaded".
            try { CommandPort.Start(); }
            catch (Exception e) { Tizen.Log.Error("MLHOST", "command port failed: " + e); }
        }

        protected override void OnTerminate()
        {
            Log.Trace("OnTerminate");
            try { Log.Trace("Engine.Stop"); Engine.Stop?.Invoke(); } catch { }
            base.OnTerminate();
        }

        private static void Main(string[] args) => new App().Run(args);
    }
}
