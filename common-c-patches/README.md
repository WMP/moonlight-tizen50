# common-c-patches/

`-core` patches applied to the **vendored** `moonlight-common-c` inside the
`third_party/moonlight-tizen` submodule, at build time by `build-deps-lib.sh`
(idempotent: a reverse-check skips an already-applied patch). The submodule tree
and its pin are not changed.

The directory is not called `patches/` on purpose: a repository that consumes
this project with an overlay keeps its own overlay `patches/` directory, which
its sync excludes (see `.github/workflows/dev-loop.yml`). A `patches/` here would
collide with it, so `build-deps-lib.sh` would pick up the overlay patches and the
patches here would not reach the build.

## Removed: 0001-control-plaintext-when-not-negotiated.patch (#13)

This patch sent the control and input streams in **plaintext** to a Sunshine-style
host that did not negotiate `SS_ENC_CONTROL_V2`. It was wrong for **Wolf**: Wolf
drops every control message that is not of type `ENCRYPTED` (0x0001) and only
logs `[ENET] Received unencrypted message`. So it never got the IDR request
(`INVALIDATE_REF_FRAMES` in plaintext), input or termination. The TV log with
this patch still ended with `-101`.

Unchanged upstream behaviour (`encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431)`)
is correct for Wolf: Wolf decrypts the legacy (V1) AES-GCM control format (16-byte
IV, `iv[0] = seq`, key = `rikey`) and acts on the decrypted `IDR_FRAME` (0x0302),
which is what the Gen7 encrypted packet table sends.

The mechanism stays: `build-deps-lib.sh` applies any `*.patch` in this directory
(none at the moment).
