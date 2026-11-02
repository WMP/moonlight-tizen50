# roottest — on-device install probe (diagnostic)

`ml_probe.c` is a minimal native `capp` that logs its uid/SMACK and sleeps — used to
test whether the TV installs + launchpad-starts a hand-assembled native `.tpk`.

## ⚠️ Key finding (2026-11-02): do NOT `pkgcmd -i` an unsigned `.tpk` on the TV

Installing an unsigned `.tpk` via `pkgcmd -i` made `tpk-backend` call into
**TrustZone** for signature/UEP verification; that call hung, leaving `tpk-backend`
in **D-state (unkillable)** holding the pkgmgr lock and wedging the secure-world RPC
threads (`nsrpc`, `tzdev`, `tzlogd`). Load climbed to ~27 — the same signature that
preceded an earlier freeze needing a power-cycle. `pkginfo --imd` is "Not supported"
and the app user is **uid 5001**.

**Therefore:** the root install path must NOT use pkgmgr/pkgcmd with an unsigned
package. Reuse an already-registered package's dir (e.g. `ChqHQExU28`, the NaCl
Moonlight), or sign properly for the dev-cert path. See `../../BACKPORT.md`.
