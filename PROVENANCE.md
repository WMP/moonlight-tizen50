# Proweniencja kodu i wyniki reverse engineering

Natywny port Moonlight na **Samsung TV Tizen 5.0** (armv7l softfp, glibc 2.24,
GCC 6.2), bez NaCl i bez WASM. Powstał przez połączenie wielokrotnie używalnych
części **moonlight-tizen** (brightcraft) z nowym natywnym silnikiem opartym na
wewnętrznej bibliotece TV `libesplusplayer.so`.

Powód osobnego portu: moonlight-tizen jest WASM/Emscripten i używa Samsung EMSS,
który istnieje dopiero od Tizen 5.5. Na 5.0 nie ma EMSS ani publicznego API do
niskolatencyjnego podawania pakietów elementarnych — jedyną drogą jest natywny
esplusplayer, którego ABI trzeba było odczytać z kodu maszynowego.

Legenda: **[UP]** wzięte z moonlight-tizen bez zmian · **[MOD]** podmienione/
zastąpione · **[NEW]** nowe (brak odpowiednika/API) · **[RE]** wynik reverse
engineering.

---

## 1. Kod wzięty z oryginału bez zmian — [UP]

| Składnik | Pochodzenie | Jak użyty |
|---|---|---|
| `moonlight-common-c` (Connection, RTSP, Control/Video/Audio stream, depacketizer, ENet, Reed-Solomon) | submoduł moonlight-tizena (upstream) | kompilowany natywnie GCC 6.2; 27/27 plików bez błędów |
| `libgamestream` — `http.c`, `pairing.c`, `mkcert.c` | moonlight-tizen | kompilowane na nagłówkach OpenSSL 1.0.2o (z TV) |
| `h264bitstream`, `opus`, `enet` | wbudowane w moonlight-tizen | dostępne do linkowania (na TV ich nie ma) |
| **Web UI** — `wasm/platform/*.js`, `index.html`, `static/`, `res/` | moonlight-tizen | pakowane do wgt **bez zmian w logice** (lista hostów, parowanie, siatka aplikacji, box art, ustawienia) |

Jedyna ingerencja w UI: usunięcie ładowania modułu WASM (`platform.js`) i
wstrzyknięcie `mlbridge.js` — patrz sekcja 3.

---

## 2. Co trzeba było podmienić — [MOD]

| Oryginał (moonlight-tizen) | Nasza podmiana | Dlaczego |
|---|---|---|
| `wasm/wasmplayer.cpp` (EMSS `ElementaryMediaTrack.AppendPacket`) | `mlclient/video_esplus.c` | EMSS nie istnieje na 5.0 → esplusplayer |
| `wasm/auddec.cpp` (EMSS audio / WebAudio) | `mlclient/audio_opus.c` | j.w., ścieżka audio/x-opus do esplusplayer |
| Transport `Module.*` + `sendMessage`/`handleMessage` (embind) | `pkg/ui/mlbridge.js` (klient message-port) | brak WASM/embind na 5.0 |
| `http.cpp` MakeCert/LoadCert/HttpInit (embind) | `cert_init.c` (CLI) / obsługa certu w `mlservice.c` (usługa) | j.w., natywnie zamiast embind |
| `Platform.h` — bezwarunkowy `#include <emscripten.h>` i makro `Limelog`→`emscripten_log` | 2 drobne atrapy (shim `emscripten.h`) | rdzeń nie woła emscripten; shimy tylko do kompilacji |
| Orkiestracja GameStream robiona w JS przez WASM (serverinfo/pair/launch) | `gs_client.c` (wariant CLI) lub sterowana z JS przez mostek (wariant hybrydowy) | na 5.0 HTTP/krypto muszą działać natywnie |

---

## 3. Kod nowy — brak odpowiednika lub brak API — [NEW]

