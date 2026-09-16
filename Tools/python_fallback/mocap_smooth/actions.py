"""Doing the smoothing. Called by the Animation Modifier Blueprint, or by hand from the console.

THE TWO WAYS IN

    1. Right-click AnimSequences -> Animation Modifier(s) -> Add -> AM_MocapSmooth -> Apply.
       That Blueprint holds the sliders and calls straight into `smooth_by_path` below.

    2. From the editor's Python console, with the assets selected in the Content Browser:

           import mocap_smooth as ms
           ms.smooth_selected()                        # Rokoko's _body group at slider 5
           ms.smooth_selected(full_body=6.5)
           ms.smooth_selected(full_body=5, fingers=0)  # _fullbody: smooth the fingers too
           ms.smooth_selected(on_top=True)             # stack another pass on the current result
           ms.revert_selected()                        # back to the untouched original

WHY NOT A PYTHON ANIMATION MODIFIER
    A Python-generated UClass is transient, so the moment an instance of a Python
    `UAnimationModifier` subclass is serialized into an AnimSequence, that asset can never be
    saved:

        Can't save <asset>.uasset: Object (<asset>:AnimationModifiersAssetUserData_0.
        MocapSmoothModifier_0) is an export but is an instance of class
        (/Game/Python/mocap_smooth/modifier_PY.MocapSmoothModifier) which is unsaveable:
        It is transient.

    That is not patchable -- it is how `PyGeneratedClass` works. `UAnimationModifier` is marked
    Blueprintable and its OnApply/OnRevert are Blueprint events, so a Blueprint subclass is an
    ordinary saveable asset. It carries the sliders and their tooltips, and calls in here.

EVERY RUN STARTS FROM THE UNTOUCHED ORIGINAL
    Unless you ask for `on_top`. So moving the slider from 5 to 6 gives you 6, not 5 and then 6.
    See `trackcache.py` for how the original is protected.
"""

import re

import unreal
import numpy as np

from . import regions as R
from . import trackcache
from . import smooth_core

OFF = -1.0
INHERIT = 0.0


# --------------------------------------------------------------------------- small helpers


def _resolve(*vals):
    """First explicit value down the inheritance chain. None means 'do not filter'."""
    for v in vals:
        if v is None:
            continue
        if v < -0.001:
            return None
        if v > 0.001:
            return float(v)
    return None


def _leading_reference_pose(rot, probe=90):
    """True if frame 0 looks like an embedded reference pose rather than captured motion.

    Rokoko's FBX exporter ships 'Include reference pose' enabled, which writes the A-pose as the
    first frame. Measured on this project's takes: frame 0 -> 1 moves 15-19 degrees mean while real
    consecutive frames move 0.07-0.8. Detect the outlier rather than assume it, so a clip that
    genuinely starts fast is not truncated.
    """
    n = rot.shape[1]
    if n < 8:
        return False
    k = min(probe, n - 1)
    a, b = rot[:, :k, :], rot[:, 1:k + 1, :]
    step = np.degrees(2.0 * np.arccos(np.clip(np.abs((a * b).sum(axis=2)), 0.0, 1.0))).mean(axis=0)
    rest = np.median(step[1:])
    return bool(step[0] > 2.0 and step[0] > 10.0 * max(rest, 1e-4))


def _selected_anims():
    out = []
    for a in unreal.EditorUtilityLibrary.get_selected_assets():
        if isinstance(a, unreal.AnimSequence):
            out.append(a)
        else:
            unreal.log_warning("[MocapSmooth] skipping %s (%s, not an AnimSequence)"
                               % (a.get_name(), type(a).__name__))
    return out


def _slider_for(reg, full_body, fingers, hands, upper_body, lower_body, smooth_root):
    if reg == R.FINGERS:
        return _resolve(fingers, hands, full_body)
    if reg == R.HANDS:
        return _resolve(hands, upper_body, full_body)
    if reg == R.UPPER:
        return _resolve(upper_body, full_body)
    if reg == R.LOWER:
        return _resolve(lower_body, full_body)
    if reg == R.PELVIS:
        return _resolve(full_body)      # Rokoko's Lower Body deliberately excludes the hips
    if reg == R.ROOT:
        return _resolve(full_body) if smooth_root else None
    return _resolve(full_body)


# --------------------------------------------------------------------------- the work


