#!/usr/bin/env python3
"""Print upstream smooth_root.smooth_signal on the path tests/postprocess_test.cpp
rebuilds, for the reference values embedded in that test.

The smoother is NumPy/SciPy only, so upstream's torch-importing module is
loaded with torch stubbed out:

    python reference/dump_smooth_root_reference.py /path/to/nv-tlabs/kimodo
"""
import importlib.util
import math
import sys
import types

import numpy as np

FRAMES = 97
SAMPLES = [0, 1, 2, 7, 15, 31, 48, 63, 64, 80, 94, 95, 96]


def path():
    # A walk with sway and noise-like wobble, rounded to float32 as upstream
    # receives its torch float32 hips.
    t = np.arange(FRAMES, dtype=np.float64)
    x = 0.045 * t + 0.03 * np.sin(1.3 * t) + 0.01 * np.cos(4.7 * t)
    z = 0.4 * np.sin(0.05 * t) + 0.02 * np.cos(2.1 * t)
    return np.stack([x, z], axis=1).astype(np.float32)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    sys.modules["torch"] = types.ModuleType("torch")
    tools = types.ModuleType("kimodo.tools")
    tools.ensure_batched = lambda **_: (lambda f: f)
    sys.modules["kimodo"] = types.ModuleType("kimodo")
    sys.modules["kimodo.tools"] = tools
    spec = importlib.util.spec_from_file_location("smooth_root", f"{root}/kimodo/motion_rep/smooth_root.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    x = path()
    smoothed = module.smooth_signal(x, np.full(FRAMES, 0.06))  # get_smooth_root_pos
    for frame in SAMPLES:
        print(f"    {{{frame}, {smoothed[frame, 0]:.9g}F, {smoothed[frame, 1]:.9g}F}},")
    print(f"max |smoothed - input| = {np.abs(smoothed - x).max():.6f}", file=sys.stderr)


if __name__ == "__main__":
    main()
