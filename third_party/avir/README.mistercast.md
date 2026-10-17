# AVIR / LANCIR integration

Upstream: https://github.com/avaneev/avir

Revision: `f29c2e2783d6aeb276c413f45784660f00c13be5`

`lancir.h` identifies itself as LANCIR 3.1.1. The header and MIT
`LICENSE` are verbatim upstream files. MiSTerCast's own license is unchanged.
The AVIR license is installed under MiSTerCast's documentation directory.

Each persistent scaling worker owns one independent `CLancIR` through the
private `src/core/LancIRRuntime.h` adapter. Default Lanczos radius is 3.0.
The executor assigns contiguous pre-rotation destination strips, retaining
the full source crop and full output width for every worker.

For a strip starting at `firstRow`, the vertical parameters are:

```cpp
fullKy = double(sourceHeight) / fullOutputHeight;
fullOy = (fullKy - 1.0) * 0.5;
params.ky = -fullKy;
params.oy = fullOy + fullKy * firstRow;
```

Floating-point addition is not associative. Evaluating each local position
as `params.oy + fullKy * i` can cross upstream's 1/1000 filter-phase boundary
when the full-image position `fullOy + fullKy * (firstRow + i)` does not.
Source height 3 resized to height 40 exercises this difference.

The adapter uses the pinned header's protected position-cache API to restore
global-row evaluation order, including padding and source offsets. The
unmodified upstream `resizeImage()` then reuses this table. Filter generation,
pixel arithmetic and SIMD remain upstream's implementation. Changes to the
upstream revision require reviewing this protected API dependency and running
the full-call reference and strip-equivalence tests.

Positive source stride is passed directly, including physical row padding.
Negative stride requires a reusable packed copy of only the selected crop.
Output scratch is bounded by 720 * 576 * 4 bytes. Rotation and channel
conversion operate after resizing, with the same pool partitioning final
destination rows. No frame queue or per-frame worker creation is used.
