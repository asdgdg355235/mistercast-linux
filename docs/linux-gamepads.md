# Linux virtual gamepads

Enable **Forward MiSTer controllers as Linux gamepads** in the Connection panel.
This Qt option is saved as `systemGamepads` in QSettings. It applies to the next
stream & disables the direct mpv mapper. No mpv process or IPC socket is needed.
The application creates native uinput devices; consuming applications receive
standard evdev events. Wine/Proton compatibility depends on the consuming
application's Linux controller backend & mappings.

## Arch/CachyOS setup

Run these commands from the repository. The group grants access to uinput device
creation, which permits input injection; add only the intended desktop user.
It doesn't grant access to other physical input devices.

```sh
sudo pacman -S --needed base-devel cmake qt6-base pipewire lz4
sudo groupadd -f mistercast-input
sudo usermod -aG mistercast-input "$USER"
sudo install -m 0644 packaging/70-mistercast-input.rules /etc/udev/rules.d/70-mistercast-input.rules
sudo modprobe uinput
sudo modprobe joydev
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput
```

Log out & back in for group membership. To load modules at boot:

```sh
printf 'uinput\njoydev\n' | sudo tee /etc/modules-load.d/mistercast-input.conf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/mistercast
```

Enable **Joysticks: Digital** or **Analog** in Groovy_MiSTer before starting.
Map the physical controls in MiSTer. Select a desktop source, enable the Linux
gamepad checkbox, then start streaming. Start the consuming game after the pads
appear if it doesn't support hotplug. Applications must have access to the
event nodes; sandboxed applications need their own device access configuration.

## Devices & controls

Each accepted snapshot describes two slots. Both `MiSTerCast Gamepad 1` &
`MiSTerCast Gamepad 2` are created on the first accepted snapshot. The protocol
has no presence flag, so an unused slot also appears as a neutral device.
Identification uses `BUS_VIRTUAL`, vendor/product `0000:0000`, version `0001` &
a per-session endpoint/slot physical path. No commercial controller is copied.
Devices persist through ordinary unchanged input. A mode change recreates them
with the corresponding capabilities. No rumble or force feedback is advertised.

| Wire control | Linux control |
| --- | --- |
| Right/Left, Down/Up | `ABS_HAT0X/Y`, positive right/down |
| B1/B2/B3/B4 | `BTN_SOUTH/EAST/WEST/NORTH` |
| B5/B6 | `BTN_SELECT/START` |
| B7/B8 | `BTN_TL/TR` |
| B9/B10 | `BTN_TL2/TR2`, digital triggers |
| Spare digital bits 14/15 | `BTN_TRIGGER_HAPPY1/2` |
| Analog LX/LY/RX/RY | `ABS_X/Y/RX/RY`, signed 8-bit, center 0 |

Analog mode advertises -128..127 with no rescaling, fuzz or extra dead zone.
MiSTer's `joy_analog` applies its configured dead zone before transmission.
Digital mode advertises X/Y -1..1 mirroring the hat to satisfy gamepad detection;
it doesn't advertise right-stick axes. Opposing directions resolve to neutral.
There are no transmitted analog trigger, menu/guide or stick-click fields.
The spare digital bits are preserved without assigning undocumented meanings.
SDL's generic joystick interface exposes the advertised controls. SDL game
controller mappings, Steam layouts & RetroArch bindings can be configured for
this named device; compatibility with each application needs host testing.

The supplied rule provides discoverable symlinks. Never assume `js0`:

```sh
lsmod | grep -E 'uinput|joydev'
ls -l /dev/uinput /dev/input/by-id/mistercast-gamepad-*
cat /proc/bus/input/devices
evtest /dev/input/by-id/mistercast-gamepad-1-event-joystick
jstest /dev/input/by-id/mistercast-gamepad-1-joystick
./build/real_gamepad_test
```

`real_gamepad_test` creates a temporary pad, discovers its event node through
sysfs, verifies evdev identity/capabilities/press/release/axis events & checks
SDL2 enumeration, button transitions & axes when that runtime is installed. It skips with exit 77 if
uinput is absent or inaccessible. The ordinary test suite also runs a syscall
harness with synthetic packets; that harness doesn't prove kernel behavior.

