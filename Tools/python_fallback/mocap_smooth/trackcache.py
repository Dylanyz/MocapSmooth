"""The untouched original, kept safe outside the asset.

THE RULE THIS FILE ENFORCES
    Smoothing always reads from a pristine copy of the raw animation, never from whatever is
    currently on the asset. Filtering an already-filtered curve compounds silently, so without
    that rule "change the slider from 5 to 6" would quietly mean "5, and then 6 on top".

WHERE IT LIVES
    <Project>/Saved/MocapSmooth/<asset>.npz         the original bone tracks. Write-once.
    <Project>/Saved/MocapSmooth/<asset>.meta.json   small bookkeeping: which FBX the original came
                                                    from, and a signature of what we last wrote.
    Both are regenerable and `.dvignore` keeps them out of Diversion. Storing the original inside
    the asset is not an option -- 80 bones x 24,331 frames is ~55 MB that every save would rewrite
    into version control.

WHEN THE ORIGINAL IS REPLACED -- and it is the only time
    When the source FBX changes. The meta file records the source file's path, size and modified
    time at capture; if those differ the take has been re-exported and reimported, so the asset is
    holding genuinely new raw data and a new original is taken. Nothing else replaces it. There is
    no everyday switch that can, which is the point.

THE ONE WAY IT COULD STILL GO WRONG, AND THE GUARD
    Capturing an "original" from an asset that is secretly already smoothed. So every write records
    a cheap signature of what was written (64 sampled frames), and a capture that is about to
    REPLACE an existing original checks it: if the asset still matches what we last wrote, the
    capture is refused and you are told to reimport the FBX instead.

    The very first capture on a take cannot be guarded -- there is nothing to compare against, and
    the asset is assumed raw. Capture before you smooth, which is what happens naturally.
"""

import json
import os
import re
import time
import unreal
import numpy as np

CACHE_SUBDIR = "MocapSmooth"
PROBE_FRAMES = 64


def _base_path(anim):
    root = os.path.join(unreal.Paths.project_saved_dir(), CACHE_SUBDIR)
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", anim.get_path_name().split(".")[0].lstrip("/"))
    return os.path.normpath(os.path.join(root, safe))


def cache_path(anim):
    return _base_path(anim) + ".npz"


def meta_path(anim):
    return _base_path(anim) + ".meta.json"


def has_cache(anim):
    return os.path.exists(cache_path(anim))


# --------------------------------------------------------------------------- bookkeeping


