# Ładowanie silnika — drzewo decyzyjne i mechanizm blokady exec-mmap

*(Wersja angielska: [ENGINE-LOAD.en.md](ENGINE-LOAD.en.md).)*

Dokument opisuje, **dlaczego `libmlengine.so` ładuje się tylko w ręcznie
startowanym procesie hosta**, jak diag rozstrzygnął mechanizm, oraz pełne
drzewo decyzyjne tras ładowania — od martwych po działającą (manual loader,
`patches/0002-manual-engine-loader.patch`).

Stan na 2026-11-04. Oznaczenia: **[O]** obserwacja z artefaktu/logu, **[W]** wniosek,
**[?]** niezweryfikowane. Metoda: statyczna (Hopper CLI, readelf, IL hosta) +
diagnostyka na TV (`mlDiag`, debug build) + logi startów z konsoli UI.

## 1. Objaw

Ten sam host .NET (`MoonlightHost.dll`), ta sama binarna `libmlengine.so`
(sha256 `cca9a8e5…041da`, 513128 B, ELF32-ARM, ET_DYN):

| Kontekst startu procesu | Typ aplikacji | memfd dlopen | plik dlopen | Źródło |
|---|---|---|---|---|
| ręczny (`app_launcher -s` przez SSH) | service .tpk, potem ui-app nodisplay .tpk | **OK** | OK | log startu 2026-11-03 |
| start z Web UI | hybryda (web .wgt) | **FAIL** | **FAIL** | historia git `50c9dde`, `6119724` |
| start z Web UI (`tizen.application.launch`) | ui-app nodisplay (osobny .tpk) | **FAIL** | **FAIL** | historia git `d898499`, log 2026-11-04 |

Błąd glibc w każdym przypadku: `failed to map segment from shared object`
= `mmap(fd, PROT_READ|PROT_EXEC)` zwróciło MAP_FAILED. Doklejane przez hosta
„errno 0" to artefakt pomiaru (managed→native transition kasuje errno) —
realny kod błędu z sond wyniósł **EPERM (errno 1)**.

**[W] Kluczowa obserwacja:** wzorzec porażki nie zależy od typu aplikacji
(service-app vs ui-app vs hybryda), lecz od **kontekstu startu procesu** —
kto/ jak uruchomił proces w momencie exec-mmap.

## 2. Co NIE jest przyczyną (wykluczone)

- **Binarna**: ELF formalnie poprawny dla glibc-2.24/ld-2.24/armv7 — brak
  execstack (PT_GNU_STACK RW), brak DT_TEXTREL, brak TLS (PT_TLS), kongruentne
  segmenty, poprawne aligny (readelf, §ANALIZA-1).
- **Konstruktory**: `.init_array` = 1 wpis (`frame_dummy`, pusty trampolin),
  `.fini_array` = destruktor libc — zero kodu aplikacyjnego, który mógłby
  zawieść przy load (dekom­pilacja Hopper).
- **Zależności**: wszystkie 9 NEEDED (esplusplayer, ecore_wl2, ecore, ssl/crypto
  1.0.0, curl, pthread, dl, libc) dlopen'uje się **w obu kontekstach** [O diag].
- **Start silnika**: cała praca systemowa (wątki, gniazda, dekoder) dzieje się
  w `LiStartConnection` **po** załadowaniu — porażka mmap jest wcześniejsza.
- **Seccomp / capabilities / mount**: Seccomp=0, CapEff=0, `/opt` vdfs rw
  (odczyt działa), nie memfd-specific (plik i kopia do data dir failują tak samo),
  nie squashfs (data dir to vdfs).
- **Smak-label mismatch pliku**: kopia w data dir ma etykietę **identyczną**
  z etykietą procesu (`User::Pkg::org.moonlight.host`), brak SMACK64MMAP, a
  odmowy Smack zwracają EACCES, nie EPERM.

## 3. Mechanizm — ustalony przez diag (mlDiag, web-launched)

Sonda `mlDiag` (debug build, uruchomiona z konsoli UI) zbadała w padającym
procesie wszystkie trasy jednocześnie:

| Sonda | Wynik |
|---|---|
| file (`bin/res/lib/libmlengine.so`, vdfs rw) | read=ok, **exec-mmap=EPERM**, dlopen fail |
| realpath (dokładnie ten sam plik) | read=ok, **exec=EPERM**, dlopen fail |
| memfd (anonimowy fd) | read=ok, **exec=EPERM**, dlopen fail |
| data-copy (kopia we własnym data dir) | read=ok, **exec=EPERM**, dlopen fail |
| **anon-exec** (mapa anonimowa RW → `mprotect` RX → wywołanie) | **ok(42)** — działa |
| deps (6 bibliotek systemowych z /usr/lib) | **wszystkie dlopen OK** |

