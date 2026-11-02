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

1. the clean upstream's files are laid down at the repo root (upstream code only,
   not its repo-level `AGENTS.md`; this repo's own `patches/`, sync workflow and
   `FUNDING.yml` are preserved);
2. `git apply patches/*.patch` re-adds this layer;
3. the result is opened as a pull request.

If a patch no longer applies, the upstream changed the same lines this overlay
touches — the sync run fails on purpose so a human can refresh the patch.

Regenerate after intentionally changing the tracked tree:

    # base = clean upstream (minus AGENTS.md) ; target = this repo (minus patches/,
    # sync-from-clean.yml, FUNDING.yml). Diff base -> target into the patch.
