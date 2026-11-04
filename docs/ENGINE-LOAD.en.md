# Engine loading — decision tree and the exec-mmap block

*(Polish version: [ENGINE-LOAD.md](ENGINE-LOAD.md).)*

Why `libmlengine.so` loads only in a manually started host process, how the
on-device diagnostics resolved the mechanism, and the full decision tree of
load routes — from the dead ones to the working one (the manual loader,
carried by this repo).

State as of 2026-11-04. Notation: **[O]** observed fact, **[W]** conclusion,
**[?]** unverified. Method: static analysis (Hopper CLI, readelf, host IL) +
on-device diagnostics (`mlDiag`, debug build) + start logs from the UI console.

## 1. Symptom

The same .NET host (`MoonlightHost.dll`), the same `libmlengine.so` binary
(sha256 `cca9a8e5…041da`, 513128 B, ELF32-ARM, ET_DYN):

| Process start context | App type | memfd dlopen | file dlopen | Source |
|---|---|---|---|---|
| manual (`app_launcher -s` over SSH) | service .tpk, later ui-app nodisplay .tpk | **OK** | OK | start log 2026-11-03 |
| started from the Web UI | hybrid (web .wgt) | **FAIL** | **FAIL** | git history `50c9dde`, `6119724` |
| started from the Web UI (`tizen.application.launch`) | ui-app nodisplay (separate .tpk) | **FAIL** | **FAIL** | git history `d898499`, log 2026-11-04 |

The glibc error in every case: `failed to map segment from shared object`
= `mmap(fd, PROT_READ|PROT_EXEC)` returned MAP_FAILED. The host-appended
"errno 0" is a measurement artifact (the managed→native transition clobbers
errno) — the probes measured the real code: **EPERM (errno 1)**.

**[W] Key observation:** the failure pattern does not depend on the app type
(service-app vs ui-app vs hybrid), but on the **process start context** —
who/how launched the process at the moment of the exec-mmap.

## 2. What is NOT the cause (ruled out)

- **The binary**: the ELF is formally valid for glibc-2.24/ld-2.24/armv7 — no
  execstack (PT_GNU_STACK RW), no DT_TEXTREL, no TLS (PT_TLS), congruent
  segments, correct alignment (readelf).
- **Constructors**: `.init_array` = 1 entry (`frame_dummy`, an empty
  trampoline), `.fini_array` = the libc destructor — no application code that
  could fail at load (Hopper decompilation).
- **Dependencies**: all 9 NEEDED libs (esplusplayer, ecore_wl2, ecore,
  ssl/crypto 1.0.0, curl, pthread, dl, libc) dlopen fine **in both
  contexts** [O diag].
- **Engine start**: all the system work (threads, sockets, decoder) happens
  inside `LiStartConnection` **after** the load — the mmap failure is earlier.
- **Seccomp / capabilities / mounts**: Seccomp=0, CapEff=0, `/opt` vdfs rw
  (reads work), not memfd-specific (the file and the data-dir copy fail the
  same way), not squashfs (the data dir is vdfs).
- **A file Smack-label mismatch**: the data-dir copy carries a label
  **identical** to the process label (`User::Pkg::org.moonlight.host`), there
  is no SMACK64MMAP, and Smack denials return EACCES, not EPERM.

## 3. Mechanism — resolved by the diag (mlDiag, web-launched)

The `mlDiag` probe (debug build, run from the UI console) examined every route
in the failing process at once:

