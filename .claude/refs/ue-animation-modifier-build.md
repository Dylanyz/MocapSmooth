# Installing the Animation Modifier in a UE project

Two implementations ship here. **Use the C++ plugin.** Built and verified on UE 5.8.2,
CitySample, 2026-09-15.

| | the plugin at the repo root (C++) | `Tools/python_fallback/mocap_smooth/` (Python) |
|---|---|---|
| Install | copy the repo to `<Project>/Plugins/MocapSmooth/`, build | copy to `<Project>/Content/Python/`, build a Blueprint |
| Property tooltips + slider clamps | **yes**, in code | no — see Limitations |
| Whole apply, 80 tracks x 3,492 frames | **1.2 s** (0.9 s of it UE's recompression) | ~13 s |
| Track read | one call per bone | one pose evaluation per frame |
| Correct key count | **yes** | no — off by one, see below |
| Needs | nothing, if the shipped `Binaries/` match the engine build id; otherwise one compile + editor restart | nothing |

The Python route is kept only for a project with no C++ toolchain. It carries two known bugs
(below) that are not worth fixing now that the plugin exists.

## Installing the plugin

1. Copy this repo (or a release zip) to `<Project>/Plugins/MocapSmooth/` — or junction it into the engine, see `../rules/build-and-install.md`.
2. Add `{"Name": "MocapSmooth", "Enabled": true}` to the `.uproject`'s `Plugins` array. (Project
   plugins are discovered anyway, but be explicit.)
3. Launch and check: right-click an AnimSequence → Animation Modifier(s) → Add → **Mocap Smooth**.
4. Verify with `MocapSmooth.SelfTest` in the console before trusting it.

**A target project does NOT have to be a C++ project.** `Binaries/Win64/` ships with the plugin
here, and Unreal loads a prebuilt editor module as-is — no compiler, no `Source/` in the project,
the project stays Blueprint-only.

The catch is `Binaries/Win64/UnrealEditor.modules`, which carries a **`BuildId`**. Unreal loads the
DLL only when that matches the engine install's own build id; otherwise it offers to rebuild, and
that needs a toolchain. The shipped binary was built against:

    BuildId 55116800   UE 5.8.2   Win64   Development Editor

So: same engine install (or any install with the same build id) → drop-in. Different engine
version, a source build, or another platform → build once (below) on a machine with a toolchain,
then copy the new `Binaries/Win64/` back over this one so the next project is a drop-in again.

### Building it

1. **Close the editor** — Live Coding holds a build lock, and a brand-new module cannot be
   hot-loaded regardless. **Ask Dylan before closing his editor.**
2. ```
   & "<Engine>/Build/BatchFiles/Build.bat" <Project>Editor Win64 Development -Project="<...>.uproject" -WaitMutex
   ```
3. Reopen, and run `MocapSmooth.SelfTest`.

Version control: track `Plugins/MocapSmooth/Source/**` and the `.uplugin`; do **not** track
`Binaries/` or `Intermediate/` in a project repo — the copy that travels between projects is the
one at this repo root.

## What is in it

| File | What |
|---|---|
| `MocapSmoothFilter.{h,cpp}` | the DSP, ported line-for-line from `Tools/smooth_core.py`, plus `SelfTest()` |
| `MocapSmoothRegions.{h,cpp}` | bone region classification by hierarchy walk |
| `MocapSmoothCache.{h,cpp}` | the protected original, the source-FBX fingerprint, the refusal guard |
| `MocapSmoothModifier.{h,cpp}` | the `UAnimationModifier` subclass: properties, tooltips, apply/revert |
| `MocapSmoothEditorModule.cpp` | module impl + the `MocapSmooth.SelfTest` console command |

Reading uses `IAnimationDataModel::GetBoneTrackTransforms(FName, TArray<FTransform>&)` — **one
call per bone**. That API carries no `UFUNCTION`, which is precisely why the Python version had to
evaluate a whole pose per frame instead.

## Traps, all hit while building this

- **UHT rejects multi-line string literals in metadata.** `meta = (ToolTip = "a" "b")` fails with
  *"Found string constant when expecting ',' or ')'"*. Write the tooltip as a `/** doc comment */`
  above the property instead — UHT turns that into the tooltip, and it can span lines freely.
- **`AnimationModifier.h` includes `AnimationBlueprintLibrary.h`**, so `AnimationBlueprintLibrary`
  must be a **Public** dependency of any module whose public header includes it.
- **`IAnimationDataController::FScopedBracket` takes a reference**, not a pointer. Passing
  `&Controller` silently picks the `TScriptInterface` overload and fails to compile.
- **`UAnimationModifier::ApplyToAnimationSequence` is not a `UFUNCTION`**, so a scripted test has
  to call `Instance.call_method("OnApply", (Anim,))`. The right-click UI runs the real path.
- **`AnimationModifiersAssetUserData.add_animation_modifier_of_class` returns a bool**, not the
  instance. Fetch it from `animation_modifier_instances` afterwards.
- **Build.bat needs the editor fully closed** — *"Unable to build while Live Coding is active"*.

