# Licensing and credits — what may and may not be said

The repo is **public** and **Apache-2.0**. `LICENSE` and `NOTICE` at the root are the authority. The
rules every one of Dylan's plugin repos shares (SPDX headers, no engine code, no machine paths, never
commit binaries, ask before any licence change) are in the plugin hub's `refs/licensing.md`. This file
holds what is MocapSmooth's.

## What Apache covers

Everything authored here: the C++ module, `Tools/`, the Python fallback, the reference docs.
Apache-2.0 section 4d makes `NOTICE` travel with any redistribution, which is what carries the
credit to **Dylan Gitalis (Mad Rice)** and the clean-room framing below.

## The clean-room framing — never break it

The Rokoko spec in `.claude/refs/rokoko-*.md` was obtained by **observing a shipping product's
outputs** on a licensed installation, for interoperability, and reimplemented from textbook DSP.
The repo ships **no Rokoko code, binaries or assets**, and `Tools/rokoko_probe.js` only talks to
a local installation over its own IPC. Keep it that way:

| Situation | Do |
|---|---|
| Tempted to paste a decompiled or extracted snippet, a bundled JS file, a scene file from Rokoko's install | **don't.** Describe the behaviour, cite the measurement. |
| Documenting another vendor's smoother (Move, Vicon, Xsens…) | same method: observe outputs, tag every claim **measured / from source / inferred**, add the trademark line to `NOTICE` in the same commit |
| Adding a new source file | copy the one-line SPDX header verbatim from an existing file |

Charlie Driscoll gets credit for popularising the UE Curve Editor workflow; the analysis of what it
computes is original here. Keep both halves of that sentence in `NOTICE`.
