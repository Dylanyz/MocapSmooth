# Alternatives considered

Every approach weighed for offline mocap smoothing in Unreal, and why each was kept or dropped.
Recorded so nobody re-litigates them from scratch.

## The brief these were judged against

Non-destructive, adjustable later, zero lag, lightweight to operate, scales to takes of 13 to 36
minutes, and safe to render through Movie Render Graph.

## Summary

| Approach | Non-destructive | Adjustable | Zero lag | Verdict |
|---|---|---|---|---|
| **Offline filter, baked into the asset** | yes, with a cache | yes, re-bake | yes | **chosen** |
| Animation Modifier wrapper around that | yes | yes, slider | yes | **chosen delivery** |
| Two assets plus Sequencer weight cross-fade | yes | yes, keyable | yes | dropped, user rejected two assets |
| IK Retargeter Filter Bones op | yes | yes | **no** | fallback only |
| Layered Control Rig with spring units | yes | yes | **no** | rejected |
| Bake to Control Rig plus Curve Editor filter | no | no | yes | rejected at scale |
| Post-process Anim Blueprint | yes | yes | **no** | rejected |
| Driving Rokoko's engine headlessly | n/a | n/a | yes | rejected |

## Kept

### Offline filter baked into the asset

Read the bone tracks, filter them zero-phase, write them back. Plain animation data afterwards, so
it is deterministic in scrubbing, in PIE and in Movie Render Graph, with no warm-up and no state.

The only real problem is the undo story, solved with a sidecar cache plus the source FBX. See
[ue-implementation.md](ue-implementation.md).

### Animation Modifier as the delivery mechanism

Right-click an AnimSequence, Animation Modifier(s), Apply. Sliders in a details panel, works on a
multi-selection, settings stored per asset. Native UX, nothing to maintain, no custom window.

## Dropped, and why

### Two assets plus a Sequencer weight cross-fade

Keep the raw asset and a smoothed copy on two rows of the same binding, key one Weight curve from 0
to 1 to dial smoothing per moment. Mathematically this **is** a smoothing filter:

```
(1-w)*raw + w*G(raw)  =  ((1-w)*delta + w*G) convolved with raw
```

so it varies strength continuously with no lag, and Sequencer normalises overlapping absolute
sections so the blend is exact. Genuinely elegant, and it gives true per-moment control.

Dropped because it doubles the assets and the retargets, and because in practice smoothing gets set
once per take rather than animated. Worth remembering if per-moment control ever becomes a real
requirement.

### IK Retargeter Filter Bones op

`IKRetargetFilterBoneOp`, shipped by Epic in 5.7+, a per-bone One-Euro adaptive low-pass. Zero
custom code, and it sits naturally in a pipeline that already retargets.

Rejected as a default because **One-Euro is causal**. It lags, and the lag varies with velocity, so
fast moves shift more than slow ones. On a performance-capture shoot where body, face and audio were
aligned frame by frame, that is a real cost. Keep it as an Epic-native A/B comparison.

### Layered Control Rig with spring units

A layered Control Rig reads the incoming animated pose through its backwards solve and writes
additive deltas on top, with a keyable weight. Combined with `RigUnit_SpringInterp` or
`RigUnit_AlphaInterp` it gives a live smoothing knob you can key in Sequencer. It is the most
obvious "filter with a slider" in Unreal.

Rejected for three compounding reasons:

1. **Stateful, therefore lagging.** Springs are causal by definition.
2. **Scrub-dependent.** Jumping backwards or starting mid-clip gives different results than linear
   playback. Movie Render Graph warm-up frames fix convergence, not lag.
3. **Build cost.** A rig covering 82 bones is real work to author and maintain.

Reserve springs for things that should feel physical, like props and hair, not for cleaning capture.

### Bake to Control Rig plus Curve Editor filter

The commonly taught route. Works, and is zero-code. Rejected here for long takes:

- Bake To Control Rig destroys the link to the AnimSequence;
- 24,331 frames times hundreds of rig controls is not workable in the Curve Editor UI;
- it filters Euler channels, which wrap at ±180°, and IK controls, which drags foot plants;
- the FFT wraps, so on a long clip the end of the take couples to the start.

Full detail, including exactly what UE's filter computes:
[driscoll-ue-curve-editor.md](driscoll-ue-curve-editor.md).

### Post-process Anim Blueprint

`AnimNode_SpringBone`, `AnimNode_AnimDynamics`, `AnimNode_DeadBlending`, `AnimNode_Inertialization`,
with parameters driven from Sequencer. Non-destructive and adjustable, but stateful, so the same lag
and scrub problems as the Control Rig route, plus a per-character setup cost.

### Driving Rokoko's engine headlessly

Tempting once you know its JSON-RPC surface: use Rokoko's real filter without opening the app. It
does run. Rejected anyway:

- it only loads its own `.rkk-scene` packages, so you would still open Preview to get takes in
  (`importGltfClip` accepts a hand-built glTF and produces a static pose);
- it is a private, undocumented protocol that can change with any update;
- edits take seconds per call.

Once the filter is characterised there is nothing left to gain. See
[rokoko-measurement.md](rokoko-measurement.md).

### Building a custom GUI

A tkinter or Slate window with sliders and a preview. Rejected: it is a thing to maintain and a
thing to click, and the Animation Modifier details panel already provides sliders natively.

## Things that sound relevant but are not

- **Rokoko's Live Link plugin.** Pure pass-through, no filtering at all on the UE side.
- **Rokoko Studio 2.x Filters.** Locomotion, knee pop, drift fix, treadmill, toe bend. None is a
  general body low-pass; the only averaging is on vertical hip motion.
- **MetaHuman Animator realtime smoothing.** `HyprsenseRealtimeSmoothingNode` and friends are face
  and realtime only.
- **Live Link preprocessors.** Transform deadband and axis switching only.
- **Motion Trails / Mixer Trails.** Visualisation, not filtering.