def smooth_anim(anim,
                full_body=5.0,
                fingers=OFF,
                hands=INHERIT,
                upper_body=INHERIT,
                lower_body=INHERIT,
                shape="butter",
                hand_shape=None,
                order=2,
                smooth_root=False,
                smooth_pelvis_translation=True,
                skip_leading_reference_pose=True,
                on_top=False,
                slow=None):
    """Smooth one AnimSequence. Defaults are Rokoko's `_body` group at slider 5 (5.5 Hz).

    Region values: -1 Off, 0 inherit the wider region, 0.5-10 a strength (higher is smoother).
    `hand_shape` overrides `shape` for hands and fingers ("gaussian" to avoid ringing).
    `on_top` smooths the animation currently on the asset instead of the untouched original,
    stacking a second pass. The original is left cached either way, so one revert still undoes
    everything.
    """
    AL = unreal.AnimationLibrary
    names = [str(n) for n in AL.get_animation_track_names(anim)]
    dm = anim.get_editor_property("controller").get_model_interface()
    fr = dm.get_frame_rate()
    fs = float(fr.numerator) / float(fr.denominator)

    parent_of = R.parent_map_for_skeleton(anim.get_editor_property("skeleton"))
    if not parent_of:
        unreal.log_error("[MocapSmooth] %s: no bone hierarchy available (the skeleton needs a "
                         "preview mesh). Nothing changed." % anim.get_name())
        return False
    region_of = R.classify(names, parent_of)

    # Always resolve the original first, even for on_top: it is what makes revert work, and on a
    # take that has never been smoothed it is captured here while the asset is still raw.
    pos, rot, scl, captured = trackcache.load_or_capture(anim, names, slow=slow)
    prior_passes = int(trackcache.read_meta(anim).get("passes", 0) or 0)

    if on_top:
        if prior_passes == 0:
            unreal.log_warning("[MocapSmooth] %s: 'on top' asked for, but nothing has been smoothed "
                               "yet -- this is just a normal first pass." % anim.get_name())
        else:
            pos, rot, scl = trackcache.read_current(anim, names, slow, what="the current animation")

    out_pos, out_rot = pos.copy(), rot.copy()
    start = 1 if (skip_leading_reference_pose and _leading_reference_pose(rot)) else 0
    order = max(1, min(4, int(order)))
    touched, skipped = {}, []

    for i, name in enumerate(names):
        reg = region_of.get(name, R.UPPER)
        slider = _slider_for(reg, full_body, fingers, hands, upper_body, lower_body, smooth_root)
        if slider is None:
            skipped.append(reg)
            continue
        fc = smooth_core.rokoko_slider_to_fc(slider)
        if fc >= fs * 0.5:
            skipped.append(reg)
            continue
        shp = (hand_shape or shape) if reg in (R.HANDS, R.FINGERS) else shape
        out_rot[i, start:] = smooth_core.smooth_quaternions(rot[i, start:], fc, fs, order=order, shape=shp)
        if not (reg in (R.ROOT, R.PELVIS) and not smooth_pelvis_translation):
            out_pos[i, start:] = smooth_core.smooth_positions(
                pos[i, start:].astype(np.float64), fc, fs, order=order, shape=shp).astype(np.float32)
        touched.setdefault(reg, []).append(name)

    if not touched:
        unreal.log_warning("[MocapSmooth] %s: every region is set to Off, so there is nothing to do."
                           % anim.get_name())
        return False

    # Write EVERY track, not only the filtered ones. out_* started as a copy of the source, so a
    # region set to Off is restored to it rather than left holding a previous run's output. That is
    # what makes re-running with different settings land on those settings exactly.
    trackcache.write_tracks(anim, names, out_pos, out_rot, scl,
                            label="Mocap smooth (%s)" % anim.get_name(), slow=slow)

    passes = (prior_passes + 1) if (on_top and prior_passes) else 1
    trackcache.record_write(anim, names, passes, {
        "full_body": full_body, "fingers": fingers, "hands": hands, "upper_body": upper_body,
        "lower_body": lower_body, "shape": shape, "hand_shape": hand_shape, "order": order,
        "smooth_root": smooth_root, "smooth_pelvis_translation": smooth_pelvis_translation,
    })

    bands = ", ".join(
        "%s %.2f Hz on %d bones" % (r, smooth_core.rokoko_slider_to_fc(
            _slider_for(r, full_body, fingers, hands, upper_body, lower_body, smooth_root)), len(v))
        for r, v in sorted(touched.items()))
    left = sorted(set(skipped))
    unreal.log("[MocapSmooth] %s: smoothed from %s. %d frames at %g fps. %s%s%s%s" % (
        anim.get_name(),
        "the animation already on the asset (pass %d)" % passes if passes > 1 else "the untouched original",
        dm.get_number_of_frames(), fs, bands,
        (". Left alone: %s" % ", ".join(left)) if left else "",
        ". Frame 0 reference pose skipped" if start else "",
        ". Original captured just now" if captured else ""))
    return True


