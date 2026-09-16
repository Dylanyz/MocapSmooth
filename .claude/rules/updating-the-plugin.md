# "Update the plugin" — the runbook

When Dylan says **"update the plugin"**, "install the update", "push the new build", "get the
latest in", or opens a chat in this repo and asks for the changes to take effect, follow this
exactly. Do not improvise, and do not skip step 1.

## Step 0 — one call tells you where things stand

```powershell
Tools\build_mocapsmooth.ps1 -Status
```

Read-only, safe with the editor running. It prints the newest source timestamp, the installed DLL,
any waiting build, whether the editor is up, and the single next action.

## Step 1 — is a C++ build even needed?

| If the change is | Then |
|---|---|
| Docs, `README.md`, `.claude/` | commit. Done. |
| `Tools/smooth_core.py` or the Python fallback | re-copy the vendored `smooth_core.py` into `Tools/python_fallback/mocap_smooth/`, run `python Tools/smooth_core.py` (self-test). Done, no restart. |
| Anything in `Source/` | build, then install, which needs a restart. Continue below. **If the C++ filter changed, `smooth_core.py` must change identically** — the two are kept line-for-line equivalent and `MocapSmooth.SelfTest` checks the compiled one against the reference numbers. |

Say which of these it is before doing anything.

## Step 2 — build (safe while he works)

```powershell
Tools\build_mocapsmooth.ps1
```

Packages to `%TEMP%\msb`. The editor can stay open. If it fails, read the failure table in
`build-and-install.md` before retrying.

## Step 3 — stop and ask

**The install overwrites a DLL the running editor holds open, so the editor must be closed first.
Ask him. Wait for a yes. Never close it yourself.** See `editor-restarts.md`.

Tell him three things in one short message: what the update changes, that it is built and waiting,
and the one command. Then stop.

```powershell
Tools\build_mocapsmooth.ps1 -InstallOnly
```

"I'll get it on my next restart" is a complete answer. Do not ask twice.

## Step 4 — after he relaunches

```
MocapSmooth.SelfTest
```

then apply on a scratch duplicate and confirm: same settings re-applied → bit-identical; Revert →
0.0000°. If a `UPROPERTY` was renamed, existing modifier instances on assets lose that value —
say so, and re-set it on the assets that carry the modifier.

## Step 5 — close the loop

- Commit and push. The repo is public; see `licensing-and-credits.md`.
- Update `.claude/refs/` and `README.md` if behaviour changed, per `.claude/refs/maintenance.md`.
- Bump `VersionName` in the `.uplugin` and cut a GitHub release with `Binaries/Win64` attached
  when the change is user-visible.
