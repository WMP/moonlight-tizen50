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
        private const int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
        private const int MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20, Page = 4096;
        private const int RTLD_NOW = 0x0002;
        private const int RTLD_GLOBAL = 0x0100;

        [DllImport("libc", SetLastError = true)]  private static extern IntPtr mmap(IntPtr addr, UIntPtr length, int prot, int flags, int fd, IntPtr offset);
        [DllImport("libc", SetLastError = true)]  private static extern int    mprotect(IntPtr addr, UIntPtr length, int prot);
        [DllImport("libdl.so.2", SetLastError = true)]  private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlerror();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void InitFn();

        private static ManualImage s_image;   // kept for process life; its mmap is never unmapped

        // The manually mapped image, when the last load used this loader (else null).
        public static ManualImage Image { get { return s_image; } }

        private static long PageDown(long v) => v & ~((long)Page - 1);
        private static long PageUp(long v)   => (v + Page - 1) & ~((long)Page - 1);

        private static string CStr(byte[] b, int off)
        {
            int n = off;
            while (n < b.Length && b[n] != 0) n++;
            return Encoding.ASCII.GetString(b, off, n - off);
        }

        // A manually mapped image: resolves its exported symbols by name from its own symtab.
        public sealed class ManualImage
        {
            public readonly IntPtr Base;
            private readonly long _bias;
            private readonly byte[] _so;
            private readonly int _symOff, _symCount, _strOff;

            public ManualImage(IntPtr @base, long bias, byte[] so, int symOff, int symCount, int strOff)
            { Base = @base; _bias = bias; _so = so; _symOff = symOff; _symCount = symCount; _strOff = strOff; }

            public IntPtr Resolve(string name)
            {
                for (int i = 0; i < _symCount; i++)
                {
                    int e = _symOff + i * 16;                        // Elf32_Sym is 16 bytes
                    ushort shndx = BitConverter.ToUInt16(_so, e + 14);
                    if (shndx == 0) continue;                        // SHN_UNDEF: not defined here
                    uint stName = BitConverter.ToUInt32(_so, e);
                    if (CStr(_so, _strOff + (int)stName) != name) continue;
                    return (IntPtr)(_bias + BitConverter.ToUInt32(_so, e + 4));
                }
                return IntPtr.Zero;
            }
        }

        public static ManualImage Load(byte[] so)
        {
            if (so.Length < 52 || so[0] != 0x7F || so[1] != (byte)'E' || so[2] != (byte)'L' || so[3] != (byte)'F')
                throw new InvalidOperationException("not an ELF image");
            if (so[4] != 1) throw new InvalidOperationException("not ELFCLASS32");
            if (BitConverter.ToUInt16(so, 18) != 40) throw new InvalidOperationException("not ARM (e_machine != 40)");

            uint phoff = BitConverter.ToUInt32(so, 28);
            int phentsize = BitConverter.ToUInt16(so, 42);
            int phnum = BitConverter.ToUInt16(so, 44);

            // Program headers. loads entry = {offset, vaddr, filesz, memsz, flags}.
            var loads = new List<uint[]>();
            uint dynOff = 0, dynSize = 0;
            for (int i = 0; i < phnum; i++)
            {
                int p = (int)phoff + i * phentsize;
                uint type = BitConverter.ToUInt32(so, p);
                if (type == 1)                                       // PT_LOAD
                    loads.Add(new[] { BitConverter.ToUInt32(so, p + 4), BitConverter.ToUInt32(so, p + 8),
                                      BitConverter.ToUInt32(so, p + 16), BitConverter.ToUInt32(so, p + 20),
                                      BitConverter.ToUInt32(so, p + 24) });
                else if (type == 2)                                  // PT_DYNAMIC
                { dynOff = BitConverter.ToUInt32(so, p + 4); dynSize = BitConverter.ToUInt32(so, p + 20); }
            }
            if (loads.Count == 0) throw new InvalidOperationException("no PT_LOAD segments");
            if (dynOff == 0) throw new InvalidOperationException("no PT_DYNAMIC segment");

            long minV = long.MaxValue, maxV = 0;
            foreach (var l in loads)
            {
                long lo = PageDown(l[1]), hi = PageUp(l[1] + l[3]);
                if (lo < minV) minV = lo;
                if (hi > maxV) maxV = hi;
            }
            long span = maxV - minV;

            IntPtr @base = mmap(IntPtr.Zero, (UIntPtr)(ulong)span, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, IntPtr.Zero);
            if (@base == IntPtr.Zero || @base == (IntPtr)(-1))
                throw new InvalidOperationException("mmap failed: errno " + Marshal.GetLastWin32Error());
            long bias = @base.ToInt64() - minV;
            Log.Trace("manual loader: mapped span=0x" + span.ToString("x") + " at base=0x" + @base.ToInt64().ToString("x"));

            // Copy file-backed bytes of each PT_LOAD; the bss tail stays zero from the anon map.
            foreach (var l in loads)
                Marshal.Copy(so, (int)l[0], (IntPtr)(bias + l[1]), (int)l[2]);

            // A vaddr -> file offset, for tables that live in a file-backed segment.
            Func<uint, int> fileOff = (v) =>
            {
                foreach (var l in loads)
                    if (v >= l[1] && v < l[1] + l[2]) return (int)(l[0] + (v - l[1]));
                throw new InvalidOperationException("vaddr 0x" + v.ToString("x") + " not in a file-backed segment");
            };

            // Dynamic section (Elf32_Dyn is 8 bytes: d_tag i32, d_val u32).
            uint dtStrtab = 0, dtSymtab = 0, dtHash = 0, dtRel = 0, dtRelsz = 0, dtJmprel = 0, dtPltrelsz = 0;
            uint dtInit = 0, dtInitArray = 0, dtInitArraySz = 0;
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
                }
            }
            if (dtStrtab == 0 || dtSymtab == 0 || dtHash == 0)
                throw new InvalidOperationException("missing DT_STRTAB/DT_SYMTAB/DT_HASH");

            int strOff = fileOff(dtStrtab);
            int symOff = fileOff(dtSymtab);
            int symCount = (int)BitConverter.ToUInt32(so, fileOff(dtHash) + 4);   // nchain == symbol count
            Log.Trace("manual loader: " + symCount + " dynamic symbols, " + needed.Count + " needed libs");

            // Load every NEEDED library and keep its handle. The .NET runtime may already
            // have some of them (libcrypto/libssl/curl) in its own RTLD_LOCAL scope: then
            // dlopen returns the existing handle WITHOUT promoting it to the global scope,
            // and dlsym(RTLD_DEFAULT) will not see its symbols. Resolving per-handle
            // (dlsym(handle, name)) works in both cases.
            var depHandles = new List<KeyValuePair<string, IntPtr>>();
            foreach (var nOff in needed)
            {
                string lib = CStr(so, strOff + (int)nOff);
                IntPtr h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
                Log.Trace("manual loader: dep " + lib + (h == IntPtr.Zero ? " FAILED: " + dlerror() : " ok handle=0x" + h.ToInt64().ToString("x")));
                if (h != IntPtr.Zero) depHandles.Add(new KeyValuePair<string, IntPtr>(lib, h));
            }

            // Resolve one symbol by index: defined here -> own address; else search the
            // dependency handles directly, then RTLD_DEFAULT as a last resort.
            Func<int, IntPtr> resolve = (symIndex) =>
            {
                int e = symOff + symIndex * 16;
                ushort shndx = BitConverter.ToUInt16(so, e + 14);
                if (shndx != 0) return (IntPtr)(bias + BitConverter.ToUInt32(so, e + 4));
                string name = CStr(so, strOff + (int)BitConverter.ToUInt32(so, e));
                IntPtr a = IntPtr.Zero;
                foreach (var d in depHandles)
                {
                    a = dlsym(d.Value, name);
                    if (a != IntPtr.Zero) break;
                }
                if (a == IntPtr.Zero) a = dlsym(IntPtr.Zero, name);
                if (a == IntPtr.Zero && (so[e + 12] >> 4) != 2)      // st_info>>4 == STB_WEAK(2) may stay 0
                    throw new InvalidOperationException("unresolved symbol: " + name +
                        " (searched " + depHandles.Count + " dependency handles and RTLD_DEFAULT)");
                return a;
            };

            // Relocations: DT_REL then DT_JMPREL. Both REL (Elf32_Rel is 8 bytes); the addend
            // is the word already in place. Semantics match glibc's ARM dynamic linker.
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
                    IntPtr P = (IntPtr)(bias + rOffset);
                    int A = Marshal.ReadInt32(P);                    // in-place addend
                    switch (type)
                    {
                        case 23: Marshal.WriteInt32(P, (int)(bias + (uint)A)); relative++; break;              // R_ARM_RELATIVE
                        case 21: Marshal.WriteInt32(P, (int)resolve(symIndex)); globDat++; break;              // R_ARM_GLOB_DAT
                        case 22: Marshal.WriteInt32(P, (int)resolve(symIndex)); jumpSlot++; break;             // R_ARM_JUMP_SLOT
                        case 2:  Marshal.WriteInt32(P, (int)((uint)A + (uint)resolve(symIndex))); abs32++; break; // R_ARM_ABS32
                        default: throw new NotSupportedException("unsupported reloc type " + type);
                    }
                }
            };
            applyRel(dtRel, dtRelsz);
            applyRel(dtJmprel, dtPltrelsz);
            Log.Trace("manual loader: relocs relative=" + relative + " glob_dat=" + globDat +
                      " jump_slot=" + jumpSlot + " abs32=" + abs32);

            // Segment permissions, after relocations (which wrote into the mapped pages).
            foreach (var l in loads)
            {
                int prot = (((l[4] & 1) != 0) ? PROT_EXEC : 0) |
                           (((l[4] & 2) != 0) ? PROT_WRITE : 0) |
                           (((l[4] & 4) != 0) ? PROT_READ : 0);
                long start = bias + PageDown(l[1]);
                long len = PageUp(l[1] + l[3]) - PageDown(l[1]);
                if (mprotect((IntPtr)start, (UIntPtr)(ulong)len, prot) != 0)
                    throw new InvalidOperationException("mprotect failed: errno " + Marshal.GetLastWin32Error());
            }

            // Initializers: DT_INIT then DT_INIT_ARRAY.
            if (dtInit != 0)
                ((InitFn)Marshal.GetDelegateForFunctionPointer((IntPtr)(bias + dtInit), typeof(InitFn)))();
            if (dtInitArray != 0)
            {
                int n = (int)(dtInitArraySz / 4);
                for (int i = 0; i < n; i++)
                {
                    int fn = Marshal.ReadInt32((IntPtr)(bias + dtInitArray + 4 * i));
                    if (fn == 0 || fn == -1) continue;
                    ((InitFn)Marshal.GetDelegateForFunctionPointer((IntPtr)fn, typeof(InitFn)))();
                }
                Log.Trace("manual loader: ran " + n + " init-array function(s)");
            }

            Log.Info("manual loader: image ready at base=0x" + @base.ToInt64().ToString("x"));
            s_image = new ManualImage(@base, bias, so, symOff, symCount, strOff);
            return s_image;
        }
    }
}
