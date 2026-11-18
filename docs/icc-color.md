# ICC color correction

Choose a display-class RGB ICC profile for the CRT, enable correction in
**Connection**, and check the profile status below the controls. MiSTerCast
builds a LittleCMS 2 transform in the background when the path changes and
reuses it when either checkbox changes. A failed path leaves correction off.
Changes rerender the latest eligible capture with the current settings
revision and mode epoch, so the desktop need not produce a new frame.

The source is LittleCMS's built-in sRGB profile, an assumption for SDR
desktop capture. PipeWire's negotiated raw video format can report range and
transfer metadata, which MiSTerCast logs. The app currently doesn't use this
metadata to pick a source ICC profile. Captures of wide-gamut or HDR content
may need a different source treatment.

LittleCMS produces packed BGR24 using relative colorimetric intent, including
the profile's PCS-to-device LUT where available. The transform runs after
framing and CRT geometry on the full raster. Cabinet V-Size's gamma-2.4
interpolation operates on capture values and doesn't use the selected
profile's transfer curves. Field extraction reads the finished BGR raster.

Embedded `vcgt` is a separate video LUT stage. Argyll's `dispread -k` loads
the calibration while measuring the display, then records the curves for
`colprof`. MiSTerCast can apply those curves to the converted device values
with **Apply embedded calibration (vcgt)**. The switch defaults to off.
MiSTerCast disables this second stage for profiles marked as already
calibrated by Argyll's `applycal`. Leave it off if equivalent calibration
is already active downstream or has been baked into the profile by other
means. [Argyll dispread](https://www.argyllcms.com/doc/dispread.html),
[dispwin](https://www.argyllcms.com/doc/dispwin.html),
[applycal](https://www.argyllcms.com/doc/applycal.html)

## Reference basis

- `s3gfaultx/mistercast-linux` at `bcb6988`,
  `src/capture/PipeWireVideoCapture.cpp`, `processFrame()`. PipeWire converts
  to packed BGR and publishes to the mailbox. The target's existing
  `publishConverted()` inserts CRT geometry here and now applies ICC before
  that same publication.
- `psakhis/Groovy_MiSTer` at `109908c`, `Groovy.sv`, `S_Switchres_Mode`
  and the blit states. The receiver handles output timing and blits separately.
  MiSTerCast supplies corrected bytes through its existing
  `StreamSession` field extractor and encoder.
- LittleCMS 2.16, `cmsCreateTransformTHR`, `cmsDoTransform`, and
  `cmsReadTag`. `cmsFLAGS_NOCACHE` disables the mutable one-pixel cache so
  the frame executor can share a compiled transform across row ranges.
  [LittleCMS engine API](https://www.littlecms.com/LittleCMS2.16%20API.pdf)
- ArgyllCMS 3.3.0, `profile/applycal.c`, `main()` calibration check.
  The `AR0T` private tag signals that calibration has been incorporated
  into the profile. MiSTerCast uses LittleCMS to check for that tag.

## Local verification

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DMISTERCAST_BUILD_BENCHMARKS=ON \
  -DMISTERCAST_ICC_TEST_PROFILE="/absolute/path/to/display.icc"
cmake --build build
ctest --test-dir build --output-on-failure
scratch="$(mktemp -d)"
./build/icc_benchmark "/absolute/path/to/display.icc" "$scratch/matrix.icm" 300
```

The optional external-profile test exercises a hybrid DisplayCAL / Argyll
profile with a relative-colorimetric B2A LUT. The supplied v2.2 ICC file
reported perceptual intent in its header and contained a B2A1 table. The
transform requests relative colorimetric intent explicitly. The benchmark expects a LUT
profile with separate `vcgt` and writes a synthetic matrix/TRC profile
to the second path.

## Performance

Release build with LittleCMS 2.16 on an AMD EPYC 9V74 host, nine existing
scaling workers and 300 measured frames per case. Timing starts with
same-size RGB capture conversion and ends after geometry and ICC. The
benchmark excludes mailbox publication, compression, and UDP. It uses
a synthetic matrix/TRC profile and the supplied DisplayCAL / Argyll
XYZ LUT + matrix profile. Values are in milliseconds.

| Raster | Stages | Median | p95 | p99 | Max |
| --- | --- | ---: | ---: | ---: | ---: |
| 320×240p | off | 0.14 | 0.20 | 0.42 | 1.12 |
| 320×240p | cabinet | 0.41 | 0.50 | 0.62 | 0.84 |
| 320×240p | matrix | 0.39 | 0.53 | 0.66 | 1.93 |
| 320×240p | lut | 0.85 | 1.06 | 1.48 | 2.37 |
| 320×240p | lut-vcgt | 0.81 | 1.26 | 3.17 | 6.69 |
| 320×240p | lut-vcgt-cabinet | 1.09 | 1.42 | 2.01 | 3.04 |
| 384×288p | off | 0.13 | 0.20 | 0.36 | 0.52 |
| 384×288p | cabinet | 0.49 | 0.57 | 0.76 | 1.54 |
| 384×288p | matrix | 0.62 | 0.73 | 1.15 | 1.91 |
| 384×288p | lut | 0.71 | 1.13 | 1.20 | 1.23 |
| 384×288p | lut-vcgt | 0.87 | 1.24 | 2.10 | 3.79 |
| 384×288p | lut-vcgt-cabinet | 0.99 | 1.50 | 1.69 | 6.70 |
| 640×480i | off | 0.21 | 0.28 | 0.56 | 1.18 |
| 640×480i | cabinet | 0.72 | 1.11 | 1.14 | 1.16 |
| 640×480i | matrix | 0.86 | 1.00 | 1.47 | 5.17 |
| 640×480i | lut | 1.69 | 1.86 | 3.41 | 5.68 |
| 640×480i | lut-vcgt | 1.74 | 2.33 | 4.58 | 6.84 |
| 640×480i | lut-vcgt-cabinet | 2.24 | 4.85 | 6.50 | 7.65 |
| 720×576i | off | 0.22 | 0.43 | 0.88 | 2.32 |
| 720×576i | cabinet | 0.87 | 1.56 | 3.51 | 5.00 |
| 720×576i | matrix | 1.04 | 1.20 | 1.50 | 2.01 |
| 720×576i | lut | 2.17 | 2.80 | 4.28 | 24.71 |
| 720×576i | lut-vcgt | 2.24 | 2.41 | 3.79 | 11.81 |
| 720×576i | lut-vcgt-cabinet | 2.88 | 3.09 | 3.83 | 9.11 |

With a single transform caller, the supplied LUT at 720×576i took 12.09 ms
at the median before calibration or Cabinet. The existing workers reduced
that stage's cost without changing output bytes. The full 576i Cabinet + LUT + vcgt path reached 3.83 ms p99 in the
standalone run. A separate run alongside builds and tests measured
11.63 ms p99 for that case, above half of a 50 Hz field period.

The enabled path allocates one reusable 1,244,160-byte destination buffer
on its first frame. A separate allocation-counting run observed zero heap
allocations across 40 warmed frames per case. The bypass path calls neither
LittleCMS nor the executor's ICC phase and adds no pixel copy or
frame buffer. It can keep an already allocated buffer for later use.

## CRT verification

For a CRT check, stream an SDR desktop showing RGB primaries, a neutral
ramp with near-black steps, and smooth gradients. Compare correction off,
ICC alone, and ICC with embedded calibration. Keep the CRT's brightness,
contrast, and downstream calibration fixed. Measure the patches with the
same instrument and operating conditions used to create the profile.
Repeat in 480i or 576i with **Show diagnostics** enabled and look at
`icc_us_avg`, `icc_us_max`, and the FPGA fallback counts. A software
reference comparison only verifies the transmitted code values.
