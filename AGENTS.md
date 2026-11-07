# AGENTS.md — rules for automated contributors (main repo)

This repository is **generated**: it is the clean upstream
[moonlight-tizen50-core](https://github.com/WMP/moonlight-tizen50-core) **plus
`patches/`**, re-assembled by `.github/workflows/sync-from-clean.yml`. The sync
merges core's branch history (original commit names, authors and trees stay
reachable) and then rebuilds the tree as core + `patches/`; a core branch
syncs into the same-named branch here (core `main` → `sync/main`).

## Where to make changes

- **Engine and UI code, build scripts, the esplusplayer API (`esplusplayer_tv50.h`),
  `mlclient/`, `pkg/` (except the device-only parts below)** → change them in the
  **core** repo. They arrive here through the sync PR; editing them here will be
  **overwritten on the next sync**.
- **Maintained directly here only:** `patches/` (the device-integration overlay),
  the device-only files (`pkg/service/`, `pkg/roottest/`), `docs/`, `README.md`
  (this repo's README is its own; core's README is excluded from the sync), and
  the infra (`.github/`, this file). This includes the manual engine loader
  (`pkg/dotnet-host/ManualLoader.cs`, with `docs/ENGINE-LOAD*.md` and the
  `Program.cs`/`Diag.cs` hook-up in `patches/0002-manual-engine-loader.patch`):
  it works around the TV's exec-mmap policy and stays out of the clean upstream
  on purpose.
- If a change in core also needs the overlay adjusted, regenerate
  `patches/0001-*.patch` (see `patches/README.md`) and keep the round-trip valid:
  `clean (minus AGENTS.md, README.md, .gitmodules, third_party/) + patch == this
  tree (minus patches/, sync-from-clean.yml, FUNDING.yml, README.md,
  .gitmodules, third_party/)`.

## Build & release

See `README.md` (**Build** + **Releasing**). Initialize the submodule first:
`git submodule update --init`.
