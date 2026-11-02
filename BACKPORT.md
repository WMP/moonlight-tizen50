# Backport moonlight-tizen -> Tizen 5.0 natywny

Cel: szybki natywny klient Moonlight na Tizen 5.0 (armv7l softfp, glibc 2.24,
GCC 6.2), wideo przez `/usr/lib/libesplusplayer.so` (ABI w `esplusplayer_tv50.h`,
zmierzone opóźnienie 2-3 klatki). Źródło: brightcraft/moonlight-tizen (WASM/EMSS).

## Co jest wspólne (zweryfikowane w drzewie źródeł)

| Warstwa | Pliki | Uwaga |
|---|---|---|
| Rdzeń strumienia | `moonlight-common-c/src/*.c` (Connection, RTSP, Control/Video/Audio, depacketizer, ENet, Reed-Solomon) | czysty C, przenośny |
| Krypto rdzenia | `moonlight-common-c/src/PlatformCrypto.c` | **mbedTLS** (cipher + ctr_drbg + entropy, AES-GCM) |
| Parowanie/HTTP | `libgamestream/{http,pairing,mkcert}.c` | **libcurl + OpenSSL 1.0** |
| Audio dekod | `wasm/auddec.cpp` | opus_multistream_decode; backend EMSS lub WebAudio |
| Wideo (EMSS) | `wasm/wasmplayer.cpp` | `ElementaryMediaTrack.AppendPacket` - odpowiednik `esplusplayer_submit_packet` |
| Wejście | `wasm/input.cpp`, `wasm/gamepad.cpp` | cienkie nakładki na API moonlight-common-c |
| UI/sterowanie | `wasm/platform/*.js`, `index.html` | Tizen WebAPI |

Tylko ~6 plików nakładek platformy trzeba przepisać. Rdzeń, libgamestream,
h264bitstream, opus, enet są wspólne.

## Biblioteki na Tizen 5.0 (sprawdzone w tv-libs)

Są: `libcrypto.so.1.0.0`, `libssl`, `libcurl.so.4.5.0`, PulseAudio, ALSA,
libevdev, libinput, Ecore_Wl2, GLES/EGL, `libesplusplayer.so`.
Brak: libopus, libenet (oba wbudowane w repo - OK).
Brak nagłówków dev w sysroot dla openssl/curl/mbedtls - trzeba dostarczyć.

## Milestone 1 krok 1 - WYNIK (2026-11-01)

`build-core.sh`: rdzeń (moonlight-common-c + enet + reedsolomon),
**27/27 plików kompiluje się GCC 6.2 na sysroot, 0 błędów** (2 nieszkodliwe
ostrzeżenia strict-aliasing w enet/unix.c, CMSG_DATA - są też w upstream).
Potrzebne były tylko 2 drobne atrapy dla zbędnych odwołań Emscripten w
`Platform.h` (żaden plik .c rdzenia nie woła API emscripten):
- `#include <emscripten.h>` w gałęzi POSIX (linia 46) - zbędny,
- makro `Limelog` woła `emscripten_log` - port przekieruje na
  `ListenerCallbacks.logMessage`.

**Największe ryzyko z planu (czy GCC 6.2 zbuduje rdzeń) - ZDJĘTE.**

## Milestone 1 krok 2 - WYNIK (2026-11-01)

Skrypty: `fetch-backport-deps.sh` (pobiera zależności), `build-deps-lib.sh`
(buduje i linkuje). Dobrano wersje pod biblioteki TV: TV ma OpenSSL **1.0.2o**
i libcurl **7.59.0**.

- **mbedTLS 2.28.8 LTS**: 96/96 plików kompiluje GCC 6.2, archiwum statyczne.
- **PlatformCrypto.c** (`-DUSE_MBEDTLS`, API 2.x): kompiluje się na mbedTLS.
- **libgamestream** (http/pairing/mkcert): kompiluje się na nagłówkach OpenSSL
  1.0.2o + curl 7.59 (opensslconf.h ręczny dla 32-bit ARM, `OPENSSL_NO_KRB5`).
