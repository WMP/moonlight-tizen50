# AGENTS.md — rules for automated contributors (main repo)

This repository is **generated**: it is the clean upstream
[moonlight-tizen50-core](https://github.com/WMP/moonlight-tizen50-core) **plus
`patches/`**, re-assembled by `.github/workflows/sync-from-clean.yml`.

## Where to make changes

- **Engine and UI code, build scripts, the esplusplayer API (`esplusplayer_tv50.h`),
  `mlclient/`, `pkg/` (except the device-only parts below)** → change them in the
  **core** repo. They arrive here through the sync PR; editing them here will be
  **overwritten on the next sync**.
- **Maintained directly here only:** `patches/` (the device-integration overlay),
  the device-only files (`pkg/service/`, `pkg/roottest/`), `docs/`, and the infra
  (`.github/`, this file). This includes the manual engine loader
  (`pkg/dotnet-host/ManualLoader.cs` + the host fallback and the diag `anon-exec`
  probe, carried by `patches/0002-manual-engine-loader.patch`): it works around
  the TV's exec-mmap policy and stays out of the clean upstream on purpose.
- If a change in core also needs the overlay adjusted, regenerate
  `patches/0001-*.patch` (see `patches/README.md`) and keep the round-trip valid:
  `clean (minus AGENTS.md, .gitmodules, third_party/) + patch == this tree
  (minus patches/, sync-from-clean.yml, FUNDING.yml, .gitmodules, third_party/)`.

## Build & release

See `README.md` (**Build** + **Releasing**). Initialize the submodule first:
`git submodule update --init`.
