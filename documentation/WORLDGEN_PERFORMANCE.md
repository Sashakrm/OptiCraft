# World generation performance changes

## Pipeline changes

- A vertical `Chunk_Column_Job` shares immutable X/Z preparation fields across requested Y levels. The expensive per-chunk voxel-fill/packing stage is then fanned out into the same worker pool.
- All six available worker slots (capped at six by the manager) can process generation, mesh, and regional preparation. Meshes keep priority over ordinary chunk/build jobs.
- Cold cave and river cache builds use a separate priority-ordered preparation queue. Cave and hydrology regions are independent tasks and can be built concurrently. Columns waiting for cold caches are held by the scheduler rather than making ordinary chunk workers block on a shared future.
- Cave water marking traverses the bit-packed data in Y/Z/X memory order, preserving the same predicates while improving locality.
- Hydrology priority-flood uses a monotone radix queue plus a bit-packed min-index set for equal-height entries. It retains height/index ordering and the minimum seed key initializes the radix invariant.
- Release builds enable LTO and native CPU instructions by default. Use `-DOPTICRAFT_NATIVE_ARCH=OFF` for a portable build intended for different CPUs.


- Initial world entry and generation-preset reload synchronously warm only the player landing region
  before requesting chunk columns. This moves the one-time cold start out of the first visible column;
  adjacent regions are still prepared asynchronously by the worker scheduler.
- On player chunk changes, the manager starts preparing the current cave/hydrology region before
  it starts enqueueing that frame's new chunk columns. When moving toward a region edge, it also
  schedules the forward region (and the diagonal region for diagonal movement) several chunks
  ahead. The normal scheduler deduplicates these requests and keeps preparation off ordinary
  chunk workers. This lookahead reduces cold-region stalls during continuous traversal; an arbitrary teleport into an
  unprepared region can still wait for regional preparation.
- The hydrology priority-flood loop now uses fixed-order flat neighbour indices rather than an
  eight-direction coordinate loop. The order and all height/water predicates are unchanged.

## Regression microbenchmarks

Synthetic C++ microbenchmarks, compiled with `-O3 -DNDEBUG -march=native`. These isolate the regional generators and are **not** end-to-end timings for the game or for a full nine-chunk column. Times vary with CPU frequency and other system load.

| Test | Before | Optimized | Output hash |
|---|---:|---:|---|
| One river region (256 x 256 output columns; 768 x 768 calculation window), three-run range | 109–127 ms | 72–80 ms | unchanged: `9bed822914b1d6eb` |
| One cave region, build stage, three-run range | 54–63 ms | 37–43 ms | unchanged: `af02f569e3143ff3` |
| Four river regions, observed range | 407–422 ms | 313–363 ms | unchanged: `c5ce68b6614c13c9` |
| Four cave regions, build + sample test | 244–250 ms | 176–180 ms | unchanged: `e33c5f1c560c141f` |
| One cold cave + river region prepared concurrently on two threads | — | about 86 ms total | both caches ready |

The hashes match the pre-optimization generators for the tested terrain function, seeds and region coordinates. This is a regression check, not proof for every possible seed/coordinate.

## Remaining limitation

The regional cache output is shared by 64 horizontal chunk columns, so its cost is amortized over neighbouring columns. Cave and river regions are independent background tasks; normal column workers do not perform a cold build while filling voxels. The spawn/reload region is warmed before the first column request and lookahead starts later tasks before the player reaches the next region during continuous movement. This does not make an arbitrary teleport into an unprepared region finish in 27 ms end-to-end: a cold river region is still tens of milliseconds of work. The complete nine-level column time was not measured end-to-end in this environment, so the 27 ms hard limit is not yet certified.

A full application build could not be completed here because the environment lacks system OpenGL development libraries (and GLM headers). The isolated cave and river C++ modules compile successfully, and the regional output hashes match.


## Verification rerun (2026-10-29, available sandbox)

These are standalone regional microbenchmarks on a virtualized AMD EPYC host with only three
CPU vCPUs exposed. They are not measurements on the target Intel i5-13420H and are not full
nine-chunk-column timings.

| Test | Observed time | Output hash |
|---|---:|---|
| One cave region build, three runs | 33.5–41.0 ms | `af02f569e3143ff3` (sample set unchanged) |
| One river region build + 256x256 lookup/hash pass, three runs | 68.3–78.6 ms | `9bed822914b1d6eb` |
| Four river regions + lookup/hash pass | 257.8 ms | `c5ce68b6614c13c9` |
| Four cave regions build/sample pass | 33.98 ms build + 111.92 ms sampling | `e33c5f1c560c141f` |

The manager/Game translation units could not be compiled in this environment because the system
GLM development headers are absent (`glm/glm.hpp`); only the standalone Cave_Network and
River_Network microbenchmarks were compiled and run. **The 27 ms whole-column maximum is not
certified here**: end-to-end timing needs a complete build and a test on the target machine. The
changes in this revision address when cold regional work is started and preserve checked terrain
hashes; they do not turn an arbitrary cold-region request into a sub-27-ms operation.


## Final optimization pass (2026-10-29)

This pass compares the original archive source tree and the final source tree using the same
standalone harnesses, compiler flags (`g++ -std=c++20 -O3 -DNDEBUG -march=native`), seed,
height callback, and coordinates on the same virtualized host. The machine had only three CPU
vCPUs exposed, so absolute timings are not an Intel i5-13420H guarantee. Every row's output hash
matched the original source tree.

| Test | Original source | Final source | Output hash |
|---|---:|---:|---|
| One river-region query/hash pass | 103.2–108.3 ms | 62.6–68.2 ms | `9bed822914b1d6eb` |
| River test spanning four cache regions | 411.7–486.6 ms | 282.9–297.3 ms | `c5ce68b6614c13c9` |
| Cave single-region build | 50.0–55.9 ms | 32.0–37.8 ms | `af02f569e3143ff3` |
| Cave test spanning four cache regions, build stage | 56.3–57.6 ms | 33.0–35.6 ms | `e33c5f1c560c141f` |
| Same cave test, sample stage | 180.8–184.2 ms | 109.5–125.1 ms | `e33c5f1c560c141f` |

Changes in this pass:

- Cave carving hoists repeated Y-distance calculations, reuses X/Z terms, and skips columns
  outside each ellipsoid's X/Z projection. The equation, iteration order, and bit writes remain
  the same for the tested coordinates.
- Cave water marking iterates the set bits of the network mask in the same increasing linear
  order instead of testing every voxel. Cached neighboring surface heights preserve the original
  six-neighbor short-circuit order and predicates.
- River smoothing uses an unrolled interior path while retaining the same addition order and
  division; flood-discovery metadata doubles as the visited marker, removing the separate byte
  array. The neighbor order remains unchanged.
- The prepared column stores the existing `world_x * 0.11` and `world_z * 0.11` ore-noise inputs
  once per X/Z column, and computes `world_y * 0.11` once per Y layer. Vein-noise expressions
  and all noise formulas remain untouched.

These are standalone regional microbenchmarks, not a full end-to-end measurement of the nine
32x32x32 chunks in one vertical column. The 27 ms whole-column maximum is still not certified:
regional generation itself can exceed 27 ms on a cold cache, and this environment lacks the
system `glm/glm.hpp` development header needed to build the complete game. The isolated Cave and
River translation units compile and pass the hash regression checks; this pass improves their
measured cost without changing the tested outputs.
