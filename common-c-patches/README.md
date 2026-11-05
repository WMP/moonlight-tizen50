# common-c-patches/

`-core` patches applied to the **vendored** `moonlight-common-c` inside the
`third_party/moonlight-tizen` submodule, at build time by `build-deps-lib.sh`
(idempotent: a reverse-check skips an already-applied patch). The submodule tree
and its pin are not changed.

The directory is not called `patches/` on purpose: a repository that consumes
this project with an overlay keeps its own overlay `patches/` directory, which
its sync excludes (see `.github/workflows/dev-loop.yml`). A `patches/` here would
collide with it, so `build-deps-lib.sh` would pick up the overlay patches and the
fix below would not reach the build.

## 0001-control-plaintext-when-not-negotiated.patch (#13)

`encryptedControlStream` was gated only on the host version
(`APP_VERSION_AT_LEAST(7, 1, 431)`), so the client always encrypted the control
and input streams for a 7.1.431 host. A Sunshine-style host (e.g. **Wolf**) that
does **not** advertise `SS_ENC_CONTROL_V2` expects **plaintext** control; it then
cannot decrypt the client's control messages (`EVP_DecryptFinal_ex failed`),
never receives the IDR request, sends no key frame, and the stream ends with
`-101` (`ML_ERROR_NO_VIDEO_FRAME`). PC Moonlight sends plaintext here and works.

The patch only encrypts when encryption was negotiated, or for GFE hosts:

    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431) &&
        (!IS_SUNSHINE() || (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2));

## A1 — the fork alternative

The clean long-term fix is to make the same change in a fork of
`brightcraft/moonlight-tizen` (where `moonlight-common-c` is vendored; its
`.gitmodules` is commented out, so it is not a nested submodule) and repoint the
`third_party/moonlight-tizen` submodule in the root `.gitmodules` to that fork.
Once the fork carries the fix, this patch becomes a no-op (already applied).
