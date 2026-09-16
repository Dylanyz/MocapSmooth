"""Bone region classification for mocap smoothing.

Regions mirror Rokoko Studio Preview's smoothing groups. They are resolved by WALKING THE
SKELETON HIERARCHY, never by string prefixes -- names like `upperarm_correctiveroot_l`,
`pinky_03_bulge_l` and `wrist_inner_l` do not partition cleanly on a MetaHuman skeleton.

Rokoko's groups, for reference:
    _fullbody   all bones
    _body       all bones EXCEPT the finger bones      <- the practical default
    _upperbody  spine and up, excluding fingers
    _lowerbody  hips and down, EXCLUDING the hips bone
    _hands      hand bones including all finger bones

`_body` is not a region of its own: it is FullBody with FINGERS switched off. That is why
FINGERS exists separately from HANDS -- the exemption stops at the hand bone's children, so
under Body the wrist is still filtered.
"""

import unreal

ROOT = "root"
PELVIS = "pelvis"
UPPER = "upper"
LOWER = "lower"
HANDS = "hands"
FINGERS = "fingers"

ORDER = [FINGERS, HANDS, UPPER, LOWER, PELVIS, ROOT]

_PELVIS_NAMES = ("pelvis", "hips", "hip")
_LOWER_HINTS = ("thigh", "upleg", "leg", "femur", "foot", "ankle", "toe", "ball")
_UPPER_HINTS = ("spine", "neck", "head", "clavicle", "shoulder", "chest", "torso", "arm")


def parent_map_for_skeleton(skeleton):
    """{bone_name: parent_name or None} for every bone in the skeleton, or None if unavailable.

    Needs a SkeletalMesh: `unreal.SkeletonModifier` reads the hierarchy off a mesh, not off the
    Skeleton asset. Tries the preview mesh first, then any mesh in the project that uses this
    skeleton.
    """
    mesh = skeleton.get_skeleton_preview_mesh() or _find_mesh_using(skeleton)
    if mesh is None:
        return None
    mod = unreal.SkeletonModifier()
    mod.set_skeletal_mesh(mesh)
    names = [str(b) for b in mod.get_all_bone_names()]
    known = set(names)
    out = {}
    for n in names:
        p = str(mod.get_parent_name(n))
        # a root bone reports the mesh/None as its parent; anything outside the set is "no parent"
        out[n] = p if p in known and p != n else None
    return out


def _find_mesh_using(skeleton):
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    f = unreal.ARFilter(
        class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "SkeletalMesh")],
        recursive_classes=True,
    )
    want = skeleton.get_path_name()
    for ad in ar.get_assets(f):
        tag = ad.get_tag_value("Skeleton")
        if tag and want in str(tag):
            return unreal.load_asset(str(ad.package_name))
    return None


def classify(track_names, parent_of):
    """{track_name: region}. `parent_of` comes from parent_map_for_skeleton()."""
    children = {}
    for n, p in parent_of.items():
        if p:
            children.setdefault(p, []).append(n)

    def ancestors(n):
        out, p, guard = [], parent_of.get(n), 0
        while p and guard < 512:
            out.append(p)
            p = parent_of.get(p)
            guard += 1
        return out

    def subtree(n):
        out, stack = set(), [n]
        while stack:
            c = stack.pop()
            out.add(c)
            stack.extend(children.get(c, ()))
        return out

    # --- hand bones: shallowest bones whose name mentions "hand".
    # Catches hand_l / hand_r (UE) and LeftHand / RightHand (Rokoko Newton) while rejecting
    # LeftHandIndex1, which has a hand candidate as an ancestor. ik_* rigs are not real hands.
    cand = {n for n in parent_of if "hand" in n.lower() and not n.lower().startswith("ik_")}
    hands = {n for n in cand if not (cand & set(ancestors(n)))}

    # --- pelvis
    pelvis = next((n for n in parent_of if n.lower() in _PELVIS_NAMES), None)
    if pelvis is None:
        roots = [n for n, p in parent_of.items() if p is None]
        best = None
        for r in roots:
            for c in subtree(r):
                if len(children.get(c, ())) >= 2 and (best is None or len(subtree(c)) > len(subtree(best))):
                    best = c
        pelvis = best

    # --- which side of the body each pelvis child subtree belongs to
    side = {}
    for c in children.get(pelvis, ()):
        st = subtree(c)
        lc = c.lower()
        if st & hands or any(h in lc for h in _UPPER_HINTS):
            side[c] = UPPER
        elif any(h in lc for h in _LOWER_HINTS):
            side[c] = LOWER
        else:
            side[c] = UPPER

    out = {}
    for t in track_names:
        anc = ancestors(t)
        anc_set = set(anc)
        if anc_set & hands:
            out[t] = FINGERS
        elif t in hands:
            out[t] = HANDS
        elif t == pelvis:
            out[t] = PELVIS
        elif pelvis in anc_set:
            # the pelvis child on the path from t up to the pelvis decides upper vs lower
            branch = anc[anc.index(pelvis) - 1] if anc.index(pelvis) > 0 else t
            out[t] = side.get(branch, UPPER)
        else:
            out[t] = ROOT
    return out


def summarise(regions):
    counts = {}
    for r in regions.values():
        counts[r] = counts.get(r, 0) + 1
    return ", ".join("%s=%d" % (r, counts[r]) for r in ORDER if r in counts)