## Reception & recovery

The receiver resolves the configured host, discovers the routed local interface,
binds an ephemeral UDP port on that interface & sends a single registration byte
to the MiSTer's input port 32101. Input is received on that registered ephemeral
port. Each sender IP/port is checked before decoding. A persistent regular file
in `/tmp/mistercast-input-<numeric-address>-<port>.lock`, opened without following
symlinks & locked with `flock`, prevents competing MiSTerCast receivers. The lock
is released on close/process exit; its file must remain to avoid inode races.
Other senders such as RetroArch don't honor this lock & must not register against
the same Groovy session simultaneously.

Registration occurs after desktop-frame preparation, immediately before the
stream's `CMD_INIT`. Groovy's non-XDP `setInit` consumes one queued registration;
it doesn't acknowledge registration or poll for replacements during streaming.
Repeated registrations would queue stale client endpoints for future sessions,
so the receiver doesn't periodically resend them. Lost registration or an old
registration already queued at MiSTer requires restarting the stream; if the
queue remains stale, restart Groovy_MiSTer. This source-verified mechanism can
explain no-traffic symptoms, but it isn't proof of the historical failure.

Packets are full snapshots of both slots: 9 bytes digital or 17 analog. Frame
serial arithmetic rejects older/duplicate snapshots & accepts 32-bit rollover;
order must increase within a frame, matching the reference's byte comparison.
A subsequent full snapshot repairs lost state. Same-frame order wrap & sender
counter resets have no explicit epoch; reset recovery follows the watchdog or a
new session. Unchanged holds generate no duplicate Linux events.

Groovy sends idle snapshots after 2700 blits. The inactivity timeout is
`max(60 s, 2700 / modeline.cadenceHz() + 15 s)`: 60 s at 60 Hz, 69 s at 50 Hz,
105 s at 30 Hz. These values are policy derived from the receiver's reporting
interval. Long stream stalls can exceed the nominal cadence allowance. The
watchdog releases controls & destroys both pads, then accepts a new sequence &
recreates the devices on fresh input. A temporary interruption shorter than the
watchdog preserves a held button, since silence doesn't mean release.

Explicit stop, streaming error & process shutdown close both devices, emitting
neutral state first. Write failures destroy the affected device rather than
leaving stale input. Device creation retries at five-second intervals while a
valid snapshot remains live. Physical-controller removal has no dedicated wire
notification; its effect depends on MiSTer sending an updated snapshot. A lost
release can remain held until the next snapshot or timeout.

Input runs in a bounded worker outside video/audio & the Qt event loop. A
forwarding-only connection isn't provided: Groovy input registration requires
stream initialization & idle keepalives require blits. Sending unverified dummy
blits or competing with another streamer would change those semantics.

## Diagnostics

Registration logs include the routed local IP/port, configured peer & watchdog.
The worker logs first valid input, first malformed/unexpected packet, device
creation, sysfs identity, state resets & throttled device errors. A periodic
summary distinguishes raw, valid, stale, malformed, unexpected & PS/2 packets.

| Evidence | Check |
| --- | --- |
| No raw UDP traffic | Route/firewall, registered host port, Groovy `[Inputs]` endpoint, stale queued registration |
| Unexpected endpoint counter | Sender IP/port or another process sending to the host port |
| Malformed packets | Capture lengths & compare with the documented snapshot format |
| Valid input, device creation error | `/dev/uinput`, module, group & permissions |
| Device created, app cannot open it | Event-node ACLs, desktop session, sandbox access |
| App detects pad, controls differ | Application mapping against the table above |

Capture the host interface with `tcpdump -ni INTERFACE udp port 32101`; the
receiver's local ephemeral destination port should match Groovy's `[Inputs]`
log. Registration receipt is unconfirmed until valid input arrives. No single
warning treats all reception failures as a disabled Joysticks setting.

## Reference basis