| Plik | Co to | Dlaczego nowy |
|---|---|---|
| `esplusplayer_tv50.h` | nagłówek ABI starego esplusplayer z TV | **brak publicznego API** — patrz sekcja 5 |
| `pkg/service/mlservice.c` | usługa Tizen (service-app) + **mostek message-port** rozdzielający 11 funkcji UI na silnik | w oryginale UI woła WASM przez embind; natywnie **nie ma takiej metody** — potrzebny IPC wgt↔usługa |
| `pkg/ui/mlbridge.js` | klient `tizen.messageport` zastępujący transport WASM | j.w. po stronie JS |
| `mlclient/esplus_player.c` | wspólny jeden esplusplayer dla wideo i audio (TV pozwala na jeden na proces) | nowa warstwa koordynacji nieobecna w EMSS |
| `mlclient/input_evdev.c` | wejście przez `/dev/input` → `LiSend*` | natywny odpowiednik wejścia HTML5 |
| `mlclient/gs_client.c` | natywna orkiestracja + **własny `xml_search`/`xml_status`** | moonlight-tizen odwołuje się do `xml_search`, ale **nie dostarcza `xml.c`** (robił to JS); napisany bez zależności expat |
| `mlclient/main.c` | natywny `main` klienta CLI | spina całość poza przeglądarką |
| Build/pakowanie: `fetch-tizen-sources.sh`, `fetch-backport-deps.sh`, `build-core.sh`, `build-deps-lib.sh`, `build-client.sh`, `build-video-path.sh`, `pkg/Dockerfile`, `pkg/build-package.sh`, `pkg/service/build-service.sh`, `service/tizen-manifest.xml`, `ui/config.xml` | środowisko i pakiet hybrydowy | nowy toolchain pod 5.0 + pakiet .tpk/.wgt |

Kontrakt mostka (zmapowanie 1:1 z eksportów embind moonlight-tizena):
`makeCert, httpInit, openUrl, pair, startStream, stopStream, cancelRequest,
sendKeyboardEvent, stun, wakeOnLan, toggleStats` — szczegóły w `pkg/ARCHITECTURE.md`.

---

## 4. Krypto/HTTP — dostarczone zależności

Publiczny sysroot 5.0 nie ma nagłówków; wersje dobrane pod biblioteki z TV:
- **mbedTLS 2.28.8** (wbudowane źródło) — dla `PlatformCrypto.c` rdzenia.
- **OpenSSL 1.0.2o** + **libcurl 7.59.0** — nagłówki pobrane, linkowanie do `.so` z TV.
Potwierdzone: cały łańcuch linkuje się do ARM ELF, brak symboli glibc > 2.24.

---

## 5. Wyniki reverse engineering `libesplusplayer.so` — [RE]

Publiczne źródła (esplusplayer 0.0.3 z Tizen 6.5) mają **nowe** API; TV ma
**stare**. Poniższe odczytane z kodu maszynowego (objdump); oznaczenia:
**ASM** = potwierdzone w asemblerze, **TV** = potwierdzone uruchomieniem.

Konwencje wywołań:
- Funkcje zwracają **bool** (1/0), nie kod błędu Tizen. [ASM]
- `submit_packet` zwraca status, gdzie **4 = sukces**. [ASM, TV]
- `pts`/`duration` w **nanosekundach** — biblioteka dzieli przez 1 000 000
  (stała `0xF4240` w asemblerze). [ASM]
- Callbacki **bez `user_data`**; offsety w globalnym `listener_bridge`
  (error +4, buffer_status +8, resource_conflicted +12, eos +16,
  ready_to_prepare +20, prepare_async_done +24). [ASM]
- **Brak eksportu `esplusplayer_destroy`** (mimo że publiczne nagłówki go mają). [ASM]

