# CRT geometry

Open **CRT Geometry** beside **CRT Output**. The controls act on the framed,
rotated BGR picture inside the current active raster. The modeline still
determines the CRT scan timing & the payload dimensions.

| Control | Range | Effect |
|---|---|---|
| Enable geometry | Off / On | Off keeps the saved values & bypasses processing |
| H-Size | −16 to +15 | Point sampling at width multiplier `(64 + step) / 64` |
| H-Position | −48 to +48 px | Positive moves picture content right |
| V-Shift | −16 to +15 lines | Positive moves picture content down |
| Cabinet V-Size | −7 to +7 | Each step adds three virtual lines. Negative shrinks |
| Reset Geometry | Button | Sets the four amounts to zero & retains On/Off |

Interlaced V-Shift & Cabinet V-Size use field-lines. Each spans two positions
in the full frame. Cabinet samples each parity independently, so the sender
still extracts the same odd or even rows for each field. For short rasters,
the panel limits reduction to keep at least one virtual line.

The left edge anchors H-Size. The top anchors Cabinet enlargement, while the
bottom anchors reduction. Content outside the active rectangle is clipped.
The panel shows the requested height & clipping on each side. Active positions
outside the moved picture are black.

Profiles follow the full output timing tuple, including clock, active
dimensions, sync positions, totals & scan type. Two named modes with identical
timing share geometry settings. A new timing starts Off with zero amounts.
Geometry edits take effect while streaming. MiSTerCast rerenders the latest
valid framed image when the desktop supplies no fresh capture.

Cabinet blends adjacent lines in linear light using a pure gamma-2.4 model.
The output remains 8-bit BGR. Half-weight black plus white encodes to 191.
The result models a gamma-2.4 response. Compositor tone mapping, receiver
gamma correction & the CRT's measured response can change the light output.
A stable sampling phase removes frame-to-frame phase drift in the software.
Fine moving patterns can still alias on the fixed raster.

## Reference basis

