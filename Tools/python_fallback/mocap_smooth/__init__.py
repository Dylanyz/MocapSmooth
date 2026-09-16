"""Mocap smoothing.

The face of this is the Animation Modifier Blueprint `/Game/MocapSmooth/AM_MocapSmooth`:
right-click AnimSequences -> Animation Modifier(s) -> Add -> AM_MocapSmooth -> Apply.
Its sliders call `actions.smooth_by_path`, which is where all the work happens.

From the console instead:

    import mocap_smooth as ms
    ms.smooth_selected()            # Rokoko's _body group at slider 5 (5.5 Hz)
    ms.revert_selected()            # back to the untouched original

Every run reads from a pristine copy of the raw animation kept in `Saved/MocapSmooth/`, so
re-running with a different strength lands on that strength exactly instead of stacking.
`trackcache.py` explains how that copy is protected; pass `on_top=True` when you actually do
want to stack another pass.

`modifier.py` is NOT imported here. It is the old Python `UAnimationModifier`, kept only as the
design reference for the Blueprint's property list -- registering it would put a second, broken
entry in the Add Modifier menu, and any asset it is attached to can never be saved.

Knowledge base, math and the canonical smooth_core.py live at the root of the MocapSmooth repo
(https://github.com/Dylanyz/MocapSmooth, .claude/refs and Tools/). smooth_core.py here is a vendored copy; re-copy it rather
than patching it if the canonical version changes.
"""

from .actions import (  # noqa: F401
    smooth_selected, revert_selected, smooth_anim, revert_anim,
    smooth_by_path, revert_by_path, smooth_from_asset, revert_from_asset, forget_original,
    strip_modifier_user_data, strip_modifier_user_data_selected,
    OFF, INHERIT,
)
from . import regions, trackcache, smooth_core, actions  # noqa: F401