def revert_anim(anim, slow=None):
    """Restore one AnimSequence from its untouched original."""
    if not trackcache.has_cache(anim):
        unreal.log_error("[MocapSmooth] %s: there is no original cached at %s, so there is nothing "
                         "to revert to. Reimport the source FBX."
                         % (anim.get_name(), trackcache.cache_path(anim)))
        return False
    names = [str(n) for n in unreal.AnimationLibrary.get_animation_track_names(anim)]
    pos, rot, scl, _ = trackcache.load_or_capture(anim, names, slow=slow)
    trackcache.write_tracks(anim, names, pos, rot, scl, label="Mocap smooth revert", slow=slow)
    trackcache.record_write(anim, names, 0, None)
    unreal.log("[MocapSmooth] %s: back to the untouched original." % anim.get_name())
    return True


def forget_original(anim, i_know_this_is_destructive=False):
    """See trackcache.forget_original. Almost never what you want -- reimport the FBX instead."""
    return trackcache.forget_original(anim, i_know_this_is_destructive)


# --------------------------------------------------------------------------- selections


def _over_selection(fn, label, **kwargs):
    anims = _selected_anims()
    if not anims:
        unreal.log_error("[MocapSmooth] nothing selected. Select AnimSequences in the Content Browser.")
        return []
    done = []
    # 300 units per asset: read, filter, write.
    with unreal.ScopedSlowTask(300.0 * len(anims), "%s %d asset(s)" % (label, len(anims))) as slow:
        slow.make_dialog(True)
        for a in anims:
            if slow.should_cancel():
                unreal.log_warning("[MocapSmooth] cancelled after %d asset(s)." % len(done))
                break
            if fn(a, slow=slow, **kwargs):
                done.append(a.get_path_name())
    unreal.log("[MocapSmooth] %s finished on %d asset(s). Save them when you are happy."
               % (label, len(done)))
    return done


def smooth_selected(**kwargs):
    """Smooth every AnimSequence selected in the Content Browser. See smooth_anim for the options."""
    return _over_selection(smooth_anim, "Smoothing", **kwargs)


def revert_selected():
    """Restore every selected AnimSequence from its untouched original."""
    return _over_selection(revert_anim, "Reverting")


# --------------------------------------------------------------------------- Blueprint entry


def _f(v, default=0.0):
    try:
        return float(str(v).strip())
    except Exception:
        return default


def _b(v, default=False):
    s = str(v).strip().lower()
    if s in ("1", "true", "yes", "on"):
        return True
    if s in ("0", "false", "no", "off"):
        return False
    return default


MODIFIER_DEFAULTS = {
    "FullBody": 5.0, "Fingers": OFF, "Hands": INHERIT, "UpperBody": INHERIT, "LowerBody": INHERIT,
    "UseGaussianShape": False, "GaussianForHandsAndFingers": False, "FilterOrder": 2,
    "SmoothRoot": False, "SmoothPelvisTranslation": True, "SkipLeadingReferencePose": True,
    "SmoothOnTopOfPrevious": False,
}


def _modifier_instance(anim):
    """The AM_MocapSmooth Blueprint instance attached to this AnimSequence, or None.

    The Blueprint's OnApply hands us only the asset path -- an object cannot travel through a
    string -- so the settings are read back off the instance that Unreal is applying.
    """
    try:
        aud = anim.get_asset_user_data_of_class(unreal.AnimationModifiersAssetUserData)
        if not aud:
            return None
        for inst in aud.get_editor_property("animation_modifier_instances"):
            if inst and "MocapSmooth" in inst.get_class().get_name():
                return inst
    except Exception as e:
        unreal.log_warning("[MocapSmooth] could not read the modifier settings (%s)" % e)
    return None


def _setting(inst, name, default):
    if inst is None:
        return default
    for key in (name, re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()):
        try:
            return inst.get_editor_property(key)
        except Exception:
            continue
    return default


