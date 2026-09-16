# Implementing smoothing inside Unreal Engine

UE 5.8 facts verified in a live editor (5.8.1 / 5.8.2, 2026-09-14 and 2026-09-15), plus the design
for shipping the filter as an Animation Modifier.

## Reading and writing AnimSequence bone tracks

This is the part that wastes the most time if you go in blind.

### The data model changed

Every AnimSequence in a modern project uses **`AnimationSequencerDataModel`**, the MovieScene-backed
model, even when the console variable `a.AnimSequencer.UseSequencerModel` reads 0. All the legacy
read paths silently return nothing:

```python
dm.get_bone_animation_tracks()              # -> []
dm.get_bone_track_by_name("spine_03")       # -> empty struct, name "None", index -1
AnimationLibrary.get_raw_track_rotation_data(anim, "pelvis")   # -> 0 keys
AnimationLibrary.is_valid_raw_animation_track_name(anim, "pelvis")  # -> False
```

None of that means the asset is broken. Do not chase it.

### Reading: evaluate instead -- but NOT with `get_bone_poses_for_frame`

The obvious call is 80x slower than it looks:

```python
poses = AL.get_bone_poses_for_frame(anim, names, frame, False)   # local space -- but see below
```

`GetBonePosesForTimeInternal`
(`Engine/Source/Editor/AnimationBlueprintLibrary/Private/AnimationBlueprintLibrary.cpp`, ~line 87)
constructs an `FAnimPose` and calls `GetAnimPoseAtTime` **inside the per-bone loop**, keeping one
bone out of each full evaluation. An 80-track take pays 80 whole-animation evaluations per frame
and discards 79.

Evaluate once per frame instead and read the bones off that pose:

```python
opts  = unreal.AnimPoseEvaluationOptions()          # the same defaults the slow path uses
ext   = unreal.AnimPoseExtensions
local = unreal.AnimPoseSpaces.LOCAL

def read(frame):
    pose = ext.get_anim_pose_at_frame(anim, frame, opts)
    return [ext.get_bone_pose(pose, n, local) for n in names]
```

**Measured 2026-09-15, 80 tracks: 7.40 -> 0.34 ms/frame, 22x.** Agreement over a whole take is
0.0037 deg / 0.0003 cm -- the asset's own key-quantisation floor -- and the residual is Epic's
frame->time conversion, so the by-frame route is marginally the *more* exact of the two. Worth
checking the two against each other on a few frames at startup and falling back if they disagree.

Real capture costs after the fix, 80 bones at 30 fps: 3,491 f in 2.1 s, 24,331 f in 14 s,
58,373 f in 34 s, 65,519 f in 38 s. Before it, the 65,519-frame take was ~8 minutes.

Cache the result regardless -- see "the protected original" in
[ue-animation-modifier-build.md](ue-animation-modifier-build.md).

Useful metadata:

```python
dm = anim.get_editor_property('controller').get_model_interface()
dm.get_frame_rate()        # FrameRate struct
dm.get_number_of_frames()
dm.get_num_bone_tracks()
dm.get_bone_track_names()
```

### Writing

```python
ctrl = anim.get_editor_property('controller')      # AnimationDataController
ctrl.open_bracket("Mocap smooth")
ctrl.set_bone_track_keys(bone_name, pos_keys, rot_keys, scale_keys)
ctrl.close_bracket()
```

Also available: `set_frame_rate`, `set_number_of_frames`, `resize`, `add_bone_track`,
`remove_bone_track`, curve and attribute equivalents. `update_bone_track_keys` (partial range) is
**not** exposed to Python.

One bracket equals one undo transaction, so a mistake is recoverable with Ctrl+Z **in the editor** —
Python itself has no undo.

**Exercised and verified 2026-09-15** (UE 5.8.2, 80-bone Rokoko/Manny AnimSequence, 24,331 frames):

- `set_bone_track_keys` returns `True`, preserves frame count and track count, and an
  identity round-trip is exact to the asset's own key quantisation (0.004 deg).
- Key arrays must cover the **whole** track. `update_bone_track_keys` (partial range) is not
  exposed, so a partial edit means reading everything, patching, and writing everything back.