- **Test linkowania**: cały łańcuch (rdzeń + mbedTLS statycznie + libgamestream)
  linkuje się do ARM ELF przeciw prawdziwym `tv-libs` libssl.so.1.0.0 /
  libcrypto.so.1.0.0 / libcurl.so.4. **Wszystkie symbole krypto/curl rozwiązane.**
  Żaden symbol glibc > 2.24. Nierozwiązane zostają tylko globale aplikacji
  (`g_Cert`, `g_PrivateKey`, `g_UniqueId`, `g_CertHex`) - dostarcza je natywny `main`.

**Ryzyko zależności/linkowania z planu - ZDJĘTE.** Nie trzeba portować
libgamestream na mbedTLS; OpenSSL 1.0.2o z TV wystarcza.

## Milestone 1 krok 3 (część) - WYNIK (2026-11-01)

Sink wideo: `mlclient/video_esplus.c` - implementacja
`DECODER_RENDERER_CALLBACKS` nad esplusplayer (natywny odpowiednik
`wasm/wasmplayer.cpp`). `submitDecodeUnit` skleja łańcuch LENTRY (Annex-B)
w jeden pakiet ES i wysyła z PTS z `presentationTimeMs`. Skrypt:
`mlclient/build-video-path.sh`.

- Kompiluje się 0 ostrzeżeń.
- **Cały natywny tor wideo linkuje się w jeden ARM ELF**: rdzeń + PlatformCrypto
  (mbedTLS) + libgamestream (OpenSSL/curl) + sink + esplusplayer + ecore, przeciw
  prawdziwym `tv-libs`. NEEDED: libesplusplayer, libecore*, libssl, libcrypto,
  libcurl. Żaden symbol glibc > 2.24. `--allow-shlib-undefined` potrzebny dla
  przechodnich zależności esplusplayer tylko-z-TV (jak w `build-esppplay.sh`).

## Milestone 1 krok 3b (część) - WYNIK (2026-11-01)

- `mlclient/cert_init.c`: generuje/ładuje cert klienta (PEM), ustawia
  globale `g_Cert/g_PrivateKey/g_UniqueId/g_CertHex`, curl init. Natywny
  odpowiednik `http.cpp` MakeCert/LoadCert/HttpInit.
  **Przetestowane pod qemu-arm** na OpenSSL 1.0.2 z TV: run 1 tworzy ważny X.509
  (CN=NVIDIA GameStream Client, 10 lat), zapisuje client.pem/key/uniqueid;
  run 2 wczytuje istniejący (ta sama uniqueid). Cert poprawny wg `openssl x509`.
- `mlclient/main.c`: SZKIELET. Wiąże cert init + STREAM_CONFIGURATION +
  sink wideo + zaślepki audio/listener + `LiStartConnection`. Buduje się i
  linkuje w pełny klient ELF (`mlclient/build-client.sh`), brak symboli
  glibc > 2.24.

Uwaga do uruchamiania: klienta (z esplusplayer) nie da się uruchomić pod qemu -
esplusplayer ciąga `libopen-debugmenu.so` (lib tylko-z-TV). Testować na TV.

## Milestone 1 krok 3b cd. - orkiestracja GameStream (NA SUCHO, 2026-11-01)

`mlclient/gs_client.c`: pełna orkiestracja HTTP przeniesiona z
moonlight-embedded `client.c`, dostosowana do libgamestream forka
(`http_request(url, ppkstr, data)` + `gs_pair(...)`):
1. serverinfo po HTTP (:47989) -> major version, PairStatus;
2. parowanie `gs_pair` jeśli trzeba (PIN z env `MOONLIGHT_PIN`, wpisywany na hoście);
3. serverinfo po HTTPS (:47984, cert przypięty) -> appversion/GfeVersion;
4. klucz rikey (AES) + rikeyid jak w moonlight-embedded;
5. launch/resume -> `sessionUrl0` = rtspSessionUrl.

Zawiera własny, bezzależnościowy `xml_search`/`xml_status` (fork odwołuje się do
nich, ale nie dostarcza xml.c; unikamy expat). **Przetestowane jednostkowo na
hoście** (`xml_selftest.c` + `xml_selftest_stubs.c`): pola, tag z atrybutami,
brzegowy prefiks tagu, błędny status.

