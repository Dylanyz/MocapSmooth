# Editor restarts — always ask, never assume

## The rule

**Never restart, close or relaunch the Unreal editor without asking Dylan first and getting an
explicit yes.** Offer to do it. Do not do it unprompted. If he prefers to close it himself, let him.

His words, 2026-09-15: *"it shouldnt always assume it can restart the engine. sometimes ill be
working on things in there. so it should ask me about that."* Twice the week before: *"dont restart
the editor!"* and *"dont restart for that too! ill get them on next restart."*

**Why:** he is usually mid-shot with unsaved work, and on a big level a relaunch costs minutes plus
his viewport and selection state. The cost of waiting is zero, because a built plugin package can
sit in `%TEMP%\msb` for days with no harm.

## How to apply

1. Check before anything that needs the editor down: `Get-Process UnrealEditor`.
2. Do every part of the job that does *not* need a restart. Docs, `Tools/` scripts, the Python
   fallback, commits and the build itself all work against a live editor.
3. For the part that does, stop and say plainly what is built, what it changes, and that it is
   waiting on him. Give him the one command and let him pick the moment:
   ```powershell
   Tools\build_mocapsmooth.ps1 -InstallOnly
   ```
4. "Built, waiting for your next restart" is a complete and acceptable end state. Do not treat it
   as a failure, and do not keep asking.

`Tools/build_mocapsmooth.ps1` enforces this in code: `-Install` and `-InstallOnly` throw if
`UnrealEditor.exe` is alive rather than corrupting a locked DLL.

## Related

Waiting for a *freshly launched* editor is a different thing and is fine to do unattended: wait for
`Engine Initialization) Total time` in the project's `Saved/Logs/<Project>.log` before sending any
Python. Calling in early looks like a dropped connection.
