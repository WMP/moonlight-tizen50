# patches/

The device-integration overlay applied on top of the clean upstream
(`moonlight-tizen50-core`) to reproduce this repo's tree.

`moonlight-tizen50-core` is the clean public upstream where the engine and UI are
developed. **This repo = clean upstream + `patches/`.** The overlay re-adds the
device-integration layer (service path, probes, cert/DRM notes, provenance and
architecture docs) and the in-source notes that the upstream does not carry.

Both repos keep the project at the repository root, so the overlay applies at the
root with plain paths.

Applied automatically by `.github/workflows/sync-from-clean.yml`:

1. the target branch of the clean upstream (its default branch, or the branch
   given in the manual run's `core_branch` input) is **merged with the `ours`
   strategy**, so the original commits (names, authors, dates, trees) become
   reachable from this repo's history;
2. the upstream's files are laid down at the repo root (upstream code only;
   this repo's own `patches/`, sync workflow, `FUNDING.yml`, `AGENTS.md`,
   `.gitmodules`, `third_party/`, and the device-only files listed below are
   preserved), then `git apply patches/*.patch` re-adds this layer — the result
   is committed as one deterministic rebuild commit;
3. the branch is pushed and a pull request opened against `main` here.

Branch mapping: a core branch syncs into the **same-named branch** here
(`core:feature/x` → `feature/x`). Core's `main` goes to `sync/main` here
(a PR needs a different head than its base).

If a patch no longer applies, the upstream changed the same lines this overlay
touches — the sync run fails on purpose so a human can refresh the patch.

The first sync after this model was introduced was refreshed against core
`207360c` (the 4K/HEVC + latency/statistics work): `0001` re-based onto the new
README wording (the "Latency & statistics" section moved the context lines) and
the annotated `esplusplayer_tv50.h` (core documented `resource_conflicted`;
both notes were merged in the overlay's copy).

Patch files:

- `0001-re-root-overlay.patch` — the device-integration overlay (docs, service
  path, probes).
- `0002-manual-engine-loader.patch` — the host-side hook-up of the manual
  ELF32-ARM engine loader: the `Program.cs` fallback (memfd → manual loader →
  packaged file), the `GetDelegate` routing through the manual image's symbol
  table, the diag `anon-exec` probe, and the README link. The loader itself
  (`pkg/dotnet-host/ManualLoader.cs`) and the docs
  (`docs/ENGINE-LOAD.md`, `docs/ENGINE-LOAD.en.md`) are maintained directly in
  this repo (kept out of the rsync in sync-from-clean.yml), because on a
  web-launched host process every fd-backed exec-mmap route is refused (EPERM)
  while anonymous RW memory made executable with mprotect still works — a
  workaround for the TV's exec-mmap policy, not engine or UI development. See
  `docs/ENGINE-LOAD.en.md` for the full decision tree.

Regenerate after intentionally changing the tracked tree:

    # base = clean upstream (minus AGENTS.md, .gitmodules, third_party/);
    # target = this repo (minus patches/, sync-from-clean.yml, FUNDING.yml,
    # .gitmodules, third_party/). Diff base -> target into the patch.