`main.c` wywołuje teraz `gs_orchestrate` i `LiStartConnection`. Cały klient
buduje się i linkuje (`build-client.sh`), 0 ostrzeżeń, brak glibc > 2.24.
Uruchomienie: `mlclient <host-ip> <appid> [w h fps bitrate]`, env `MOONLIGHT_PIN`.

**UWAGA: `gs_client.c` nie jest testowany end-to-end** - wymaga hosta Sunshine/GFE
w LAN. URL-e, parametry i nazwy pól odwzorowane 1:1 z moonlight-embedded.

## Milestone 3 - wejście (NA SUCHO, 2026-11-01)

`mlclient/input_evdev.c`: czyta `/dev/input/event*` przez `<linux/input.h>`
(stabilne ABI jądra, bez EFL) w osobnym wątku i przekazuje do moonlight-common-c:
- klawiatura -> `LiSendKeyboardEvent` (mapa Linux KEY_* -> Windows VK, modyfikatory),
- mysz -> `LiSendMouseMoveEvent`/`LiSendMouseButtonEvent`/`LiSendScrollEvent`,
- pad -> `LiSendMultiControllerEvent` (przyciski, sticki, triggery, d-pad/HAT).
Fallback dla starych nagłówków jądra (brak `BTN_SOUTH` itd.). `main.c` wywołuje
`input_init()` po `LiStartConnection`, `input_shutdown()` przy wyjściu.
Buduje się i linkuje w pełny klient (0 ostrzeżeń).

**UWAGA: nieprzetestowane na TV.** Do sprawdzenia: czy pilot TV pojawia się na
evdev (czy przez CEC/uinput) - `input_init()` loguje nazwy urządzeń. Mapa klawiszy
pokrywa typowe klawisze (shortcut: rozszerzyć `map_vk()` w razie potrzeby).
Dostęp do `/dev/input` wymaga uprawnień/roota.

## Milestone 2 - audio (NA SUCHO, 2026-11-01)

ABI audio esplusplayer **zdeasemblowane** (patrz `esplusplayer_tv50.h`):
struktura `audio_stream_info` (24 B, offsety z napisów logów) i enum mime
(`add_audio`/`SetMimeType_`): **OPUS=8** na TV (nie 7 - TV wstawia DTS=7).

`mlclient/esplus_player.c`: jeden wspólny odtwarzacz dla wideo i audio
(TV pozwala na jeden na proces). Kolejność z Connection.c gwarantuje:
ar.init (dodaj audio) -> dr.setup (dodaj wideo + okno) -> dr.start (prepare+start)
-> ar.start. Dzięki temu audio i wideo mają **jeden zegar** (dobra synchronizacja),
jak ścieżka EMSS forka.

`mlclient/audio_opus.c`: `AUDIO_RENDERER_CALLBACKS` - dodaje ścieżkę
audio/x-opus (OPUS=8), submit pakietów Opus wprost (bez libopus - dekoduje
GStreamer w esplusplayer), codec_data = OpusHead wg RFC 7845 z
`OPUS_MULTISTREAM_CONFIGURATION`, PTS liczony z licznika próbek.
`video_esplus.c` przerobiony na wspólny odtwarzacz. Pełny klient linkuje się.

**UWAGA: nieprzetestowane na TV.** Ryzyko: czy esplusplayer przyjmie OpusHead
dokładnie tak jako codec_data. Fallback: dekodować libopus (wbudowany) i
submit audio/x-raw PCM, albo PCM do PulseAudio. (Struktura i OPUS=8 są pewne
z `.so`; akceptacja OpusHead - nie.)

## Dalej

- M1 krok 4: test na TV z hostem Sunshine - parowanie + obraz + audio + wejście z /tmp.
- Do potwierdzenia na realnym strumieniu: poprawność `opensslconf.h`, flagi
  `OPENSSL_NO_*` TV, zachowanie rikey/parowania, odpowiednik `kUltraLow`,
  routing pilota (evdev vs CEC), akceptacja OpusHead przez esplusplayer,
  sekwencja prepare z dwiema ścieżkami.

Niewiadome do potwierdzenia: odpowiednik EMSS `LatencyMode::kUltraLow` w
esplusplayer (brak `set_low_latency_mode` w eksportach - szukać w ustawieniach
bufora/display); synchronizacja A/V przy dwóch ścieżkach esplusplayer.
