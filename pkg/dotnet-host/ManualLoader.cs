/*
 * ManualLoader.cs - fallback ELF32-ARM image loader for targets that restrict
 * dlopen of a mapped file.
 *
 * On a web-launched host process fd-backed PROT_EXEC mmap is refused for every
 * engine route (packaged file, realpath, memfd, data-dir copy: all EPERM),
 * while anonymous RW memory made executable with mprotect still works - the
 * JIT of the .NET runtime runs on exactly that mechanism in the same process
 * (Diag.cs probe "anon-exec"). This loader uses it: it maps the image into
 * plain anonymous memory, copies the PT_LOAD segments, applies the
 * relocations, resolves the imports from the already-loaded dependencies,
 * runs the initializers, then resolves the exports by name from the image's
 * own symbol table.
 *
 * The engine .so is built to fit this path (verified against the binary):
 * no TLS, no DT_TEXTREL, SysV hash present (DT_HASH), and only the four ARM
 * reloc types below. There is no host-runnable test for this; it is verified
 * on-device via `sdb dlog MLHOST`.
 *
 * shortcut: image not registered with ld.so (no dl_iterate_phdr/dladdr/EXIDX unwind), fine while the engine does not throw C++ exceptions; upgrade = load via ld.so.
 *
 * Split out of Program.cs (MemLoader) - kept as a separate file so the core
 * upstream stays clean and the overlay carries it.
 */
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace MoonlightHost
{
    internal static class ManualLoader
    {
        private const int PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
        private const int MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20;
        private const int RTLD_NOW = 0x0002;
        private const int _SC_PAGESIZE = 30;                         // glibc Linux value
        private const int SHN_ABS = 0xFFF1;
        private const int ARM_NR_CACHEFLUSH = 0xF0002;               // __ARM_NR_cacheflush

        [DllImport("libc", SetLastError = true)]  private static extern IntPtr mmap(IntPtr addr, UIntPtr length, int prot, int flags, int fd, IntPtr offset);
        [DllImport("libc", SetLastError = true)]  private static extern int    mprotect(IntPtr addr, UIntPtr length, int prot);
        [DllImport("libc", SetLastError = true)]  private static extern int    munmap(IntPtr addr, UIntPtr length);
        [DllImport("libc", SetLastError = true)]  private static extern int    syscall(int number, IntPtr start, IntPtr end, int flags);
        [DllImport("libc")]                       private static extern int    sysconf(int name);
        [DllImport("libdl.so.2", SetLastError = true)]  private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlvsym(IntPtr handle, string symbol, string version);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlerror();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void InitFn(int argc, IntPtr argv, IntPtr envp);

        private static ManualImage s_image;   // kept for process life; its mmap is never unmapped

        // The manually mapped image, when the last load used this loader (else null).
        public static ManualImage Image { get { return s_image; } }

        private static readonly long Page = PageSize();

        private static long PageSize() { long p = sysconf(_SC_PAGESIZE); return p > 0 ? p : 4096; }
        private static long PageDown(long v) => v & ~(Page - 1);
        private static long PageUp(long v)   => (v + Page - 1) & ~(Page - 1);

        // long -> IntPtr that wraps instead of throwing when an address is above 0x7fffffff (32-bit only).
        private static IntPtr Ptr(long v) => (IntPtr)unchecked((int)v);

        // Address of a defined symbol: SHN_ABS values are absolute, all others move with the image.
        private static IntPtr SymAddr(long bias, ushort shndx, uint value) => shndx == SHN_ABS ? Ptr(value) : Ptr(bias + value);

        private static string CStr(byte[] b, int off)
        {
            int n = off;
            while (n < b.Length && b[n] != 0) n++;
            return Encoding.ASCII.GetString(b, off, n - off);
        }

        // A manually mapped image: resolves its exported symbols by name from a table built at load.
        public sealed class ManualImage
        {
            public readonly IntPtr Base;
            private readonly Dictionary<string, IntPtr> _exports;

            public ManualImage(IntPtr @base, Dictionary<string, IntPtr> exports)
            { Base = @base; _exports = exports; }

            public IntPtr Resolve(string name)
            {
                IntPtr a;
                return _exports.TryGetValue(name, out a) ? a : IntPtr.Zero;
            }
        }

        public static ManualImage Load(byte[] so)
        {
            if (s_image != null) return s_image;                     // a second load would rerun constructors and give a second copy of global state
            if (IntPtr.Size != 4) throw new PlatformNotSupportedException("ELF32-ARM image needs a 32-bit process");
            if (so.Length < 52 || so[0] != 0x7F || so[1] != (byte)'E' || so[2] != (byte)'L' || so[3] != (byte)'F')
                throw new InvalidOperationException("not an ELF image");
            if (so[4] != 1) throw new InvalidOperationException("not ELFCLASS32");
            if (so[5] != 1) throw new InvalidOperationException("not little-endian (EI_DATA != 1)");
            if (BitConverter.ToUInt16(so, 16) != 3) throw new InvalidOperationException("not ET_DYN (e_type != 3)");
            if (BitConverter.ToUInt16(so, 18) != 40) throw new InvalidOperationException("not ARM (e_machine != 40)");

            uint phoff = BitConverter.ToUInt32(so, 28);
            int phentsize = BitConverter.ToUInt16(so, 42);
            int phnum = BitConverter.ToUInt16(so, 44);
            if ((long)phoff + (long)phnum * phentsize > so.Length)
                throw new InvalidOperationException("program headers lie outside the file");

            // Program headers. loads entry = {offset, vaddr, filesz, memsz, flags}.
            var loads = new List<uint[]>();
            uint dynOff = 0, dynSize = 0, relroVaddr = 0, relroMemsz = 0;
            bool hasExidx = false;
            for (int i = 0; i < phnum; i++)
            {
                int p = (int)phoff + i * phentsize;
                uint type = BitConverter.ToUInt32(so, p);
                if (type == 1)                                       // PT_LOAD
                {
                    var l = new[] { BitConverter.ToUInt32(so, p + 4), BitConverter.ToUInt32(so, p + 8),
                                    BitConverter.ToUInt32(so, p + 16), BitConverter.ToUInt32(so, p + 20),
                                    BitConverter.ToUInt32(so, p + 24) };
                    if (l[2] > l[3]) throw new InvalidOperationException("PT_LOAD " + i + ": filesz > memsz");
                    if ((long)l[0] + l[2] > so.Length) throw new InvalidOperationException("PT_LOAD " + i + " lies outside the file");
                    loads.Add(l);
                }
                else if (type == 2)                                  // PT_DYNAMIC
                { dynOff = BitConverter.ToUInt32(so, p + 4); dynSize = BitConverter.ToUInt32(so, p + 20); }
                else if (type == 0x6474e552)                         // PT_GNU_RELRO
                { relroVaddr = BitConverter.ToUInt32(so, p + 8); relroMemsz = BitConverter.ToUInt32(so, p + 20); }
                else if (type == 0x70000001) hasExidx = true;        // PT_ARM_EXIDX
            }
            if (loads.Count == 0) throw new InvalidOperationException("no PT_LOAD segments");
            if (dynOff == 0) throw new InvalidOperationException("no PT_DYNAMIC segment");
            if (hasExidx)
                Log.Info("manual loader: image is not registered with the dynamic linker, so dl_iterate_phdr/dladdr do not see it and C++ exceptions/unwinding through it will abort");

            long minV = long.MaxValue, maxV = 0;
            foreach (var l in loads)
            {
                long lo = PageDown(l[1]), hi = PageUp((long)l[1] + l[3]);
                if (lo < minV) minV = lo;
                if (hi > maxV) maxV = hi;
            }
            long span = maxV - minV;

            IntPtr @base = mmap(IntPtr.Zero, (UIntPtr)(ulong)span, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, IntPtr.Zero);
            if (@base == IntPtr.Zero || @base == (IntPtr)(-1))
                throw new InvalidOperationException("mmap failed: errno " + Marshal.GetLastWin32Error());
            long baseAddr = (uint)@base.ToInt32();                   // ToInt64 would sign-extend on 32-bit
            long bias = baseAddr - minV;
            Log.Trace("manual loader: mapped span=0x" + span.ToString("x") + " at base=0x" + baseAddr.ToString("x"));

            uint dtInit = 0, dtInitArray = 0, dtInitArraySz = 0;
            Dictionary<string, IntPtr> exports = null;
            try
            {
                // Copy file-backed bytes of each PT_LOAD; the bss tail stays zero from the anon map.
                foreach (var l in loads)
                    Marshal.Copy(so, (int)l[0], Ptr(bias + l[1]), (int)l[2]);

                // A vaddr -> file offset, for tables that live in a file-backed segment.
                Func<uint, int> fileOff = (v) =>
                {
                    foreach (var l in loads)
                        if (v >= l[1] && v < l[1] + l[2]) return (int)(l[0] + (v - l[1]));
                    throw new InvalidOperationException("vaddr 0x" + v.ToString("x") + " not in a file-backed segment");
                };

                // Dynamic section (Elf32_Dyn is 8 bytes: d_tag i32, d_val u32).
                uint dtStrtab = 0, dtSymtab = 0, dtHash = 0, dtRel = 0, dtRelsz = 0, dtJmprel = 0, dtPltrelsz = 0;
                uint dtVersym = 0, dtVerneed = 0, dtVerneednum = 0;
                var needed = new List<uint>();
                int dcount = (int)(dynSize / 8);
                for (int i = 0; i < dcount; i++)
                {
                    int d = (int)dynOff + i * 8;
                    int tag = BitConverter.ToInt32(so, d);
                    uint val = BitConverter.ToUInt32(so, d + 4);
                    if (tag == 0) break;                                 // DT_NULL
                    switch (tag)
                    {
                        case 1:  needed.Add(val); break;                 // DT_NEEDED (string offset)
                        case 2:  dtPltrelsz = val; break;
                        case 4:  dtHash = val; break;
                        case 5:  dtStrtab = val; break;
                        case 6:  dtSymtab = val; break;
                        case 12: dtInit = val; break;
                        case 17: dtRel = val; break;
                        case 18: dtRelsz = val; break;
                        case 20: if (val != 17) throw new InvalidOperationException("DT_PLTREL is not REL"); break;
                        case 23: dtJmprel = val; break;
                        case 25: dtInitArray = val; break;
                        case 27: dtInitArraySz = val; break;
                        case 0x6ffffff0: dtVersym = val; break;          // DT_VERSYM
                        case 0x6ffffffe: dtVerneed = val; break;         // DT_VERNEED
                        case 0x6fffffff: dtVerneednum = val; break;      // DT_VERNEEDNUM
                    }
                }
                if (dtStrtab == 0 || dtSymtab == 0 || dtHash == 0)
                    throw new InvalidOperationException("missing DT_STRTAB/DT_SYMTAB/DT_HASH");

                int strOff = fileOff(dtStrtab);
                int symOff = fileOff(dtSymtab);
                int symCount = (int)BitConverter.ToUInt32(so, fileOff(dtHash) + 4);   // nchain == symbol count
                Log.Trace("manual loader: " + symCount + " dynamic symbols, " + needed.Count + " needed libs");

                // Export table: every defined dynamic symbol, first definition wins.
                exports = new Dictionary<string, IntPtr>();
                for (int i = 0; i < symCount; i++)
                {
                    int e = symOff + i * 16;                             // Elf32_Sym is 16 bytes
                    ushort shndx = BitConverter.ToUInt16(so, e + 14);
                    if (shndx == 0) continue;                            // SHN_UNDEF: not defined here
                    string name = CStr(so, strOff + (int)BitConverter.ToUInt32(so, e));
                    if (!exports.ContainsKey(name)) exports.Add(name, SymAddr(bias, shndx, BitConverter.ToUInt32(so, e + 4)));
                }

                // Version index -> version name, from DT_VERNEED (Elf32_Verneed / Elf32_Vernaux chains).
                var verNames = new Dictionary<int, string>();
                if (dtVersym != 0 && dtVerneed != 0)
                {
                    int vn = fileOff(dtVerneed);
                    for (int k = 0; k < dtVerneednum; k++)
                    {
                        int cnt = BitConverter.ToUInt16(so, vn + 2);
                        int aux = vn + (int)BitConverter.ToUInt32(so, vn + 8);
                        for (int j = 0; j < cnt; j++)
                        {
                            verNames[BitConverter.ToUInt16(so, aux + 6)] = CStr(so, strOff + (int)BitConverter.ToUInt32(so, aux + 8));
                            aux += (int)BitConverter.ToUInt32(so, aux + 12);
                        }
                        vn += (int)BitConverter.ToUInt32(so, vn + 12);
                    }
                }

                // Load every NEEDED library and keep its handle. Resolution is per-handle
                // (dlsym(handle, name)), so the deps do not need to be in the global scope.
                // RTLD_GLOBAL is avoided on purpose: glibc would also promote already-loaded
                // RTLD_LOCAL libs (the .NET runtime has libcrypto/libssl/curl that way) and
                // pollute the global namespace.
                var depHandles = new List<KeyValuePair<string, IntPtr>>();
                foreach (var nOff in needed)
                {
                    string lib = CStr(so, strOff + (int)nOff);
                    IntPtr h = dlopen(lib, RTLD_NOW);
                    Log.Trace("manual loader: dep " + lib + (h == IntPtr.Zero ? " FAILED: " + Marshal.PtrToStringAnsi(dlerror()) : " ok handle=0x" + ((uint)h.ToInt32()).ToString("x")));
                    if (h != IntPtr.Zero) depHandles.Add(new KeyValuePair<string, IntPtr>(lib, h));
                }

                // Resolve one symbol by index: defined here -> own address; else search the
                // dependency handles directly (versioned first), then RTLD_DEFAULT as a last resort.
                Func<int, IntPtr> resolve = (symIndex) =>
                {
                    if (symIndex == 0) return IntPtr.Zero;              // STN_UNDEF: S = 0
                    int e = symOff + symIndex * 16;
                    ushort shndx = BitConverter.ToUInt16(so, e + 14);
                    if (shndx != 0) return SymAddr(bias, shndx, BitConverter.ToUInt32(so, e + 4));
                    string name = CStr(so, strOff + (int)BitConverter.ToUInt32(so, e));
                    IntPtr a = IntPtr.Zero;
                    string ver;
                    if (dtVersym != 0 && verNames.TryGetValue(BitConverter.ToUInt16(so, fileOff(dtVersym) + 2 * symIndex) & 0x7fff, out ver))
                    {
                        foreach (var d in depHandles)
                        {
                            a = dlvsym(d.Value, name, ver);
                            if (a != IntPtr.Zero) break;
                        }
                        if (a == IntPtr.Zero) a = dlvsym(IntPtr.Zero, name, ver);
                        if (a == IntPtr.Zero) Log.Trace("manual loader: " + name + "@" + ver + " not found versioned, trying unversioned");
                    }
                    if (a == IntPtr.Zero)
                    {
                        foreach (var d in depHandles)
                        {
                            a = dlsym(d.Value, name);
                            if (a != IntPtr.Zero) break;
                        }
                    }
                    if (a == IntPtr.Zero) a = dlsym(IntPtr.Zero, name);
                    if (a == IntPtr.Zero && (so[e + 12] >> 4) != 2)      // st_info>>4 == STB_WEAK(2) may stay 0
                        throw new InvalidOperationException("unresolved symbol: " + name +
                            " (searched " + depHandles.Count + " dependency handles and RTLD_DEFAULT)");
                    return a;
                };

                // glibc trims DT_REL when DT_JMPREL lies inside it, so JMPREL is not applied twice.
                if (dtJmprel != 0 && dtRel != 0 && dtRel + dtRelsz == dtJmprel + dtPltrelsz && dtJmprel >= dtRel)
                    dtRelsz -= dtPltrelsz;

                // Relocations: DT_REL then DT_JMPREL. Both REL (Elf32_Rel is 8 bytes); the addend
                // is the word already in place. Reloc-type semantics match glibc's ARM dynamic
                // linker; symbols defined in the image bind to its own copy (no interposition).
                int relative = 0, globDat = 0, jumpSlot = 0, abs32 = 0;
                Action<uint, uint> applyRel = (tblVaddr, tblSize) =>
                {
                    if (tblVaddr == 0 || tblSize == 0) return;
                    int off0 = fileOff(tblVaddr);
                    int n = (int)(tblSize / 8);
                    for (int i = 0; i < n; i++)
                    {
                        int r = off0 + i * 8;
                        uint rOffset = BitConverter.ToUInt32(so, r);
                        uint rInfo = BitConverter.ToUInt32(so, r + 4);
                        int symIndex = (int)(rInfo >> 8);
                        int type = (int)(rInfo & 0xFF);
                        if (rOffset < minV || (long)rOffset + 4 > maxV)
                            throw new InvalidOperationException("reloc r_offset 0x" + rOffset.ToString("x") + " outside the image");
                        IntPtr P = Ptr(bias + rOffset);
                        int A = Marshal.ReadInt32(P);                    // in-place addend
                        switch (type)
                        {
                            case 23: Marshal.WriteInt32(P, unchecked((int)bias + A)); relative++; break;                      // R_ARM_RELATIVE
                            case 21: Marshal.WriteInt32(P, resolve(symIndex).ToInt32()); globDat++; break;                    // R_ARM_GLOB_DAT
                            case 22: Marshal.WriteInt32(P, resolve(symIndex).ToInt32()); jumpSlot++; break;                   // R_ARM_JUMP_SLOT
                            case 2:  Marshal.WriteInt32(P, unchecked(A + resolve(symIndex).ToInt32())); abs32++; break;       // R_ARM_ABS32
                            default: throw new NotSupportedException("unsupported reloc type " + type);
                        }
                    }
                };
                applyRel(dtRel, dtRelsz);
                applyRel(dtJmprel, dtPltrelsz);
                Log.Trace("manual loader: relocs relative=" + relative + " glob_dat=" + globDat +
                          " jump_slot=" + jumpSlot + " abs32=" + abs32);

                // Segment permissions, after relocations (which wrote into the mapped pages).
                // segs entry = {start, end, prot}, page-aligned, image-relative.
                var segs = new List<long[]>();
                foreach (var l in loads)
                {
                    int prot = (((l[4] & 1) != 0) ? PROT_EXEC : 0) |
                               (((l[4] & 2) != 0) ? PROT_WRITE : 0) |
                               (((l[4] & 4) != 0) ? PROT_READ : 0);
                    segs.Add(new long[] { PageDown(l[1]), PageUp((long)l[1] + l[3]), prot });
                }
                Action<long, long, int> protect = (start, end, prot) =>
                {
                    if (mprotect(Ptr(bias + start), (UIntPtr)(ulong)(end - start), prot) != 0)
                        throw new InvalidOperationException("mprotect failed: errno " + Marshal.GetLastWin32Error());
                };
                foreach (var s in segs) protect(s[0], s[1], (int)s[2]);
                // A page shared by two segments keeps both permissions.
                for (int a = 0; a < segs.Count; a++)
                    for (int b = a + 1; b < segs.Count; b++)
                    {
                        long lo = Math.Max(segs[a][0], segs[b][0]), hi = Math.Min(segs[a][1], segs[b][1]);
                        if (lo < hi) protect(lo, hi, (int)(segs[a][2] | segs[b][2]));
                    }

                // The CPU does not keep the I-cache coherent with the copied code: flush executable segments.
                foreach (var l in loads)
                {
                    if ((l[4] & 1) == 0) continue;
                    long start = bias + l[1];
                    if (syscall(ARM_NR_CACHEFLUSH, Ptr(start), Ptr(start + l[3]), 0) != 0)
                        throw new InvalidOperationException("cacheflush failed: errno " + Marshal.GetLastWin32Error());
                }

                // PT_GNU_RELRO becomes read-only; glibc rounds the end down.
                if (relroMemsz != 0)
                {
                    long rs = PageDown(relroVaddr), re = PageDown((long)relroVaddr + relroMemsz);
                    if (re > rs) protect(rs, re, PROT_READ);
                }

                // Page gaps between PT_LOAD ranges inside the span get no access.
                segs.Sort((x, y) => x[0].CompareTo(y[0]));
                long covered = segs[0][1];
                for (int i = 1; i < segs.Count; i++)
                {
                    if (segs[i][0] > covered) protect(covered, segs[i][0], PROT_NONE);
                    if (segs[i][1] > covered) covered = segs[i][1];
                }
            }
            catch
            {
                munmap(@base, (UIntPtr)(ulong)span);                 // nothing from the image has run yet
                throw;
            }

            // Initializers get (argc, argv, envp) like glibc passes them.
            IntPtr nullArr = Marshal.AllocHGlobal(IntPtr.Size);      // never freed: lives as long as the process
            Marshal.WriteIntPtr(nullArr, IntPtr.Zero);
            IntPtr envp = nullArr;
            IntPtr libc = dlopen("libc.so.6", RTLD_NOW);
            IntPtr environ = libc == IntPtr.Zero ? IntPtr.Zero : dlsym(libc, "environ");
            if (environ != IntPtr.Zero) envp = Marshal.ReadIntPtr(environ);

            // Initializers: DT_INIT then DT_INIT_ARRAY.
            if (dtInit != 0)
                ((InitFn)Marshal.GetDelegateForFunctionPointer(Ptr(bias + dtInit), typeof(InitFn)))(0, nullArr, envp);
            if (dtInitArray != 0)
            {
                int n = (int)(dtInitArraySz / 4);
                for (int i = 0; i < n; i++)
                {
                    int fn = Marshal.ReadInt32(Ptr(bias + dtInitArray + 4 * i));
                    if (fn == 0 || fn == -1) continue;
                    ((InitFn)Marshal.GetDelegateForFunctionPointer((IntPtr)fn, typeof(InitFn)))(0, nullArr, envp);
                }
                Log.Trace("manual loader: ran " + n + " init-array function(s)");
            }

            Log.Info("manual loader: image ready at base=0x" + baseAddr.ToString("x"));
            s_image = new ManualImage(@base, exports);
            return s_image;
        }
    }
}