Kontekst procesu: Uid 5001, label `User::Pkg::org.moonlight.host`,
cgroup `6:devices:/lxc.payload/dotnetzone` (proces .NET działa w kontenerze
LXC „dotnetzone"), launch bundle z `__K_RELOCATE_BELOW=com.samsung.tv.csfs`
(przy starcie ręcznym tego klucza nie ma), runtime .NET 32-bit JIT-ujący.

**[W] Konkluzja:** kernelowa polityka exec-mmap (rodzina UEP — platformowe
executables `amd`/`launchpad-process-pool`/`dotnet-launcher` noszą trailer
`##UEP530x…`): w web-launched kontekście exec-mmap (PROT_EXEC na mmap z fd)
jest dozwolony tylko dla obiektów whitelisted/podpisanych (biblioteki
systemowe z /usr/lib); pliki aplikacyjne, kopia w data dir i memfd (anonimowy
inode bez podpisu) są odrzucane **EPERM**. **Anonimowy exec** (MAP_ANONYMOUS
RW + `mprotect(PROT_EXEC)`) pozostaje dozwolony — dowód: JIT runtime'u .NET
wykonuje cały diag w tym samym procesie, oraz sonda anon-exec: ok(42).

## 4. Drzewo decyzyjne tras ładowania

```
Start hosta (web-launched)
└─ Q0: deps (esplusplayer, ecore_wl2, ecore, ssl, crypto, curl) ładują się?
   ├─ NIE → żadna trasa nie pomoże (loader sam potrzebuje deps,
   │        dlopen per-dep to jego pierwsza czynność)
   │        → tylko zmiana kontekstu startu procesu (patrz §6)
   └─ TAK → kontynuuj
      └─ Q1: dlopen(plik w pakiecie, PROT_EXEC mmap) działa?
         ├─ TAK → ładować zwykłym dlopen z pliku (trasa kanoniczna)
         └─ NIE (EPERM) →
            └─ Q2: kopia do data dir / inna ścieżka / memfd działa?
               ├─ TAK → ładować z tamtąd (nie nasz przypadek)
               └─ NIE (EPERM na każdym fd-backed exec-mmap) →
                  └─ Q3: anon-exec (mmap anon RW → mprotect RX) działa?
                     ├─ NIE → loader odpada; tylko zmiana kontekstu (§6)
                     └─ TAK (ok(42)) → MANUAL LOADER = działająca trasa
```

Weryfikacja empiryczna na tym TV: Q0=TAK, Q1=NIE (EPERM), Q2=NIE (EPERM na
wszystkich czterech wariantach), Q3=TAK (ok(42)) — stąd manual loader.

### Martwe drogi (potwierdzone empirycznie)

- **drop memfd → plik**: fail identycznie (EPERM).
- **kopia do data dir**: EPERM.
- **zmiana ścieżki/mounta** (realpath vs fd-path): EPERM.
- **„naprawa" uprawnień pliku**: nie ma czego naprawiać — odczyt działa,
  etykieta procesu = etykieta kopii, a odmowa to EPERM nie EACCES.

### Działająca droga — manual loader

`ManualLoader.cs` (patch 0002) wykonuje dokładnie to, co sonda anon-exec:
mapuje obraz przez `MAP_ANONYMOUS` RW, kopiuje PT_LOAD, aplikuje relokacje
(R_ARM_RELATIVE 306 / GLOB_DAT 62 / JUMP_SLOT 661 / ABS32 4 — zero innych
typów w tej binarnej), dopiero potem `mprotect` nadaje segmentom docelowe
uprawnienia. Mechanicznie identyczne z tym, co robi JIT .NET w tym procesie.

Kluczowa poprawka względem pierwszej wersji loadera (gałąź
`claude/manual-elf-loader`): **resolve importów per uchwyt zależności**
(`dlsym(dlopen(lib), name)`), nie przez `dlsym(RTLD_DEFAULT)`. Runtime .NET
ładuje część deps (libcrypto/libssl/curl) w swoim scope **RTLD_LOCAL** —
dlopen zwróci istniejący uchwyt bez promocji do scope globalnego i symbole
zostaną niewidoczne dla RTLD_DEFAULT. Resolve per-handle działa w obu przypadkach.

Dowód działania (web-launched, debug build 2026-11-04): `manual loader: image
ready at base=0xffffffffaaa40000`, wszystkie `ml_engine_*` rozwiązywane,
`ml_engine_http_init -> 0`.

## 5. Ograniczenia manual loadera

- **Tylko ELF32-ARM** i tylko 4 typy relokacji — inne binarne wymaga
  rozszerzenia (loader rzuca `NotSupportedException` na nieznany typ).
- **Wymaga DT_HASH** (SysV) do policzenia symboli — binarne z samym GNU_HASH
  odpadają (nasza ma oba; licznik brany z DT_HASH nchain).
- **Brak TLS, brak TEXTREL** w binarnej — invariants utrzymywane przez build
  (build-engine-so.sh); przy zmianie flag linkera zweryfikować ponownie.
- **Brak testu host-runnable** — weryfikacja tylko na urządzeniu (`sdb dlog
  MLHOST`); sonda anon-exec w mlDiag jest tanią predykcją działania.
- Init-array wykonywany po relokacjach (kolejność zgodna z glibc — wpisy
  RELATIVE są czytane z biasem).

## 6. Ścieżki alternatywne (drzewo decyzji platformowych)

Jeśli kiedyś zamiast loadera wolisz trasę „zgodną z platformą", decyzja
zależy od odpowiedzi na pytanie o kontekst startu:

- **rpc-port auto-launch** — zamiana transportu UI↔host z MessagePort na
  `rpc-port`. Potwierdzone binarnie w `libamd.so` (`__check_execute_permission`):
  `launch-type == "rpc-port"` to wyjątek od blokady startu svcapp
  międzypakietowo; proces hosta startowałby w kontekście uprzywilejowanym
  jak przy starcie ręcznym. **[?]** Czy kontekst rpc-port daje prawo
  exec-mmap — do potwierdzenia empirycznie.
- **Start ręczny (status quo przed loaderem)** — działa, wymaga SSH.
- **Hybryda (UI+host w jednym .wgt)** — martwe: obie trasy failują, a content
  web-appów leży na squashfs `noexec`.
- **On-boot** — martwe: installer gubi on-boot/auto-restart, po reboocie zero
  linii MLHOST.

## 7. Czego nie robić

- Nie próbować obchodzić Smack/UEP **po stronie systemu**: zmiana etykiet,
  podmiana xattr, modyfikacje poza własnym pakietem aplikacji.
- Nie scalać loadera do czystego upstreamu (`moonlight-tizen50-core`) —
  to workaround polityki urządzenia, nie rozwój engine/UI; plik
  `pkg/dotnet-host/ManualLoader.cs` jest utrzymywany bezpośrednio tutaj,
  a zmiany w `Program.cs`/`Diag.cs` jeździ w overlayu (patch 0002).

## 8. Baseline do zebrania (pozycje otwarte)

- [x] mlDiag po ręcznym starcie (`app_launcher -s`) — **zebrane 2026-11-04
      (root SSH)**. Wynik zaskakujący: kontekst ręczny jest **identyczny**
      z web-launched — ta sama etykieta procesu
      (`User::Pkg::org.moonlight.host`), ten sam uid 5001/gid 100,
      `CapEff=0`, `Seccomp=0`, ta sama cgroupa `6:devices:/lxc.payload/dotnetzone`
      (z `devices.list = a *:* rwm`), wspólne pid/ipc/net ns z systemem
      (odrębny tylko mnt ns), a `__K_RELOCATE_BELOW=com.samsung.tv.csfs`
      występuje w **obu** bundlach (to nie jest wyznacznik drogi startu).
      A mimo to w procesie ręcznym memfd-dlopen **przechodzi**
      (`/proc/<pid>/maps` bez odmowy UEP w dmesg), a w web-launched
      ten sam plik dostaje EPERM. Wniosek: różnica nie leży w żadnym
      atrybucie widocznym w `/proc` — rozstrzyga stan wewnątrz taska
      (najbardziej prawdopodobny mechanizm: **flaga `PF_UEP_*` /
      exemption wydawana przez launchpad przy `app_launcher -s`,
      dziedziczona przez procesy potomne** — wymaga potwierdzenia
      w kodzie launchpada/libaul). Praktycznie: nie da się odróżnić
      „dobrego" kontekstu żadnym statycznym testem — dopóki mechanizm
      nie zostanie zlokalizowany, loader pozostaje jedyną działającą
      trasą dla web-launched.
- [ ] Empiryczny test rpc-port: czy proces started-by-rpc dostaje prawo
      exec-mmap (§6). Po wyniku baseline wyżej: rpc-port nadal wart testu —
      start odbywa się przez amd/launchpad (nie przez child web-app), więc
      szansa, że dostanie exemption jak start ręczny, jest realna.

## 9. Odtworzenie diagnozy

Konsola UI (Apps2Samsung) → `mlDiag()` wypisuje raport (debug build; commity
diag: `05a6c53`, sonda anon-exec: `12047b8`). Na TV read-only:

```bash
PID=$(pgrep -f MoonlightHost.dll | head -1)
cat /proc/$PID/attr/current                 # etykieta SMACK procesu
grep -E "^(Uid|Gid|CapEff|Seccomp)" /proc/$PID/status
cat /proc/$PID/cgroup                       # lxc.payload/dotnetzone?
# launch bundle (kontekst startu): szukaj __K_RELOCATE_BELOW w diag#1 launch
# etykieta pliku silnika:
attr -g SMACK64 /opt/usr/apps/org.moonlight.host/bin/res/lib/libmlengine.so
```

Sukces loadera w logu: `manual loader: image ready at base=…` → wszystkie
`dlsym ml_engine_*` → `ml_engine_http_init -> 0`.
