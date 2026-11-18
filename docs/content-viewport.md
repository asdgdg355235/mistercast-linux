# Content viewport

Choose the actual output timing in **CRT Output**. In **Framing**, turn on
**Custom content dimensions** and enter the picture width in output pixels
and its height in full-frame lines. **Center content** uses the floor of
half the remaining space on each axis. Turn it off to enter a signed
origin from the left and top edges of the active raster. The picture is
clipped where it extends beyond that raster.

| Active raster | Picture dimensions | Centered origin |
| --- | --- | --- |
| 664 × 498i | 640 × 480 | X=12, Y=9 |
| 656 × 492i | 640 × 480 | X=8, Y=6 |
| 640 × 480i | 640 × 480 | X=0, Y=0 |

Source alignment and offsets still select the 4:3 crop, accounting for
rotation. A native-sized BGR crop is copied into position at its existing
resolution. A size change uses the selected scaler. Viewport settings are
saved by the output timing tuple, including the scan mode and sync timing.
**Reset viewport** restores full-raster framing.

The viewport is composed on black before CRT Geometry and ICC. Geometry
operates on the full raster and can move, crop, or interpolate the picture
edges and padding. ICC then transforms the resulting raster, including
the black padding. A viewport edit preserves the modeline, field sequence,
and stream epoch.

Desktop capture supplies progressive pictures. Interlaced output extracts
its existing odd and even raster rows after placement, CRT Geometry, and
ICC. For a viewport at Y=9, source row 0 is on output row 9 and therefore
belongs to the odd-row output field. A one-line move exchanges the source
row parity sampled by the two fields. Their transmitted order is unchanged.
A capture node reporting interlaced input is rejected because the desktop
capture path has no field-timed processing or deinterlacing.

The capture owns one packed copy of the latest source while that source is
active. Its storage grows as needed, up to the negotiated 8192 × 8192
capture bounds for four-byte pixels, and is released on capture stop.
This retains pixels outside the current crop so a live edit can rebuild
the raster from source. If allocation fails, subsequent frames still use
the viewport and a static rerender waits for a fresh source. The viewport
scratch buffer reuses the storage previously reserved to resize a converted
mode preview. A mode preview now recomposes from cached source pixels.

## Reference basis

- The Linux upstream capture path at `s3gfaultx/mistercast-linux`
  `bcb6988`, `src/capture/PipeWireVideoCapture.cpp` `processFrame()`,
  converts and publishes an active raster. In the target, viewport
  composition runs before `publishConverted()` applies geometry and ICC.
- The receiver at `psakhis/Groovy_MiSTer` `109908c`, `Groovy.sv`
  `S_Switchres_Mode` and the blit states, handles timing independently
  from frame delivery. The sender still calls `extractInterlacedField()`
  before passing each field to the existing encoder.

## Benchmark

Build with `-DMISTERCAST_BUILD_BENCHMARKS=ON` and run
`./build/viewport_benchmark 1000`. The benchmark uses 640×480 BGR input,
640×480 native and 630×472 scaled viewport cases, and the 664×498i output
raster. The cached rows include a packed source copy. The benchmark times
framing and composition and excludes CRT Geometry, ICC, mailbox, and UDP.

Release build on an AMD EPYC 9V74, nine existing scaling workers,
1,000 measured frames per case. Times are in microseconds.

| Path | Median | p95 | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Original full-raster scaling | 329.64 | 574.78 | 765.73 | 4039.23 |
| Full-raster scaling with source copy | 357.39 | 603.76 | 1248.25 | 3373.36 |
| Native viewport with source copy | 50.03 | 76.71 | 133.41 | 311.17 |
| Scaled viewport with source copy | 379.41 | 618.59 | 751.92 | 4400.79 |

The extra packed source copy is 921,600 bytes for this BGR input. The
capture test verifies the buffer address stays stable as the viewport
changes and more frames arrive. The executable and static core allocator
probe recorded zero allocations across 1,000 warmed native and scaled
viewport compositions.

## Hardware check

Stream a progressive 640×480 capture into the 664×498i mode. Set a
640×480 centered viewport and check the border and one-pixel grid. Move
the origin up and down by one full-frame line, including through Y=0.
Watch for field or sync interruptions while audio and input stay active.
Repeat in the 656×492i mode and check that its viewport settings restore
separately.
