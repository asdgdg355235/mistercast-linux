# MiSTerCast Linux

Low-latency Wayland desktop and PipeWire audio streaming to the Groovy_MiSTer core.

The GUI supports automatic CRT timing generation and editable NTSC and PAL presets from 256×240p through 720×576i.

## Build

Arch Linux dependencies:

```sh
sudo pacman -S --needed cmake ninja qt6-base pipewire wireplumber lz4 pkgconf \
  xdg-desktop-portal xdg-desktop-portal-kde
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt Multimedia is deliberately not used. Source selection goes through the XDG ScreenCast portal, after which the application consumes the selected PipeWire node directly.

For a user-local SteamOS/Arch installation:

```sh
cmake --install build --prefix "$HOME/.local"
```

Ensure `$HOME/.local/bin` is in `PATH`. The desktop entry and scalable icon are installed under `$HOME/.local/share`.

A local Arch package can also be built from the checkout:

```sh
makepkg -D packaging -si
```

## Hardware Pattern Test

Start the Groovy_MiSTer core, connect the MiSTer over Gigabit Ethernet, then run:

```sh
./build/mistercast --pattern 192.168.1.50
```

The test uses the default 320x240p60 modeline and displays color bars with a square that changes every frame. Stop it with `Ctrl+C`.

Use `./build/mistercast --tone 192.168.1.50` to include a 440 Hz test tone. Network audio must be enabled in the core OSD.

## GUI

Run `./build/mistercast`, choose **Select Desktop Source**, and approve a monitor or window in the compositor portal. Enter the MiSTer address and start streaming. With diagnostics enabled, the GUI reports the median, p95, and maximum age since PipeWire dequeue for newly captured frames, plus the number of output frames that reused an unchanged capture. Capture processing, preflight-ready wait, compression, UDP-capacity wait, and socket-submission time are also reported. Frame age and the component timings are complementary measurements and should not be added together.

### CRT output

**Preloaded / manual modeline** is the default and keeps the saved preset selection. On first launch, MiSTerCast copies the ten bundled modes to `~/.config/MiSTerCast/MiSTerCast/modelines.dat`. Use **Edit File** and **Reload** to update the catalog. Invalid lines appear in the status log.

**Automatic CRT generation** uses Switchres to calculate timings for `ntsc`, `pal`, `generic_15`, `arcade_15`, or a custom CRT range. Set the output dimensions, frame or field rate, scan type, pixel-clock range, and refresh tolerance. **Auto** dimensions use a compatible source size when available. An HD source defaults to 640×480i for NTSC or 720×576i for PAL and uses the selected frame scaler. **Profile default** uses 59.94 Hz for NTSC, generic, and arcade profiles, or 50 Hz for PAL. Custom profiles use the midpoint of their configured cadence range. The timing preview shows the result before MiSTerCast sends the mode.

The NTSC profile allows 15.700–15.760 kHz and 59.7–60.3 Hz. The PAL profile allows 15.575–15.675 kHz and 49.7–50.3 Hz. These are MiSTerCast acceptance ranges around the nominal Switchres TV profiles for integer-line progressive modes and fractional refresh. The two other 15 kHz presets use the upstream ranges. Custom ranges accept horizontal frequency in Hz, frame or field cadence in Hz, horizontal porches in µs, and vertical porches in ms. The calculator uses porch durations as nominal timing values. It accepts horizontal widths down to Switchres's 90% threshold and allows one timing count for vertical rounding. Combined nominal porch and sync duration must stay below 75% of the shortest line or field period in the configured range.

Automatic mode accepts 2.5–25 MHz, keeps the output within 720×576, and checks each porch and sync width against the FPGA's 8-bit timing registers. Interlaced output uses an even active height, an odd full-frame total, and a half-line field offset. The preview reports fields per second for interlaced modes. A requested 59.94 Hz interlaced cadence corresponds to 29.97 complete frames per second. MiSTerCast sends these timings to Groovy_MiSTer without switching the host display.

If generation fails, MiSTerCast tries the selected fallback, the closest compatible catalog entry, then the built-in 320×240p preset. A fallback must pass the same CRT, clock, and refresh checks, match the chosen active height and scan policy, and match any explicit width. The status log identifies rejected candidates and the selected fallback. If no candidate passes, MiSTerCast stops the current stream or blocks startup. When output timing changes, MiSTerCast joins the old sender before changing the capture epoch and opening a fresh Groovy_MiSTer session. The new session starts with fresh pacing, field, and compression state.

The source is center-cropped to 4:3 by default. The **Framing** group provides alignment, offsets, rotation, & a **Scaling** selector. **Nearest** remains the default & preserves hard pixel edges. **Bilinear** interpolates between neighboring pixel centers. **Area** averages pixel coverage when shrinking & uses bilinear interpolation on enlarging axes. **LANCIR (Lanczos-3)** uses Aleksey Vaneev's [AVIR project](https://github.com/avaneev/avir) implementation with its default Lanczos-3 filter. Framing changes apply to subsequent captured frames without restarting capture or streaming. The selected scaler is saved between launches & also applies to the temporary frame shown during output-mode changes. The streaming path retains only the newest frame; it never queues stale video to improve apparent smoothness.

Image scaling uses a persistent multicore worker pool for all four algorithms. Its worker count comes from `std::thread::hardware_concurrency()`, with one worker when the reported count is zero. Each frame is divided into disjoint row ranges & completed before publication. The same pool handles output-mode resizing.

AVIR image resizing algorithm designed by Aleksey Vaneev. LANCIR 3.1.1 is vendored from AVIR commit `f29c2e2783d6aeb276c413f45784660f00c13be5` under the MIT license. The original header & license are in `third_party/avir/`; the AVIR license is also installed with MiSTerCast documentation.

Network audio must also be enabled in the Groovy_MiSTer core OSD. The GUI warns if MiSTer acknowledgements report audio disabled.

Enable **Show diagnostics** to display detailed latency, audio, transport, and FPGA metrics and print their periodic values to the terminal. The setting is disabled by default; connection messages, warnings, and errors remain visible. Audio capture starts with a 256-frame (5.33 ms) jitter reserve, lowers it after sustained full reads, and raises it immediately after an underflow, within a 128-512 frame range. The reserve remains available to absorb field-timing corrections rather than being held back from packet reads. The latency watermark also allows the largest observed PipeWire callback batch so burst delivery is not discarded before it can be consumed. `underflow` means the PipeWire ring did not contain enough samples and the remainder was zero-filled. `stale/overflow` reports captured samples discarded to bound latency or overwritten because the ring filled. `UDP peak` and `pacing overruns` show transport pressure. These counters are cumulative for the current stream.

The FPGA delivery line reports status samples where MiSTer's fallback framebuffer was active, its VRAM stream was unsynchronized, or its pixel queue was empty. Video transmission reuses the latest capture without waiting at each field boundary and keeps every frame in the negotiated LZ4 format. Delivery starts with a half-field reserve and slowly reduces it to no less than three-eighths of a field after sustained healthy FPGA feedback; any fallback, unsynchronized, or queue-empty report immediately restores the half-field reserve.

## Runtime Requirements

- Wayland compositor with XDG ScreenCast portal support
- PipeWire and a running session manager such as WirePlumber
- Wired Gigabit Ethernet required; both ends and every intervening switch port must negotiate 1000 Mb/s full duplex
- Groovy_MiSTer core listening on UDP port 32100

The currently active desktop compositor determines how early a rendered frame becomes available to PipeWire. Application metrics therefore measure from PipeWire delivery onward; use a high-speed camera showing both source and CRT for capture-to-photon measurements.

## Troubleshooting

- If MiSTer does not acknowledge the stream, verify its address, the Groovy_MiSTer core, UDP port 32100, firewall rules, and VLAN/routing configuration.
- If the source chooser does not open, verify that `xdg-desktop-portal` and the compositor-specific backend are running.
- If output audio is missing, check the default PipeWire sink with `wpctl status`, verify that a session manager such as WirePlumber is running, and enable network audio in the core OSD.
- If diagnostics show UDP queue growth, repeated drops, pacing overruns, fallback, unsynced, or queue-empty samples, verify both Ethernet ports with `ethtool`. A damaged cable or 100 Mb/s switch port cannot sustain high-resolution video payloads.

## Scaling benchmark

An optional benchmark measures completed 1920x1440 to 720x576 BGR resize operations, including worker dispatch & completion. It compares one worker with the specified count after warm-up. It is excluded from normal builds & CTest.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMISTERCAST_BUILD_BENCHMARKS=ON
cmake --build build --target scaler_benchmark
./build/scaler_benchmark 8 30
```

The arguments are worker count & measured iterations. Defaults use hardware concurrency & 30 iterations. Results depend on the host, available CPU time, & source/output dimensions.

## Optional mpv controller controls

MiSTer controllers can control an external mpv player's JSON IPC socket while
streaming. This is disabled by default. See [setup, bindings, protocol references
& tests](docs/mpv-controller.md).

## System-wide MiSTer controllers

See [Linux virtual gamepads](docs/linux-gamepads.md) for uinput setup, controls,
permissions, diagnostics & host verification. The feature works independently
of mpv while streaming.

## Licenses

The MiSTerCast build that links Switchres is distributed under GPL-3.0-or-later. The original MiSTerCast files retain the Unlicense, Switchres retains GPL-2.0-or-later, and AVIR retains MIT. See the [Switchres source and license notes](third_party/switchres/README.md) for revision details and binary distribution requirements.