- Writing is cheap: ~0.1 s per bone for 24,331 keys. Building the `unreal.Quat` / `unreal.Vector`
  lists is cheaper still (0.04 s per 24k). **Reading is the whole cost.**
- Measured read *via `get_bone_poses_for_frame`*: 8 ms per frame for all 80 bones, so ~28 s for
  3,491 frames and ~195 s for 24,331. Reading one bone at a time is no faster in total (2.6 s x 80).
  **Use the single-evaluation reader above instead** -- same data, 0.34 ms/frame. Cache either way.
- With the fast reader, writing becomes the larger half of an apply.
- The legacy raw-track paths are dead on a Sequencer-backed AnimSequence (every modern one):
  `UAnimationBlueprintLibrary::GetRawTrackData` and friends are empty `{}` stubs, and
  `UAnimationSequencerDataModel::GetBoneTrackByName` returns a static empty `FBoneAnimationTrack`.
  `IAnimationDataModel::GetBoneTrackTransforms` would return a whole track in one call but carries
  no `UFUNCTION`, so Python cannot reach it. Per-frame evaluation is the only route.
- Note the fourth argument is **`extract_root_motion`**, not a raw/compressed switch:
  `get_bone_poses_for_frame(anim, names, frame, extract_root_motion, preview_mesh=None)`.

### Reading the bone hierarchy

Region masking needs parent links, and the `Skeleton` asset does not expose them. `unreal.Skeleton`
has no hierarchy accessor at all; the route that works is **`unreal.SkeletonModifier`**, which reads
off a **SkeletalMesh**:

```python
mod = unreal.SkeletonModifier()
mod.set_skeletal_mesh(skeleton.get_skeleton_preview_mesh())
names = [str(b) for b in mod.get_all_bone_names()]
parent = str(mod.get_parent_name("hand_l"))          # -> "lowerarm_l"
```

Two gotchas. A **root bone reports the mesh name as its parent** (`root` -> `SKM_Manny`), so treat
any parent outside the bone set as "no parent". And the preview mesh can be null, in which case fall
back to an asset-registry scan for any `SkeletalMesh` whose `Skeleton` tag matches.

### Environment

`numpy` and `scipy` are both importable in UE 5.8's editor Python (3.11.8, numpy 1.26.4,
scipy 1.11.4), so `butter` and `filtfilt` need no vendoring. If you want zero dependencies anyway,
use [Tools/smooth_core.py](../../Tools/smooth_core.py), which implements both in numpy alone.

## Animation Modifiers

The right delivery mechanism: right-click selected AnimSequences, Animation Modifier(s), Apply.
Works on a multi-selection.

### Verified API

