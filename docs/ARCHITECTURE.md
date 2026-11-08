# Pakiet Moonlight dla Tizen 5.0 (UI z moonlight-tizen + natywny silnik)

Cel: w pasku aplikacji TV pojawia się **UI moonlight-tizena** (web), a strumieniowanie
robi **nasz natywny silnik** przez `libesplusplayer.so` (bo EMSS jest dopiero od 5.5).
Bez NaCl, bez WASM.

> **Aktualny układ (od v0.0.x): dwa pakiety.** Web UI jedzie jako `.wgt`, a host
> natywnego silnika jako osobny `.tpk` (aplikacja .NET ładująca `libmlengine.so`).
> UI uruchamia hosta w momencie startu streamu. „Pakiet hybrydowy” opisany
> poniżej to wcześniejszy projekt — jeden pakiet z natywną usługą `mlservice.c` —
> zachowany tu jako historia projektowa, która doprowadziła do obecnego
> kontraktu mostka.

## Pakiet hybrydowy Tizen (zastąpiony; patrz notka wyżej)

Jeden pakiet zawiera dwie aplikacje (Tizen "hybrid app"):
- **UI = wgt** (web): `wasm/platform/*.js` + `index.html` + `res/` z moonlight-tizena,
  bez zmian w logice. Zmieniamy TYLKO warstwę transportu `sendMessage`/`handleMessage`.
- **Usługa = tpk** (natywna, service-application): nasz silnik + serwer mostka.

## Kontrakt UI ↔ usługa (11 funkcji, 1:1 z eksportami embind moonlight-tizena)

JS woła je przez `sendMessage(name, args)`; wyniki wracają przez `handleMessage`.
W oryginale szły do modułu WASM; u nas idą przez mostek do natywnej usługi.

| Funkcja | Natywny odpowiednik (już mamy) |
|---|---|
| `makeCert` | mkcert / cert_init.c |
| `httpInit(cert,key,uniqueId)` | ml_cert_init (cert_init.c) |
| `openUrl(url,ppk,bin)` | http_request (libgamestream http.c) |
| `pair(major,addr,port,pin)` | gs_pair (libgamestream pairing.c) |
| `startStream([host,port,w,h,fps,bitrate,rikey,rikeyid,appVer,gfeVer,sessionUrl0,codecMode,...])` | LiStartConnection + video_esplus + audio_opus (esplus_player) |
| `stopStream` | LiStopConnection + esplus_close |
| `cancelRequest` | http_cancel_request |
| `sendKeyboardEvent(key,action,mods)` | LiSendKeyboardEvent |
| `stun` | LiFindExternalAddressIP4 |
| `wakeOnLan` | magic packet |
| `toggleStats` | lokalne |

Uwaga: w hybrydzie JS robi parowanie/launch wołając `openUrl`/`pair`, a potem
`startStream` z gotowym `sessionUrl0` i `rikey` (jak w oryginale, patrz index.js
`sendMessage('startRequest', [...])`). Czyli nasza orkiestracja z gs_client.c
(serverinfo→pair→launch) jest tu **rozbita na wywołania `openUrl`/`pair`**, które
steruje JS — albo zostawiamy gs_client jako całość i dodajemy jedną funkcję
`connect(host,appid,pin)`. Decyzja na etapie implementacji mostka.

## Mostek: Tizen message-port (rekomendacja)

- JS: `tizen.messageport` (local message port), natyw: C `message_port` API.
- Bez sieci/portów, natywne IPC wgt↔tpk w jednym pakiecie.
- Duże dane (XML serverinfo, applist, box art) jako wiadomości bajtowe.
- Zapas: WebSocket na localhost (usługa = serwer WS, UI = klient).

## Wideo + wejście w trakcie streamingu

- Usługa renderuje przez esplusplayer na plan sprzętowy (overlay). Web UI nad nim
  pokazuje przy streamingu przezroczystą stronę → widać overlay.
- Wejście: JS przekazuje klawisze przez `sendKeyboardEvent` (jak oryginał);
  evdev (input_evdev.c) to alternatywa/uzupełnienie (pad).
- Wyjście ze streamingu przez powłokę (Wstecz/Domek) → powłoka przywraca źródło
  (rozwiązuje "czarny ekran" z testów CLI).

## Co dziś buduje Docker (pkg/Dockerfile + build-*.sh)

1. Silnik: `build-deps-lib.sh` (moonlight-common-c, libgamestream, libopus,
   depsy crypto/http) + `mlclient/build-engine-so.sh` → `libmlengine.so`
   (ARM, cross GCC 6.2, linkuje esplusplayer z `tv-libs/`).
2. Host: `pkg/build-tpk.sh` → `org.moonlight.host-*.tpk` (aplikacja .NET
   ładująca silnik; overlay tego repo dokłada ręczny loader ELF).
3. UI: `pkg/build-package.sh` → `Moonlight-*.wgt` (front-end moonlight-tizena
   + `mlbridge.js`/`mlaudio.js`, podpis dev profilem w kontenerze).

Wejścia montowane (nie w repo): `tv-libs/` (biblioteki .so z TV); CI pobiera je
z prywatnego magazynu przez sekrety `TV_LIBS_URL`/`TV_LIBS_TOKEN`.

## Status

Patrz `ARCHITECTURE.md` po polsku i `../BACKPORT.en.md` dla stanu krok po kroku.
Rdzeń silnika (cert/http/pair/wideo/audio/wejście) DZIAŁA natywnie i jest
przetestowany end-to-end z hostem.
