/*
 * MoonlightHost - Tizen .NET host for the no-root dev-cert path.
 *
 * On a retail (unrooted) Samsung TV only Web (.wgt) and .NET (.tpk) apps install, so
 * the installable unit is a dev-signed .NET .tpk; our native streaming engine ships
 * inside it as libmlengine.so and is driven over the stable C ABI (mlengine.h).
 *
 * KEY POINT (why not plain [DllImport]): 2018-2020 Samsung TVs block native code
 * loaded from the application folder. So we do NOT let the loader dlopen
 * libmlengine.so from lib/; instead we read its bytes and load it from an anonymous
 * in-memory fd (memfd_create -> write -> dlopen /proc/self/fd/N), the nuvio-native
 * pattern. Only OUR .so comes from memory; its dependencies (esplusplayer, ssl, curl,
 * ecore...) still resolve normally from the TV's /usr/lib.
 *
 * STATUS: the loader + ABI are the real mechanism; lifecycle/UI are placeholders.
 * Verify TFM/privileges against the on-device dev.moonlight.tvshell, and confirm the
 * sandbox permits memfd_create + dlopen(/proc/self/fd) on the target. See
 * ../DEV-CERT-PATH.md.
 */
using System;
using System.IO;
using System.Runtime.InteropServices;
using Tizen.Applications;

namespace MoonlightHost
{
    // Loads a shared object from memory (bypasses app-folder native-code blocking).
    internal static class MemLoader
    {
        private const uint MFD_CLOEXEC = 0x0001u;
        private const int  RTLD_NOW    = 0x0002;
        // Tizen 4/5 libc does NOT export memfd_create, so create the anonymous fd by
        // raw syscall. ARM (32-bit) __NR_memfd_create = 385. (nuvio samsung-tpk-legacy.)
        private const long SYS_memfd_create_arm = 385;

        [DllImport("libc", EntryPoint = "syscall", SetLastError = true)]
        private static extern int syscall_memfd(long number, string name, uint flags);
        [DllImport("libc", SetLastError = true)]  private static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
        [DllImport("libdl")]                       private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl")]                       public  static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl")]                       private static extern IntPtr dlerror();

        public static IntPtr LoadFromMemory(byte[] so)
        {
            // Route 1 (preferred): memfd. UEP blocks dlopen of a FILE (even signed) but
            // allows PROT_EXEC anonymous memory. The fd must stay open for process life.
            // Route 2 (fallback, nuvio's): a C# ELF32-ARM loader for TVs where memfd is
            // refused. NOT implemented here yet (memfd worked on 3 of nuvio's 4 test sets);
            // add if a target returns an mmap/dlopen error.
            int fd = syscall_memfd(SYS_memfd_create_arm, "mlengine", MFD_CLOEXEC);
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

            IntPtr h = dlopen("/proc/self/fd/" + fd, RTLD_NOW);
            if (h == IntPtr.Zero)
            {
                IntPtr err = dlerror();
                throw new InvalidOperationException("dlopen(memfd) failed: " + (err != IntPtr.Zero ? Marshal.PtrToStringAnsi(err) : "?"));
            }
            return h;
        }

        public static T GetDelegate<T>(IntPtr handle, string symbol) where T : class
        {
            IntPtr p = dlsym(handle, symbol);
            if (p == IntPtr.Zero) throw new EntryPointNotFoundException("dlsym: " + symbol);
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
            IntPtr h = MemLoader.LoadFromMemory(File.ReadAllBytes(soPath));
            Start   = MemLoader.GetDelegate<StartDelegate>(h,   "ml_engine_start");
            Running = MemLoader.GetDelegate<RunningDelegate>(h, "ml_engine_running");
            Wait    = MemLoader.GetDelegate<WaitDelegate>(h,    "ml_engine_wait");
            Stop    = MemLoader.GetDelegate<StopDelegate>(h,    "ml_engine_stop");
        }
    }

    internal sealed class App : CoreUIApplication
    {
        protected override void OnCreate()
        {
            base.OnCreate();
            try
            {
                string soPath = Path.Combine(DirectoryInfo.Resource, "lib", "libmlengine.so");
                Engine.Load(soPath);
                // TODO: real menu + pairing UI (reuse moonlight-tizen web UI, or NUI).
                string host = Environment.GetEnvironmentVariable("MOONLIGHT_HOST") ?? "192.168.123.3";
                int rc = Engine.Start(host, 0, null, 1920, 1080, 60, 20000, DirectoryInfo.Data);
                Tizen.Log.Info("MLHOST", $"engine loaded from memory; ml_engine_start(host={host}) rc={rc}");
            }
            catch (Exception e)
            {
                Tizen.Log.Error("MLHOST", "engine load/start failed: " + e);
            }
        }

        protected override void OnTerminate()
        {
            try { Engine.Stop?.Invoke(); } catch { }
            base.OnTerminate();
        }

        private static void Main(string[] args) => new App().Run(args);
    }
}