def read_meta(anim):
    try:
        with open(meta_path(anim), encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def _write_meta(anim, meta):
    p = meta_path(anim)
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(meta, fh, indent=2, sort_keys=True)
    except Exception as e:
        unreal.log_warning("[MocapSmooth] could not write %s (%s)" % (p, e))


def source_fingerprint(anim):
    """Identity of the FBX this AnimSequence was imported from, or None if it cannot be read.

    Path + size + modified time. A re-export from Rokoko changes the size or the timestamp, a
    reimport of the identical file changes neither -- which is exactly the distinction we want.
    """
    try:
        aid = anim.get_editor_property("asset_import_data")
        files = [str(f) for f in aid.extract_filenames()] if aid else []
        if not files:
            return None
        f = files[0]
        if not os.path.exists(f):
            # The FBX has moved, or this is a different machine. Fall back to the path alone rather
            # than declaring the original stale and capturing over it.
            return "path:%s" % f
        st = os.stat(f)
        return "%s|%d|%d" % (f, st.st_size, int(st.st_mtime))
    except Exception:
        return None


def probe(anim, names, read=None):
    """A short signature of the animation currently on the asset. ~20 ms.

    64 frames spread across the take is plenty to tell "this is what I wrote last time" from
    "this is something else", which is all it is ever asked.
    """
    try:
        n = anim.get_editor_property("controller").get_model_interface().get_number_of_frames()
        if n <= 0:
            return None
        read = read or _fast_pose_reader(anim, names) or (
            lambda f: unreal.AnimationLibrary.get_bone_poses_for_frame(anim, names, f, False))
        frames = sorted(set(int(round(i * (n - 1) / max(1, PROBE_FRAMES - 1)))
                            for i in range(PROBE_FRAMES)))
        acc = []
        for f in frames:
            for t in read(f):
                q = t.rotation
                acc.extend((q.x, q.y, q.z, q.w))
        a = np.round(np.asarray(acc, dtype=np.float64), 5)
        # Rounded before hashing: the asset quantises keys, so an exact bit compare would be noise.
        return "%08x" % (abs(hash(a.tobytes())) & 0xFFFFFFFF)
    except Exception as e:
        unreal.log_warning("[MocapSmooth] could not probe %s (%s)" % (anim.get_name(), e))
        return None


# --------------------------------------------------------------------------- reading


def _fast_pose_reader(anim, names):
    """f -> list of FTransform, one per name, or None if the fast route is unavailable.

    `AnimationLibrary.get_bone_poses_for_frame` evaluates the WHOLE pose once PER BONE:
    Engine/Source/Editor/AnimationBlueprintLibrary/Private/AnimationBlueprintLibrary.cpp,
    `GetBonePosesForTimeInternal` constructs an FAnimPose and calls `GetAnimPoseAtTime` *inside*
    the bone loop, then keeps one bone out of it. An 80-track take therefore pays 80 full
    animation evaluations per frame and throws 79 away.

    Evaluating once per frame and reading the bones off that pose is the same data -- both use
    default FAnimPoseEvaluationOptions -- for 1/80th of the evaluation work. Measured on an
    80-track 3,491-frame take: 7.40 -> 0.34 ms/frame, 22x. Agreement over a whole take is
    0.0037 deg / 0.0003 cm, which is the asset's own key quantisation floor; the residual is
    Epic's frame->time conversion, so the by-frame route is marginally the more exact one.

    Checked against the old path on three frames before it is trusted. The tolerance is a
    wrong-data guard, not a precision test -- 1e-3 on the summed quaternion components is ~0.1 deg.
    """
    try:
        opts = unreal.AnimPoseEvaluationOptions()
        ext = unreal.AnimPoseExtensions
        local = unreal.AnimPoseSpaces.LOCAL

        def read(f):
            pose = ext.get_anim_pose_at_frame(anim, f, opts)
            return [ext.get_bone_pose(pose, n, local) for n in names]

        def slow_read(f):
            return unreal.AnimationLibrary.get_bone_poses_for_frame(anim, names, f, False)

        n = anim.get_editor_property("controller").get_model_interface().get_number_of_frames()
        for f in (0, min(1, n - 1), n // 2):
            for a, b in zip(read(f), slow_read(f)):
                qa, qb = a.rotation, b.rotation
                va, vb = a.translation, b.translation
                if (abs(qa.x - qb.x) + abs(qa.y - qb.y) + abs(qa.z - qb.z) + abs(qa.w - qb.w) > 1e-3
                        or abs(va.x - vb.x) + abs(va.y - vb.y) + abs(va.z - vb.z) > 1e-2):
                    unreal.log_warning(
                        "[MocapSmooth] single-eval read disagrees with Epic's, using the slow path")
                    return None
        return read
    except Exception as e:
        unreal.log_warning("[MocapSmooth] single-eval read unavailable (%s), using the slow path" % e)
        return None


def read_current(anim, names, slow=None, what="tracks"):
    """(pos, rot, scale) as arrays shaped (B, N, 3/4/3) in local space, as the asset stands NOW.

    The legacy raw-track read paths (`GetRawTrackData` and friends) are empty stubs on an
    AnimationSequencerDataModel asset, which every modern AnimSequence uses, and the data model's
    own `GetBoneTrackByName` returns a static empty track there. Evaluating per frame is the only
    supported route; `_fast_pose_reader` makes that one evaluation instead of one per bone.
    """
    AL = unreal.AnimationLibrary
    n = anim.get_editor_property("controller").get_model_interface().get_number_of_frames()
    b = len(names)
    read = _fast_pose_reader(anim, names) or (
        lambda f: AL.get_bone_poses_for_frame(anim, names, f, False))
    # Rotations at full precision so a revert is exact; float32 quaternions cost ~0.04 deg.
    # Positions are centimetres, where float32 is ~1e-3 cm -- far below anything that matters.
    pos = np.empty((b, n, 3), np.float32)
    rot = np.empty((b, n, 4), np.float64)
    scl = np.empty((b, n, 3), np.float32)
    step = max(1, n // 100)
    t0 = time.time()
    for f in range(n):
        for i, t in enumerate(read(f)):
            v, q, s = t.translation, t.rotation, t.scale3d
            pos[i, f] = (v.x, v.y, v.z)
            rot[i, f] = (q.x, q.y, q.z, q.w)
            scl[i, f] = (s.x, s.y, s.z)
        if slow is not None and f % step == 0:
            if slow.should_cancel():
                raise RuntimeError("cancelled while reading %s" % what)
            slow.enter_progress_frame(step / float(n) * 100.0, "Reading %s %d/%d" % (what, f, n))
    dt = time.time() - t0
    unreal.log("[MocapSmooth] read %d frames x %d bones in %.1f s (%.2f ms/frame)"
               % (n, b, dt, dt / max(1, n) * 1000.0))
    return pos, rot, scl


def _load_npz(anim, names, n):
    """The cached original if present and still matching this asset's shape, else None."""
    p = cache_path(anim)
    if not os.path.exists(p):
        return None
    try:
        z = np.load(p, allow_pickle=False)
        if [str(x) for x in z["names"]] == list(names) and z["pos"].shape[1] == n:
            return z["pos"], z["rot"], z["scl"]
        unreal.log_warning("[MocapSmooth] the cached original for %s no longer matches the asset "
                           "(bone list or length changed)" % anim.get_name())
    except Exception as e:
        unreal.log_warning("[MocapSmooth] cached original unreadable (%s)" % e)
    return None


def _capture(anim, names, slow, reason):
    unreal.log("[MocapSmooth] capturing the untouched original for %s -- %s"
               % (anim.get_name(), reason))
    pos, rot, scl = read_current(anim, names, slow, what="the original")
    p = cache_path(anim)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    np.savez_compressed(p, names=np.array(list(names)), pos=pos, rot=rot, scl=scl)
    meta = read_meta(anim)
    meta.update({
        "source": source_fingerprint(anim),
        "captured": time.strftime("%Y-%m-%d %H:%M:%S"),
        "frames": int(pos.shape[1]),
        "bones": int(pos.shape[0]),
        "passes": 0,
        "last_write_probe": None,
        "last_settings": None,
    })
    _write_meta(anim, meta)
    return pos, rot, scl


def _refuse_because_ours(anim, names, meta, context):
    """True if the asset still holds OUR last smoothing pass, so capturing now would bake it in."""
    last = meta.get("last_write_probe")
    if not last:
        return False
    if probe(anim, names) != last:
        return False
    unreal.log_error(
        "[MocapSmooth] %s: refusing to take a new original -- %s, but the animation on the asset is "
        "still the smoothing applied on %s. Capturing now would make that smoothed result the new "
        "'original' and the raw would be gone. Reimport the FBX (which resets the asset to raw), "
        "then smooth again."
        % (anim.get_name(), context, meta.get("last_written", "an earlier run")))
    return True


def _backup(anim):
    p = cache_path(anim)
    try:
        bak = p + ".bak"
        if os.path.exists(bak):
            os.remove(bak)
        os.replace(p, bak)
        unreal.log_warning("[MocapSmooth] previous original kept at %s" % bak)
    except Exception as e:
        unreal.log_warning("[MocapSmooth] could not back up the previous original (%s)" % e)


def load_or_capture(anim, names, slow=None):
    """The untouched original tracks. Captured on first use; only a changed FBX ever replaces it.

    Returns (pos, rot, scl, captured), where `captured` says whether this call did the capturing.
    """
    n = anim.get_editor_property("controller").get_model_interface().get_number_of_frames()
    meta = read_meta(anim)
    cached = _load_npz(anim, names, n)

    if cached is None:
        return _capture(anim, names, slow, "first time this take has been smoothed") + (True,)

    stored, current = meta.get("source"), source_fingerprint(anim)
    if stored and current and stored != current:
        # The FBX behind this asset changed, so the asset is holding new raw data. This is the one
        # and only situation in which an existing original is replaced.
        if _refuse_because_ours(anim, names, meta, "the source FBX looks different"):
            return cached + (False,)
        _backup(anim)
        return _capture(anim, names, slow,
                        "the source FBX changed, so this take was reimported") + (True,)

    return cached + (False,)


def forget_original(anim, i_know_this_is_destructive=False):
    """Throw the cached original away so the next run captures a new one from the asset as it stands.

    Almost never the right thing. The normal way to get a new original is to reimport the FBX,
    which is detected on its own. This exists for the case where the cached original is genuinely
    wrong and a reimport is not possible. The old one is kept as `.npz.bak`.
    """
    if not i_know_this_is_destructive:
        unreal.log_error(
            "[MocapSmooth] forget_original() does nothing unless you pass "
            "i_know_this_is_destructive=True. It discards the raw copy of %s, and the next smooth "
            "treats whatever is on the asset as the new raw. If the asset is holding a smoothed "
            "result, that becomes your 'original' permanently. Reimporting the FBX is the safe route."
            % anim.get_name())
        return False
    names = [str(x) for x in unreal.AnimationLibrary.get_animation_track_names(anim)]
    if _refuse_because_ours(anim, names, read_meta(anim), "you asked to forget the original"):
        return False
    if os.path.exists(cache_path(anim)):
        _backup(anim)
    meta = read_meta(anim)
    meta["last_write_probe"] = None
    _write_meta(anim, meta)
    unreal.log_warning("[MocapSmooth] %s: original discarded. The next smooth captures a new one."
                       % anim.get_name())
    return True


# --------------------------------------------------------------------------- writing


def write_tracks(anim, names, pos, rot, scl, label="Mocap smooth", slow=None):
    """Write (B, N, ...) arrays back. One bracket == one editor undo transaction."""
    ctrl = anim.get_editor_property("controller")
    ctrl.open_bracket(label)
    try:
        for i, name in enumerate(names):
            ok = ctrl.set_bone_track_keys(
                name,
                [unreal.Vector(float(a), float(b), float(c)) for a, b, c in pos[i]],
                [unreal.Quat(float(a), float(b), float(c), float(d)) for a, b, c, d in rot[i]],
                [unreal.Vector(float(a), float(b), float(c)) for a, b, c in scl[i]],
            )
            if not ok:
                unreal.log_warning("[MocapSmooth] write failed on bone %s" % name)
            if slow is not None:
                slow.enter_progress_frame(100.0 / len(names), "Writing %s" % name)
    finally:
        ctrl.close_bracket()


def record_write(anim, names, passes, settings):
    """Remember what we just wrote, so a later capture can recognise it and refuse."""
    meta = read_meta(anim)
    meta.update({
        "last_write_probe": probe(anim, names),
        "last_written": time.strftime("%Y-%m-%d %H:%M:%S"),
        "passes": int(passes),
        "last_settings": settings,
    })
    if not meta.get("source"):
        meta["source"] = source_fingerprint(anim)
    _write_meta(anim, meta)