Struktury:
- `video_stream_info` = **36 B**: codec_data@0, codec_data_length@4, mime@8,
  width@12, height@16, max_width@20, max_height@24, framerate_num@28,
  framerate_den@32. Offsety potwierdzone napisami logów („from->width" itd.). [ASM]
- `es_packet`: type@0, buffer@4, size@8, pts@16, duration@24. [ASM]
- `audio_stream_info` = **24 B**: codec_data@0, len@4, mime@8, bitrate@12,
  channels@16, sample_rate@20. Offsety z napisów logów. [ASM]

Enumy (odczytane ze skoków tablicowych w `SetMimeType_`):
- Wideo: H263=1, **H264=2**, HEVC=3, MPEG1/2/4=4/5/6, VP8=7, VP9=8, WMV3=9
  (bez AV1/MJPEG). [ASM]
- Audio: AAC=1, MP2=2, MP3=3, AC3=4, EAC3=5, VORBIS=6, **DTS=7, OPUS=8**.
  **Uwaga: różni się od publicznego enum** — TV wstawia DTS=7, przez co OPUS=8
  (nie 7). Wzięcie publicznej wartości dałoby zły kodek. [ASM]
- `DisplayType`: NONE=0, **OVERLAY=1**, EVAS=2. Inna wartość → `assert_fail`
  (abort). Okno wideo to `_Ecore_Wl2_Window*`. [ASM]

Zachowanie (z TV):
- Sekwencja prepare: `ready_to_prepare` → trzeba wysłać ~9–10 pakietów →
  `prepare_async_done`. [TV]
- Model konfliktu zasobu: akwizycja planu wideo **zatrzymuje dekoder broadcastu**
  (`sdp-dvde-dec0`); esplusplayer używa innego dekodera (`sdp-mfc-dec0`) na tym
  samym planie. Zwolnienie nie wznawia broadcastu — robi to dopiero powłoka TV. [TV]
- `[sdp-mfc-dec0] ERROR invalid user data a20003` jest **nieszkodliwy**
  (dekodowanie przebiega). Błędy `GStreamer-CRITICAL` (caps) pochodzą z
  wewnętrznego potoku esplusplayer — pojawiają się też przy czystym wideo bez
  audio, więc **nie z naszego OpusHead**. [TV]

Dostęp bez roota (SMACK/Cynara):
- `libesplusplayer.so` etykieta SMACK `_` (ładowalna przez każdego);
  `/dev/video*`, `/dev/sdp*`, `/dev/input/event*` etykieta `*` (dostęp dla
  każdego). esplusplayer nie linkuje Cynary. [TV]
- Test: `esppplay` uruchomiony pod etykietą sandboxa `User::Pkg::...` **zdekodował
  obraz** (create/open/overlay/prepare/submit OK) — instalacja przez dev-paczkę
  bez roota jest realna. [TV]

---

## 6. Wyniki pomiarów [TV]

- Opóźnienie wewnątrz odtwarzacza: **2–3 klatki** (kolejka 34–51 ms przy 1080p60),
  stabilne; z krokiem preroll pts ściągnięte ze stałych ~150 ms.
- Test end-to-end z hostem **Wolf** (games-on-whales) przez GameStream:
  parowanie, launch aplikacji, RTSP, inicjalizacja strumieni, setup wideo/audio,
  enumeracja wejścia (pilot na evdev) — wszystko po stronie klienta zadziałało.
  Strumień zakończył host (błąd konfiguracji Wolfa: Wayland socket runnera),
  nie klient.
- esplusplayer przyjmuje surowy **H.264 Annex-B** (bez AVCC/extradata). [TV]

---

## 7. Metoda

Narzędzia RE: `arm-linux-gnueabi-objdump`/`nm`/`readelf` z toolchainu Tizen 6.2,
analiza skoków tablicowych (`tbb`/`tbh`), rozwiązywanie napisów PC-relative,
korelacja z napisami logów `dlog` (nazwy pól) i z publicznymi nagłówkami 0.0.3
(nazwy, nie wartości). Każdy wynik oznaczony jako potwierdzony w asemblerze lub
na TV, bądź założony.

> Materiał pomocniczy inżynierski. Przed użyciem zewnętrznym części oznaczone
> jako założone wymagają weryfikacji na docelowym sprzęcie.
