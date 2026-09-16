# Licensing and credits — what may and may not be said

The repo is **public** and **Apache-2.0**. `LICENSE` and `NOTICE` at the root are the authority.

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
| Quoting Epic engine source to explain a UE behaviour | paraphrase and name the file/function; do not reproduce engine source. UE licensees can read it themselves. |
| Adding a new source file | copy the one-line SPDX header verbatim from an existing file |
| Writing docs or comments | no absolute paths from this machine, no film-project specifics. Strangers read this repo. |
| Documenting another vendor's smoother (Move, Vicon, Xsens…) | same method: observe outputs, tag every claim **measured / from source / inferred**, add the trademark line to `NOTICE` in the same commit |
| Asked to change the licence | MPL-2.0 is a one-file swap plus a README edit. Confirm with Dylan first; it is his call. |
| Tempted to commit `Binaries/`, `Intermediate/`, `Saved/`, an FBX or a `.uasset` | don't. `.gitignore` covers it; keep it that way. Releases carry binaries as attachments, not commits. |

Charlie Driscoll gets credit for popularising the UE Curve Editor workflow; the analysis of what it
computes is original here. Keep both halves of that sentence in `NOTICE`.