## `GetNumberOfKeys() == GetNumberOfFrames() + 1`

Frames are intervals, keys are samples (`UAnimationSequencerDataModel::GetNumberOfKeys` literally
returns `GetNumberOfFrames() + 1`). Anything that reads or writes `GetNumberOfFrames()` samples is
one key short of the track — measured 3491 vs 3492 on an 80-track Rokoko take.

The C++ takes its sample count from what the model actually hands back for the first track and
requires every other track to match, so it cannot make this mistake regardless of the convention.
**The Python implementation gets this wrong**, so every take it smoothed has a slightly wrong final
key and every `.npz` original it wrote is one sample short.

**Migrating those takes does NOT need a reimport.** Python never read or wrote the last key, so it
is still the original raw value sitting on the asset. Measured on an 80-track 3,492-frame take: the
C++ capture's first 3,491 samples match the `.npz` raw to 0.008 deg mean (the asset's quantisation
floor), and the 3490 -> 3491 step is 0.0049 deg against a 0.0747 deg median, i.e. ordinary motion
rather than a duplicate or garbage. So:

1. `mocap_smooth.strip_modifier_user_data(anim)` — drop the old modifier instance.
2. `mocap_smooth.revert_anim(anim)` — restores samples 0..N-1 from the `.npz`; the last key is
   already raw.
3. Add the plugin's modifier and Apply — it captures a complete, genuinely raw original.

## The protected original

Every apply reads a pristine copy of the raw animation from
`<Project>/Saved/MocapSmooth/<asset>.mocapraw`, never from what is on the asset, so re-applying at
a different strength lands on that strength instead of stacking.

- `<asset>.json` beside it records the source FBX's **path, size and mtime**. The only thing that
  ever replaces the original is that changing — a reimport — and it is detected on its own. There
  is no everyday switch that can overwrite it.
- Every write records a 64-frame signature of what was written. A capture that would **replace** an
  existing original, **or** that runs when the cache is missing or mismatched, is refused if the
  asset still matches that signature. That is the one path by which smoothing could be baked in as
  the raw, and it is closed.
- `Smooth On Top Of Previous` opts into deliberate stacking; the original stays cached, so one
  Revert still undoes every pass.

## Verifying a build

`MocapSmooth.SelfTest` re-runs the reference implementation's six checks against the compiled
filter. Expected (measured 2026-09-15):

```
selftest 1 slider mapping: ok
selftest 2 knee: measured 2.827 Hz, expected 2.807 Hz: ok      <- 0.802 * fc, filtfilt applies it twice
selftest 3 zero phase: asym 1.39e-16: ok
selftest 4 gaussian overshoot 0.00e+00: ok
selftest 5 butterworth overshoot 3.47%: ok
selftest 6 quat norm err 3.99e-08: ok
```

Then, on a scratch duplicate of a real take:

1. Apply at the defaults → the log names the regions and cutoffs, and the asset **saves**.
2. Re-apply at the same settings → bit-identical, no compounding.
3. `Smooth On Top Of Previous` → error grows (0.0252° → 0.0365° on an 80-track take at slider 5)
   and the meta file's `passes` increments.
4. Revert → 0.0000°.

The C++ and Python implementations agree to the digit on (1)–(4) and produce the same region split
(fingers 38, hands 2, upper 22, lower 16, pelvis 1, root 1 on an 80-track Manny/Rokoko skeleton)
from completely independent hierarchy walks.

## The Python fallback, and why the Blueprint route exists

`Tools/python_fallback/mocap_smooth/` is the same design in Python, driven by an Animation Modifier **Blueprint**
whose `OnApply` calls `ExecutePythonCommand`. It exists because a Python-generated UClass is
**transient**: attach a Python `UAnimationModifier` subclass to an AnimSequence and that asset can
never be saved —

```
Can't save <asset>.uasset: Object (<asset>:AnimationModifiersAssetUserData_0.MocapSmoothModifier_0)
is an export but is an instance of class (/Game/Python/mocap_smooth/modifier_PY.MocapSmoothModifier)
which is unsaveable: It is transient.
```

Recovery for an already-poisoned asset: right-click → Animation Modifier(s) → Remove Modifier(s),
answer **No** to "should the Modifiers be reverted before removing them?", or
`mocap_smooth.strip_modifier_user_data_selected()`.

`UAnimationModifier` is `Blueprintable` with `BlueprintNativeEvent` OnApply/OnRevert, so a
Blueprint subclass is an ordinary saveable asset. Building one (variables, the Format-Text →
`ExecutePythonCommand` graph, `set_blueprint_variable_instance_editable`, which is mandatory or the
properties never appear) is recorded in this file's git history if it is ever needed again.

**Its limitation, and the reason for the C++ port:** Blueprint variable tooltips and slider ranges
live in `UBlueprint::NewVariables[].MetaDataArray`, a bare `UPROPERTY()` that Python cannot see
(`get_editor_property("new_variables")` fails) and that no MCP toolset exposes. The only setters
are `FBlueprintEditorUtils::SetBlueprintVariableMetaData` (UnrealEd C++) and typing into the
Details panel by hand.
