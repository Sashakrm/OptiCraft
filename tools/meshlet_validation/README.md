# Meshlet GPU smoke tests (Linux / Mesa / EGL)

Run from the OptiCraft repository root:

```bash
python3 tools/meshlet_validation/check_glsl_link_egl.py
python3 tools/meshlet_validation/test_meshlet_compute.py
```

The scripts create a surfaceless EGL OpenGL 4.3+ context (Mesa llvmpipe works) and verify:

- compile/link of the solid, shaped and liquid Meshlet builder/culler shaders and both terrain shader pairs;
- solid, shaped and liquid packed-record AABB generation;
- fixed-order liquid MDI commands and compact opaque MDI command generation;
- conservative current-frame Hi-Z behavior: detailed opaque geometry can be rejected behind a near depth surface, while coarse LOD and liquid streams remain available.

These are shader/compute smoke tests, not a substitute for building and running the full game on the target Intel UHD driver. They do not measure FPS. The scripts use only Python standard-library `ctypes`; a working Mesa EGL/OpenGL runtime is required.
