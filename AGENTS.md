# AGENTS.md — rules for automated contributors

This repository is **self-contained**. Everything an agent needs to build, test
and improve it is in this tree. Work only within this repository.

## Hard boundaries

- **Do not connect this repository to any other repository.** Do not add git
  remotes, fetch, pull, push, submodule, subtree or mirror to or from any other
  repo. Do not search for, reference, or link a related, upstream, downstream,
  fork, or "main" repository. If a task seems to require another repository, stop
  and ask the human instead.
- **Keep this repository clean.** Do not add content about device rooting,
  exploits, jailbreaks, security-control bypasses, or reverse-engineering
  write-ups (process, disassembly, offsets, provenance narratives) — not in code,
  comments, docs, commit messages, or issues. This is a normal open-source app.
- **Never commit Samsung's TV libraries** (`libesplusplayer.so` and the other
  files under `tv-libs/`). They are proprietary, used only locally at build time,
  and are provided via the `TV_LIBS_URL` CI secret / a local mount.

## What to work on

The point of this repo is to improve the client itself:

- **Performance** of the streaming engine (`mlclient/`): latency, memory, the
  video/audio paths (`video_esplus.c`, `audio_opus.c`, `esplus_player.c`).
- **The UI** and the UI↔engine bridge (`pkg/ui/mlbridge.js`, the message port).
- **Packaging** and the build (`pkg/`, `*.sh`).

## Build & test

- Build everything in the Docker image: `docker build -t moonlight-tizen-build pkg`.
  See `README.md` for the full flow (engine `.so` → `.tpk` host → web `.wgt`).
- Host self-tests that need no TV: `mlclient/xml_selftest.c` (XML parser),
  `esppplay.c -DSELFTEST` (access-unit splitter). Run these after touching the
  parser or the bitstream code.
- The engine ABI (`mlclient/mlengine.h`) is **additive only**: never change a
  signature, only append functions.