def smooth_from_asset(anim_path):
    """Entry point for AM_MocapSmooth's OnApply. Reads the sliders off the modifier instance."""
    anim = unreal.load_asset(str(anim_path))
    if not isinstance(anim, unreal.AnimSequence):
        unreal.log_error("[MocapSmooth] %s is not an AnimSequence" % anim_path)
        return False
    inst = _modifier_instance(anim)
    if inst is None:
        unreal.log_warning("[MocapSmooth] %s: could not find the AM_MocapSmooth settings on the "
                           "asset, using the defaults." % anim.get_name())
    g = lambda k: _setting(inst, k, MODIFIER_DEFAULTS[k])
    with unreal.ScopedSlowTask(300.0, "Smoothing %s" % anim.get_name()) as slow:
        slow.make_dialog(True)
        return smooth_anim(
            anim,
            full_body=float(g("FullBody")), fingers=float(g("Fingers")), hands=float(g("Hands")),
            upper_body=float(g("UpperBody")), lower_body=float(g("LowerBody")),
            shape=("gaussian" if g("UseGaussianShape") else "butter"),
            hand_shape=("gaussian" if g("GaussianForHandsAndFingers") else None),
            order=int(g("FilterOrder")), smooth_root=bool(g("SmoothRoot")),
            smooth_pelvis_translation=bool(g("SmoothPelvisTranslation")),
            skip_leading_reference_pose=bool(g("SkipLeadingReferencePose")),
            on_top=bool(g("SmoothOnTopOfPrevious")), slow=slow)


def revert_from_asset(anim_path):
    """Entry point for AM_MocapSmooth's OnRevert."""
    return revert_by_path(anim_path)


def smooth_by_path(anim_path, full_body=5.0, fingers=OFF, hands=INHERIT, upper_body=INHERIT,
                   lower_body=INHERIT, shape="butter", hand_shape="", order=2,
                   smooth_root=False, smooth_pelvis_translation=True,
                   skip_leading_reference_pose=True, on_top=False):
    """Entry point for the Animation Modifier Blueprint.

    Everything arrives as text out of a Format Text node, so every argument is coerced leniently --
    "true"/"false" from Blueprint booleans included.
    """
    anim = unreal.load_asset(str(anim_path).split(".")[0] if "." not in str(anim_path)
                             else str(anim_path))
    if not isinstance(anim, unreal.AnimSequence):
        unreal.log_error("[MocapSmooth] %s is not an AnimSequence" % anim_path)
        return False
    hs = str(hand_shape).strip().lower()
    with unreal.ScopedSlowTask(300.0, "Smoothing %s" % anim.get_name()) as slow:
        slow.make_dialog(True)
        return smooth_anim(
            anim,
            full_body=_f(full_body, 5.0), fingers=_f(fingers, OFF), hands=_f(hands, INHERIT),
            upper_body=_f(upper_body, INHERIT), lower_body=_f(lower_body, INHERIT),
            shape=("gaussian" if str(shape).strip().lower().startswith("g") else "butter"),
            hand_shape=("gaussian" if hs.startswith("g") else ("butter" if hs.startswith("b") else None)),
            order=int(_f(order, 2)), smooth_root=_b(smooth_root),
            smooth_pelvis_translation=_b(smooth_pelvis_translation, True),
            skip_leading_reference_pose=_b(skip_leading_reference_pose, True),
            on_top=_b(on_top), slow=slow)


def revert_by_path(anim_path):
    """Entry point for the Animation Modifier Blueprint's Revert."""
    anim = unreal.load_asset(str(anim_path))
    if not isinstance(anim, unreal.AnimSequence):
        unreal.log_error("[MocapSmooth] %s is not an AnimSequence" % anim_path)
        return False
    with unreal.ScopedSlowTask(100.0, "Reverting %s" % anim.get_name()) as slow:
        slow.make_dialog(False)
        return revert_anim(anim, slow=slow)


# --------------------------------------------------------------------------- cleanup


def strip_modifier_user_data(anim):
    """Remove an unsaveable Python AnimationModifier instance, keeping the animation data.

    Use on any asset that hits "is an instance of class ... which is unsaveable: It is transient".
    The equivalent in the UI is right-click -> Animation Modifier(s) -> Remove Modifier(s), then
    answering **No** to "Should the Modifiers be reverted before removing them?".
    """
    aud = anim.get_asset_user_data_of_class(unreal.AnimationModifiersAssetUserData)
    if aud is None:
        return False
    anim.modify()
    aud.modify()
    aud.set_editor_property("animation_modifier_instances", [])
    aud.set_editor_property("applied_modifiers", {})
    anim.set_editor_property("asset_user_data", [])
    unreal.log("[MocapSmooth] stripped modifier user data from %s; it can be saved now."
               % anim.get_name())
    return True


def strip_modifier_user_data_selected():
    return [a.get_path_name() for a in _selected_anims() if strip_modifier_user_data(a)]