Target base: `asdgdg355235/mistercast-linux` `bdc3e12a9bff64c98124392b0f7471a0350761de`.
`main` contains the relevant controller implementation & commits newer than
`feature/mpv-controller` (`0b72182ba4525f4088217628daeb0730309d65f5`). The common
Linux-upstream ancestor remains `bcb698830119e43a601d90e2226ecd1a7c8954ce`.

* `psakhis/Groovy_MiSTer`, `109908c9ddbd4397ba7aadb193de61fdb8cb34e6`:
  `hps_linux/src/support/groovy/groovy.cpp`, `groovy_send_joysticks`,
  `groovy_send_joystick`, `groovy_send_analog`, `setInit`, `setBlit`,
  `KEEP_ALIVE_FRAMES`: wire layout, slots, counters, registration & keepalive.
* Same repository: `api/groovymister.cpp`, `BindInputs`, `PollInputs`,
  `setFpgaJoystick`; `api/groovymister.h`, `GM_JOY_*`: registration byte,
  accepted lengths, byte ordering & digital masks.
* Same repository: `retroarch/src/input/drivers_joypad/mister_joypad.c`,
  `mister_pad_get_button`; `hps_linux/src/input.cpp`, `joy_analog`, &
  `hps_linux/src/user_io.cpp`, `user_io_l_analog_joystick` /
  `user_io_r_analog_joystick`: control labels, signed axes & upstream dead zones.
* `s3gfaultx/mistercast-linux`, `bcb698830119e43a601d90e2226ecd1a7c8954ce`:
  `src/stream/StreamController.cpp`, `start`, `stop`; `StreamSession.cpp`, `run`;
  `GroovyMister.cpp`, `connect`; `src/app/StreamingCoordinator.cpp`,
  `startStreaming`: existing Qt/session ownership & stream initialization.
* Linux `Documentation/input/uinput.rst` & local `linux/uinput.h`: native
  `UI_DEV_SETUP`, capability ioctls, input events & `SYN_REPORT`.

Inherited behavior: wire protocol, session ownership, mpv mapper & A/V transport.
Linux-specific adaptation: routed UDP reception & uinput output. New policy:
mutually exclusive output backends, per-peer ownership, cadence watchdog,
neutral-device slot representation, capability recreation & retry diagnostics.

## Validation recorded for this change

The full Debug application & all test executables built with GCC 14.2, Qt 6.8.2,
PipeWire 1.4.2 & LZ4 1.10.0 on Debian 13. This build host differs from the target
Arch/CachyOS desktop. The offscreen Qt application started & `--help` completed.

CTest: 7 passed, 1 failed, 1 skipped. Passing suites were `virtual_gamepad`,
`scaling_executor`, `scaler`, `frame_processor`, `groovy_mister`, `concurrency` &
`portal_response`. `controller_input_test unit` also passed independently. The
full existing `controller_input` test failed at mock Unix socket creation with
`Operation not permitted`; its mpv IPC/receiver checks couldn't execute here.
`real_gamepad` skipped because `/dev/uinput` is absent. `/dev/input` is absent
as well; no actual virtual-device node or SDL result was obtained.

The new harness exercised signed analog decoding, both slots, malformed lengths,
simultaneous buttons, unchanged holds, releases, opposing/rapid directions,
frame rollover/reordering, idle keepalives, watchdog/reset recovery, capability
metadata, write/open failures, neutralization before destroy, real loopback UDP
registration, peer rejection, exclusive ownership, mode recreation & repeated
receiver start/stop. Its uinput syscalls are mocked; its UDP sockets are real.

Real MiSTer traffic, physical controller disconnects, kernel evdev/joydev,
SDL2/SDL3, Steam, RetroArch, Wine/Proton & live video/audio/mpv operation remain
unverified. On the target host run the full CTest suite, `real_gamepad_test`,
then evtest/jstest during streaming. Check combinations, holds longer than the
normal idle interval, both controllers, a temporary network interruption,
disconnect while pressed, stream restarts & application restarts. Verify sound
& picture while exercising controls, including when mpv isn't running.