- `unreal.AnimationModifier` exposes exactly `on_apply(anim_sequence)` and
  `on_revert(anim_sequence)`, plus the property `reapply_post_owner_change` ("call its reapply
  function after any change made to the owning asset").
- Modifiers attach through `AnimationModifiersAssetUserData`:
  `add_animation_modifier_of_class`, `animation_modifier_instances`, `applied_modifiers`.
- A Python subclass registers cleanly and gets real sliders:

```python
@unreal.uclass()
class SmoothModifier(unreal.AnimationModifier):
    full_body = unreal.uproperty(float, meta=dict(
        Category="Smoothing", UIMin="0.5", UIMax="10.0", ClampMin="0.5", ClampMax="10.0"))

    def _post_init(self):
        self.full_body = 7.0

    @unreal.ufunction(override=True)
    def on_apply(self, anim_sequence):
        ...

    @unreal.ufunction(override=True)
    def on_revert(self, anim_sequence):
        ...
```

### The compounding problem

`on_revert` is **not** an automatic snapshot. Epic only runs whatever you implement. Filtering an
already-filtered curve compounds silently, so any adjustable strength must always filter from the
original.

Stashing the original inside the asset is not viable. For 82 bones at 24,331 frames that is roughly
55 MB per take, and 233 MB on a 342-bone retargeted version — which would also be rewritten into
version control on every save.

Two mechanisms instead, which complement each other:

1. **Sidecar cache.** On first apply, write the original bone tracks to
   `<Project>/Saved/MocapSmooth/<asset-guid>.npz` and always filter from that. Outside version
   control, regenerable, makes slider changes instant.
2. **`reapply_post_owner_change = True`.** Reimporting the source FBX rebuilds the source data and
   re-runs the modifier, so the FBX on disk remains the ultimate ground truth if the cache is lost.

`on_revert` then restores from the cache, or asks for a reimport if it is missing.

### Suggested properties

Mirror Rokoko's groups, but resolve the overlap explicitly. Precedence: **Fingers beats Hands beats
Upper/Lower Body beats Full Body.**

| Property | Range | Meaning |
|---|---|---|
| `FullBody` | 0.5–10 | master, same scale and direction as Rokoko's slider (10 = smoothest) |
| `Hands` | Off, Inherit, or 0.5–10 | hand bone plus all finger bones |
| `Fingers` | Off, Inherit, or 0.5–10 | finger bones only — the set Rokoko's **Body** group excludes |
| `UpperBody` | Off, Inherit, or 0.5–10 | spine and up, excluding fingers |
| `LowerBody` | Off, Inherit, or 0.5–10 | hips and down, **excluding the pelvis** |
| `SmoothRoot` | bool, default off | leave the root alone unless drift needs it |
| `Shape` | enum | Butterworth (Rokoko-identical) or Gaussian (no ringing) |

Each region's slider becomes `fc = 10.5 - slider` Hz.

**Three per-region states, not two.** `Inherit` (take the master value) and a value are not enough —
a region also has to be able to opt *out* of the master. Encode as a float where `0` = Inherit and
`-1` = Off, or as a proper enum. Without `Off` there is no way to express Rokoko's Body group.

### Body = Full Body with Fingers Off

Rokoko's `_body` ("all bones in the hierarchy, excluding the finger bones") is not a fifth region —
it is the master applied everywhere with the finger set exempt:

```
FullBody = 7, Fingers = Off
```

This matters more than its absence from the first draft suggested. In Rokoko's own tutorial the
presenter runs everything on Full Body or Body, and switches to Body exactly when smoothing has
flattened the fingers: *fingers are very expressive, and smoothing costs that expressiveness.*
Treat **Body as the working default** and Full Body as the choice only when the glove data is noisy
enough that stiff fingers beat jittery ones.

**Body excludes fingers, not hands.** Setting `Hands = Off` is a different and wrong thing — it
would leave the wrist/hand bone itself unfiltered, so wrist jitter survives into an otherwise smooth
arm. That is why `Fingers` exists separately from `Hands`: Body's exemption stops at the hand bone's
children.


### Python or C++

A Python-registered modifier needs no compile and works immediately when loaded from
`Content/Python/init_unreal.py`. Its one weakness: a modifier instance saved on an asset only
resolves if that script registered at editor start. If it fails, the asset shows a null modifier
entry — annoying, but the animation data itself is untouched.

**Where the class actually lands [measured]:** a package under `Content/Python/` registers at
`/Game/Python/<pkg>/<module>_PY.<ClassName>` — e.g.
`/Game/Python/mocap_smooth/modifier_PY.MocapSmoothModifier` — not the `/Engine/PythonTypes.<Name>`
path a loose script gets. The file-derived path is the stabler of the two, so keep the modifier in
a package inside `Content/Python/`.

**Attaching from script** (the same thing the right-click menu does) — note the asset is the first
argument, which the Python signature does not make obvious:

```python
aud = anim.get_asset_user_data_of_class(unreal.AnimationModifiersAssetUserData)
if aud is None:
    anim.add_asset_user_data_of_class(unreal.AnimationModifiersAssetUserData)
    aud = anim.get_asset_user_data_of_class(unreal.AnimationModifiersAssetUserData)
aud.add_animation_modifier_of_class(anim, cls)     # (anim_sequence_base, modifier_class)
mod = list(aud.animation_modifier_instances)[0]
```

**`reapply_post_owner_change` is a C++ property, not a Python attribute.** `self.reapply_post_owner_change = True`
in `_post_init` silently does nothing (it just binds a Python attribute, no error). Use
`self.set_editor_property("reapply_post_owner_change", True)`.

A small C++ `UAnimationModifier` subclass in a plugin removes that fragility entirely and is the
right end state. Prototype in Python, port if the fragility actually bites.

**`@unreal.uenum()` works** and gives a real dropdown in the modifier's Details panel:

```python
@unreal.uenum()
class MocapSmoothShape(unreal.EnumBase):
    BUTTERWORTH = unreal.uvalue(0)
    GAUSSIAN = unreal.uvalue(1)
```

Reading one back bites, though: the value arrives as an `EnumBase`, and **`int()` raises
`TypeError`** on it. Use `getattr(v, "value", v)` before any arithmetic or comparison.

`Category="A|B"` nests the properties into sub-groups. `meta=dict(ClampMin=..., ClampMax=...)`
is honoured, so a slider that must express Off / Inherit / value needs its clamp to start at -1.

### Working reference implementation

The CitySample project has a complete Python one at `Content/Python/mocap_smooth/`
(`modifier.py` the uclass, `regions.py` the hierarchy walk, `trackcache.py` the sidecar cache).
Measured behaviour: first apply on a 3,491-frame take 32 s, re-apply from cache 3.8 s, cache
6 MB compressed. Copy it as a starting point rather than rebuilding from this document.

## Which asset to filter

Filter the **source skeleton**, then retarget. On a typical MetaHuman retarget the target skeleton
has 342 bone tracks, of which around 222 are finger, bulge and half bones and around 50 are arm
twist and corrective bones. Those are derived rig, and filtering them independently risks breaking
the corrective relationships. The source capture skeleton (82 bones in a Rokoko export) is clean FK,
is where the noise actually is, and is where Rokoko itself filters.

Cost: changing the smoothing means re-running the retarget. That is scriptable —
`unreal.IKRetargetBatchOperation` with `IKRetargetBatchOperationInputs`.

## Region masking

Classify bones by **walking the skeleton hierarchy** from named roots, not by string prefixes.
Prefix matching is unreliable on a MetaHuman skeleton, where names like `upperarm_correctiveroot_l`,
`calf_twistcor_02_r`, `pinky_03_bulge_l`, `wrist_inner_l`, `bigtoe_01_r` and `ankle_bck_r` do not
partition cleanly.

Hierarchy walk: find the hand bones; **the hand bone plus everything below it is Hands, everything
strictly below it is Fingers** — the two differ by exactly one bone per side, and that one bone is
what separates Rokoko's Body group from Full Body. Find the pelvis; its spine child subtree minus
hands is Upper Body; its thigh children subtrees are Lower Body; the pelvis itself belongs to
neither, matching Rokoko.

On a Rokoko 82-bone export the hand bones are `LeftHand` / `RightHand`; on a MetaHuman skeleton
`hand_l` / `hand_r`. Do not try to catch fingers by name — `pinky_03_bulge_l` and `wrist_inner_l`
are why the walk exists.

## Other UE mechanisms worth knowing

- **IK Retargeter `Filter Bones` op** (`IKRetargetFilterBoneOp`, 5.7+): a per-bone **One-Euro**
  adaptive low-pass with Responsiveness, CutoffFrequency, VelocityCutoffFrequency, Alpha. Epic-made
  and convenient, but One-Euro is **causal**, so it lags. Only reach for it if lag does not matter.
- **Control Rig simulation units**: `RigUnit_SpringInterp` / `V2` / `Vector` / `Quaternion`,
  `RigUnit_AlphaInterp`, `RigUnit_Accumulate*Lerp`. Stateful, so lag plus scrub-dependent results.
- **Sequencer Anim Layers** (`unreal.AnimLayers`, types BASE / ADDITIVE / OVERRIDE) and skeletal
  animation section `weight` / `blend_type`. Overlapping non-additive sections are weight-normalised,
  which makes a wet/dry cross-fade between a raw and a smoothed asset mathematically exact.
- **Animation Mixer** (`MovieSceneAnimMixer`, 5.8, experimental, off by default): mix layers with
  keyable weights and blend masks. No smoothing modifier of its own.