- The horizontal period in `examples/core_side_snippet.v` supplies the
  `(64+h)/64` ratio. `crt_adjust.sv` & `crt_adjust_sys.sv` supply the
  read-tick, address & gate behavior. MiSTerCast anchors point samples at
  the active raster & uses signed output-pixel offsets. The pinned RTL
  CONTENTSHIFT expression zero-extends negative 9-bit offsets in simulation
  because its conditional has an unsigned zero arm. The host uses the intended
  signed direction. SYNCSHIFT can put its new line reference inside active
  content, while the host clips on the fixed window.
  [MiSTer-CRT-Adjust c682de9](https://github.com/rmonic79/MiSTer-CRT-Adjust/tree/c682de9f4acc61d8f4c7779efb48149d3baa3a8e)
- Cabinet follows `rtl/crt_vsize.sv` registers `a_out_t`, `step_q8`,
  `t_pos`, & `alpha_eff`. It adds three virtual lines per user step, starts
  from source row zero, & floors the step to Q8. The host has a complete
  framebuffer, so the RTL's two-line lead-in, VSync delay & frame slew aren't
  needed. The requested gamma 2.4 replaces that RTL's gamma 2.0 arithmetic.
  [Cabinet RTL](https://github.com/rmonic79/MiSTer-CRT-Adjust/blob/c682de9f4acc61d8f4c7779efb48149d3baa3a8e/rtl/crt_vsize.sv)
- Linux upstream's `PipeWireVideoCapture::processFrame` and
  `FrameProcessor::convertToBgr` supply the capture & BGR conversion baseline.
  The target's `ScalingExecutor::Impl::dispatch` runs the added row job.
  `FrameMailbox` publishes a complete frame. The capture cache retains the
  pre-geometry picture for repeated edits.
  [Linux upstream bcb6988](https://github.com/s3gfaultx/mistercast-linux/tree/bcb698830119e43a601d90e2226ecd1a7c8954ce/src)
- Groovy_MiSTer's `setSwitchres` sets active geometry and timings,
  `process_packet` / `setBlit` select the existing payload and field, and
  `rtl/lz4.v` reconstructs modulo-256 deltas. MiSTerCast changes picture bytes
  before its usual field extraction and encoding. Deltas still use the last
  successfully submitted field of the same parity.
  [Groovy_MiSTer receiver 109908c](https://github.com/psakhis/Groovy_MiSTer/tree/109908c9ddbd4397ba7aadb193de61fdb8cb34e6)
  [Target receiver b0c8bc8](https://github.com/asdgdg355235/Groovy_MiSTer/tree/b0c8bc8a2fb45eed75580fdfb3692d6815d08821)

## Verification and benchmarks

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DMISTERCAST_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/scaler_benchmark 4 300
./build/scaler_benchmark 4 300 720x576i nearest
```

The benchmark reports geometry & total processing p50, p95, p99, and maximum.
It also records process CPU time, plan construction time, fixed buffer sizes &
output checksums. Off & neutral checksums must match. The warmed geometry
test wraps allocator calls from the executable & static core and verifies no
calls across 1,000 repeated frames. The production capture path owns one added
1,244,160-byte BGR buffer, with its coordinate plan cached per revision.

For RTL verification, clone the separate GPL-3.0-or-later MiSTer-CRT-Adjust
reference and check out `c682de9f4acc61d8f4c7779efb48149d3baa3a8e`. Configure
with `-DMISTERCAST_CRT_ADJUST_SOURCE=/path/to/MiSTer-CRT-Adjust`, then run
`ctest --test-dir build -V -R '^crt_geometry_rtl$'`. This uses Icarus Verilog
and Python 3. The harness verifies source hashes & compiles the original RTL
under its upstream license. It compares source positions & fractional weights
against the host. The RGB trace is gamma 2.0, so the gamma-2.4 oracle runs
separately.

On the MiSTer, record the core build and turn receiver geometry & gamma
correction to neutral for comparison. Select a desktop pattern containing a
grid, edge markers, moving fine lines, grayscale ramps & alternating field
colors. Exercise all controls at 240p, 288p, 480i & 576i, including a static
desktop. Check the clipping label, field parity, reset, and updates without a
stream restart. Measure HSync/VSync with a scope and check packet captures for
the absence of new mode commands during adjustments.

Run comparable 10-minute streams with geometry Off and On, a continuous tone
and active controllers. Compare pauses, drops, fallback/unsynced samples, audio
underflows & controller response. Use the existing **Show diagnostics** setting
to get capture age, geometry revision/timing, rerender age, mailbox publication,
transport and audio counters. Use a camera or optical instrumentation for
source-to-CRT latency and brightness. The pattern and tone tools can first
check basic connectivity with `./build/mistercast --pattern MISTER_IP` and
`./build/mistercast --tone MISTER_IP`. The live geometry test itself uses the
GUI desktop stream.

## Measured results

The Release application built with GCC 14.2, Qt 6.8.2, PipeWire 1.4.2, and
LZ4 1.10.0. The CPU was an AMD EPYC 9V74 with nine logical CPUs available to
the process. The benchmark used 1920×1440 BGR input, 12 warm-up frames &
300 timed frames per case. It measures compute & worker scheduling before
mailbox publication, without PipeWire capture or UDP delivery.

Selected four-worker, nearest-scaler results from the full sweep:

| Raster | Geometry | Geometry p95 | Geometry p99 | Total p99 |
|---|---|---:|---:|---:|
| 320×240p at 60 Hz | All four | 0.317 ms | 0.344 ms | 0.668 ms |
| 384×288p at 50 Hz | All four | 0.441 ms | 0.807 ms | 1.288 ms |
| 640×480i at 60 Hz | All four | 1.199 ms | 1.751 ms | 2.418 ms |
| 720×576i at 50 Hz | Off | 0 ms | 0 ms | 0.655 ms |
| 720×576i at 50 Hz | Horizontal | 0.236 ms | 0.385 ms | 0.944 ms |
| 720×576i at 50 Hz | Cabinet LUT | 1.416 ms | 2.607 ms | 3.166 ms |
| 720×576i at 50 Hz | All four | 1.562 ms | 4.117 ms | 4.813 ms |
| 720×576p at 60 Hz, stress fixture | All four | 2.179 ms | 4.012 ms | 5.975 ms |

A separate four-worker PAL sweep measured 1.437 ms geometry p99 & 1.977 ms
total p99 for the nearest-scaler combined case. Across the full sweeps,
18 of 55 four-worker cases and 20 of 55 nine-worker cases exceeded the
geometry p99 target. The total-conversion target was exceeded in 20 and 10
cases respectively, including some geometry-Off runs. Tail times varied more
than medians in this environment. Qualify the targets on the streaming host.

To reduce the scalar kernel's inverse cost, a 3,328-byte lookup table locates
the single possible Q24 threshold crossing for each light bucket. The
threshold comparison is exact for the integer numerator. A scratch AVX2
gather trial failed to show a repeatable 10% advantage across complete geometry
passes, so the application uses scalar arithmetic with persistent row workers.

The 80-digit Decimal oracle and C++ long-double reference agreed on all
16,842,752 channel/weight combinations. The production Q24 LUT reached a
maximum absolute error of one code value, mean absolute error of
`6.29351e-06` codes, and maximum pre-encode linear error of `2.96962e-08`.

The controller IPC test reaches an `AF_UNIX` socket operation barred by the
execution environment. The unchanged branch reproduced that same failure.
The real-gamepad test requires `/dev/uinput`. Physical MiSTer timing, monitor
lock, audio continuity, presentation latency & target-CPU performance require
the hardware session above.

A focused four-worker rerun measured 1.397 ms geometry p99 & 1.945 ms total
p99 for combined geometry at 720×576i, 50 Hz. At the 720×576p, 60 Hz stress
fixture the same settings took 3.132 ms geometry p99 and 7.853 ms total p99.
The focused results meet the PAL geometry target and both total-conversion
targets. The full sweep shows that the geometry target remains variable.

CTest passed 15 targets: `crt_modeline`, `crt_stream`, `pipewire_geometry`,
`crt_geometry_controls`, `crt_geometry`, `gamma24_oracle`,
`gamma24_exhaustive`, `crt_geometry_rtl`, `virtual_gamepad`,
`scaling_executor`, `scaler`, `frame_processor`, `groovy_mister`,
`concurrency`, & `portal_response`. It reported one environment-blocked
failure (`controller_input`) and one skip (`real_gamepad`).

The RTL test exercised 112 horizontal cases across both positioning modes
and 14 Cabinet cases. It matched 3,176 Cabinet active-row indices & weights
against the host. The tight-blanking fixture also observed the upstream
14-line VSync delay. The GUI, capture & renderer tests passed under
AddressSanitizer and UBSan. The capture test passed under ThreadSanitizer after
moving its callback generation into an atomic slot. The PipeWire queue now
carries only a wakeup, with no separately copied callback payload.
