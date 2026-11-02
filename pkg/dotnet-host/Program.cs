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
 * STATUS: the loader + ABI are the real mechanism; lifecycle/UI are placeholders.
 */
using System;
using System.IO;
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
        private const long SYS_memfd_create_arm = 385;

        [DllImport("libc", EntryPoint = "syscall", SetLastError = true)]
        private static extern int syscall_memfd(long number, string name, uint flags);
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

        public static void Load(string soPath)
        {
            byte[] bytes = File.ReadAllBytes(soPath);
            Log.Trace("engine .so " + soPath + " (" + bytes.Length + " bytes)");
            IntPtr h = MemLoader.LoadFromMemory(bytes);
            Start   = MemLoader.GetDelegate<StartDelegate>(h,   "ml_engine_start");
            Running = MemLoader.GetDelegate<RunningDelegate>(h, "ml_engine_running");
            Wait    = MemLoader.GetDelegate<WaitDelegate>(h,    "ml_engine_wait");
            Stop    = MemLoader.GetDelegate<StopDelegate>(h,    "ml_engine_stop");
        }
    }

    internal sealed class App : CoreUIApplication
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
                string soPath = Path.Combine(DirectoryInfo.Resource, "lib", "libmlengine.so");
                Engine.Load(soPath);
                // TODO: real menu + pairing UI (reuse moonlight-tizen web UI, or NUI).
                string host = Environment.GetEnvironmentVariable("MOONLIGHT_HOST") ?? "192.168.123.3";
                Log.Trace("Engine.Start host=" + host + " appId=0 pin=(null) 1920x1080@60 20000kbps datadir=" + DirectoryInfo.Data);
                int rc = Engine.Start(host, 0, null, 1920, 1080, 60, 20000, DirectoryInfo.Data);
                Log.Trace("Engine.Start rc=" + rc);
                Tizen.Log.Info("MLHOST", $"engine loaded from memory; ml_engine_start(host={host}) rc={rc}");
            }
            catch (Exception e)
            {
                Tizen.Log.Error("MLHOST", "engine load/start failed: " + e);
            }
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