| Probe | Result |
|---|---|
| file (`bin/res/lib/libmlengine.so`, vdfs rw) | read=ok, **exec-mmap=EPERM**, dlopen fail |
| realpath (the exact same file) | read=ok, **exec=EPERM**, dlopen fail |
| memfd (an anonymous fd) | read=ok, **exec=EPERM**, dlopen fail |
| data-copy (a copy in the app's own data dir) | read=ok, **exec=EPERM**, dlopen fail |
| **anon-exec** (anonymous RW map → `mprotect` RX → call) | **ok(42)** — works |
| deps (6 system libraries from /usr/lib) | **all dlopen OK** |

Process context: Uid 5001, label `User::Pkg::org.moonlight.host`, cgroup
`6:devices:/lxc.payload/dotnetzone` (the .NET process runs in the "dotnetzone"
LXC container), a launch bundle with `__K_RELOCATE_BELOW=com.samsung.tv.csfs`
(absent in the manual start), a 32-bit JIT-ing .NET runtime.

**[W] Conclusion:** a kernel exec-mmap policy (the UEP family — the platform
executables `amd`/`launchpad-process-pool`/`dotnet-launcher` carry a
`##UEP530x…` trailer): in the web-launched context, exec-mmap (PROT_EXEC on
an mmap backed by an fd) is allowed only for whitelisted/signed objects (the
system libraries from /usr/lib); app files, the data-dir copy and a memfd (an
anonymous inode with no signature) are refused **EPERM**. **Anonymous exec**
(MAP_ANONYMOUS RW + `mprotect(PROT_EXEC)`) stays allowed — proof: the .NET
runtime's JIT runs the whole diag in the same process, and the anon-exec
probe returns ok(42).

## 4. The load-route decision tree

```
Host start (web-launched)
└─ Q0: do the deps (esplusplayer, ecore_wl2, ecore, ssl, crypto, curl) load?
   ├─ NO  → no route helps (the loader itself needs the deps;
   │        dlopen per dep is its first step)
   │        → only a change of the process start context (see §6)
   └─ YES → continue
      └─ Q1: does dlopen(packaged file, PROT_EXEC mmap) work?
         ├─ YES → load with a plain file dlopen (the canonical route)
         └─ NO (EPERM) →
            └─ Q2: does a data-dir copy / another path / memfd work?
               ├─ YES → load from there (not our case)
               └─ NO (EPERM on every fd-backed exec-mmap) →
                  └─ Q3: does anon-exec (mmap anon RW → mprotect RX) work?
                     ├─ NO  → the loader is out; only a context change (§6)
                     └─ YES (ok(42)) → MANUAL LOADER = the working route
```

Empirical answers on this TV: Q0=YES, Q1=NO (EPERM), Q2=NO (EPERM on all four
variants), Q3=YES (ok(42)) — hence the manual loader.

### Dead routes (empirically confirmed)

- **dropping memfd → file**: fails identically (EPERM).
- **a data-dir copy**: EPERM.
- **a path/mount change** (realpath vs the fd path): EPERM.
- **"fixing" the file's permissions**: nothing to fix — reads work, the copy's
  label equals the process label, and the denial is EPERM, not EACCES.

### The working route — the manual loader

`ManualLoader.cs` does exactly what the anon-exec probe does: it maps the
image with `MAP_ANONYMOUS` RW, copies the PT_LOAD segments, applies the
relocations (R_ARM_RELATIVE 306 / GLOB_DAT 62 / JUMP_SLOT 661 / ABS32 4 — no
other types in this binary), and only then `mprotect`s the segments to their
final permissions. Mechanically identical to what the .NET JIT does in this
process.

The key fix over the loader's first version (branch `claude/manual-elf-loader`):
**import resolution per dependency handle** (`dlsym(dlopen(lib), name)`), not
through `dlsym(RTLD_DEFAULT)`. The .NET runtime loads some deps
(libcrypto/libssl/curl) in its own **RTLD_LOCAL** scope — dlopen then returns
the existing handle without promoting it to the global scope, and the symbols
stay invisible to RTLD_DEFAULT. Per-handle resolution works in both cases.

Proof of work (web-launched, debug build 2026-11-04): `manual loader: image
ready at base=0xffffffffaaa40000`, all `ml_engine_*` symbols resolve,
`ml_engine_http_init -> 0`.

## 5. Manual loader limitations

- **ELF32-ARM only** and only the 4 relocation types — another binary needs
  an extension (the loader throws `NotSupportedException` on an unknown type).
- **Requires DT_HASH** (SysV) to count the symbols — a GNU_HASH-only binary is
  out (ours has both; the count comes from the DT_HASH nchain).
- **No TLS, no TEXTREL** in the binary — invariants kept by the build
  (build-engine-so.sh); re-verify when the linker flags change.
- **No host-runnable test** — verification is on-device only (`sdb dlog
  MLHOST`); the anon-exec probe in mlDiag is the cheap predictor.
- The init array runs after the relocations (glibc order — RELATIVE entries
  are read with the bias applied).

## 6. Alternative routes (the platform decision tree)

If you ever prefer a "platform-compliant" route over the loader, the decision
hinges on the process start context:

- **rpc-port auto-launch** — swap the UI↔host transport from MessagePort to
  `rpc-port`. Confirmed binary-wise in `libamd.so`
  (`__check_execute_permission`): `launch-type == "rpc-port"` is an exception
  from the cross-package svcapp start block; the host process would start in
  a context as privileged as the manual start. **[?]** Whether the rpc-port
  context grants exec-mmap — to be verified empirically.
- **Manual start (the status quo before the loader)** — works, needs SSH.
- **Hybrid (UI + host in one .wgt)** — dead: both routes fail, and the web-app
  content sits on a `noexec` squashfs.
- **On-boot** — dead: the installer drops the on-boot/auto-restart flags; zero
  MLHOST lines after a reboot.

## 7. What not to do

- Do not try to bypass Smack/UEP **on the system side**: changing labels,
  swapping xattrs, modifying anything outside our own app package.
- Do not merge the loader into the clean upstream (`moonlight-tizen50-core`) —
  it is a device-policy workaround, not engine/UI development;
  `pkg/dotnet-host/ManualLoader.cs` is maintained directly in this repo, and
  the `Program.cs`/`Diag.cs` edits ride in the overlay (patch 0002).

## 8. Baselines still to collect (open items)

- [ ] mlDiag after a manual start (`app_launcher -s`) — will pin down what
      exactly differs in the working context (label? cgroup/dotnetzone?
      `__K_RELOCATE_BELOW`?) from the web-launched one. Does not block the
      loader.
- [ ] An empirical rpc-port test: does a started-by-rpc process get the
      exec-mmap right (§6)?

## 9. Reproducing the diagnosis

UI console (Apps2Samsung) → `mlDiag()` prints the report (debug build; diag
commits: `05a6c53`, the anon-exec probe: `12047b8`). On the TV, read-only:

```bash
PID=$(pgrep -f MoonlightHost.dll | head -1)
cat /proc/$PID/attr/current                 # the process SMACK label
grep -E "^(Uid|Gid|CapEff|Seccomp)" /proc/$PID/status
cat /proc/$PID/cgroup                       # lxc.payload/dotnetzone?
# launch bundle (start context): look for __K_RELOCATE_BELOW in diag#1 launch
# the engine file's label:
attr -g SMACK64 /opt/usr/apps/org.moonlight.host/bin/res/lib/libmlengine.so
```

Loader success in the log: `manual loader: image ready at base=…` → all the
`dlsym ml_engine_*` lines → `ml_engine_http_init -> 0`.
