# mpv controller support

Controller support is disabled by default. MiSTerCast connects to an externally
started mpv process; this repository had no mpv launcher or existing IPC socket
at base commit `babd47b8e0119405c4efb58ef7d4b973ac7caec4`.

## Enable controls

1. Enable **Joysticks: Digital** or **Analog** in Groovy_MiSTer's OSD before
   starting the stream. Map the physical controller buttons in MiSTer.
2. Start mpv with a socket in your private runtime directory:

   ```sh
   mpv --input-ipc-server="$XDG_RUNTIME_DIR/mistercast-mpv.sock" movie.mkv
   ```

3. In MiSTerCast, check **Control mpv with MiSTer controllers**. Enter the expanded
   absolute socket path in **mpv IPC socket**; the field does not expand shell
   variables. `printf '%s/mistercast-mpv.sock\n' "$XDG_RUNTIME_DIR"` prints it.
4. Select the desktop source containing mpv, then start streaming to MiSTer.

The checkbox & path are saved with the existing QSettings configuration under
`controllerEnabled` & `mpvIpcPath`. They apply to each new stream. Stop streaming
before changing them. Socket setup errors are logged without stopping video.

| Groovy input | Mask | Native mpv command |
| --- | --- | --- |
| Button 1 | `0x0010` | `cycle pause` |
| D-pad Left | `0x0002` | `seek -5 relative` |
| D-pad Right | `0x0001` | `seek 5 relative` |
| D-pad Up | `0x0008` | `add volume 2` |
| D-pad Down | `0x0004` | `add volume -2` |
| Left shoulder / Button 7 | `0x0400` | `playlist-prev weak` |
| Right shoulder / Button 8 | `0x0800` | `playlist-next weak` |

Button 1 means the core's mapped Button 1. There is no universal physical A label
in the wire protocol: upstream RetroArch calls Button 1 **B**, Button 2 **A**,
Buttons 5/6 **Select/Start**, & Buttons 7/8 **L/R**. Map the desired physical A
button to core Button 1 for this feature. Buttons 2 through 6 & 9/10 are unbound.
Analog axes, keyboard & mouse packets do not control playback.

Seek steps are 5 seconds; volume steps are 2 mpv volume units. D-pad presses act
immediately, then repeat after 400 ms at 150 ms intervals. A delayed worker tick
produces at most one repeat per action. Pause & playlist navigation act only on
press. Both controllers retain separate press/release/held masks; overlapping
presses of the same action share one effective hold, so pressing pause on both
controllers does not toggle twice. Releasing one controller does not cancel an
action still held by the other. Different simultaneous actions are dispatched
in table order.

## Verified upstream protocol

All Groovy references below are pinned to commit
`109908c9ddbd4397ba7aadb193de61fdb8cb34e6` of `psakhis/Groovy_MiSTer`.

| Source | Verified behavior |
| --- | --- |
| [groovy.cpp lines 882–909](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L882-L909) | Sender byte assignments, little-endian integers, both digital maps, optional eight analog bytes. |
| [groovymister.h lines 29–42](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/api/groovymister.h#L29-L42) | Direction & numbered-button bitmasks. |
| [mister_joypad.c lines 64–83](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/retroarch/src/input/drivers_joypad/mister_joypad.c#L64-L83) | Physical-label convention, including shoulder Buttons 7/8. |
| [BindInputs / PollInputs lines 839–932](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/api/groovymister.cpp#L839-L932) | One-byte registration, ephemeral client port, accepted joystick lengths, frame/order comparison. |
| [groovy.cpp lines 1169–1184](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L1169-L1184) | Non-XDP input registration is consumed in `setInit`, requiring registration before stream initialization. |
| [groovy.cpp lines 1225–1231](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L1225-L1231) & [2521–2536](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L2521-L2536) | Order reset per blit, increment on input events, controller IDs 0/1 select map slots. |
| [groovy.cpp lines 98–125](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L98-L125) & [1124–1128](https://github.com/psakhis/Groovy_MiSTer/blob/109908c9ddbd4397ba7aadb193de61fdb8cb34e6/hps_linux/src/support/groovy/groovy.cpp#L1124-L1128) | Server port 32101 & idle snapshot interval of 2700 blits. |

| Byte offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | Frame counter, unsigned little-endian |
| 4 | 1 | Event order within a blit |
| 5 | 2 | Controller 0 digital bits, unsigned little-endian |
| 7 | 2 | Controller 1 digital bits, unsigned little-endian |
| 9 | 8 | Optional analog axes, ignored |

Every valid packet contains both controllers. There is no separate controller
identifier field. Unsupported lengths, including truncated oversized datagrams,
are discarded. Equal frame/order tuples & older events are discarded without
changing state or refreshing the input watchdog. Frame comparison uses unsigned
serial arithmetic to allow 32-bit rollover; within a frame, order comparison
matches upstream's strictly increasing byte comparison.

## Ownership & failure handling

`StreamController` owns `ControllerReceiver`. When enabled, it registers a
connected, nonblocking UDP socket with the MiSTer host on port 32101 before
starting the stream. The kernel filters sender IP & port. The local port is
ephemeral, matching upstream `BindInputs`; binding a local listener alone would
not register with MiSTer. Registration has no acknowledgment. If it is lost,
restart streaming. The stream's `started` signal enables dispatch through an
atomic flag. The input worker is joined after `StreamSession::run()` returns,
including error exits. Explicit stop & destruction join it too. Every new
session gets fresh sockets, sequence tracking & button state.

`ControllerInput` decodes snapshots & handles edges/repeats without network or
Qt dependencies. `MpvIpc` uses the local Unix socket with newline-delimited JSON,
numeric request IDs & native [mpv IPC commands](https://mpv.io/manual/stable/#json-ipc).
Writes use `MSG_DONTWAIT | MSG_NOSIGNAL`; there is no unsent command queue.
At most 16 commands can await replies. Partial writes, peer closure, a reply
larger than 8192 bytes, or a one-second reply timeout stop controls for that
session. Unsolicited events do not acknowledge requests. Initial connection
failures retry once per second with one warning; inputs held during connection
failure must be released before acting. Once connected, a disconnect does not
silently attach to a replacement process: restart streaming after restarting mpv.
MiSTerCast does not launch, kill, or remove the external player's socket.

The receiver caps each drain at 64 UDP packets & polls for at most 10 ms between
iterations. IPC reads are bounded to eight 1024-byte reads per iteration.
Registration, packet decoding, repeats & IPC never execute in the streaming loop.
No video/audio transport code or UDP 32100 protocol is changed.

## Input interruption limits

Groovy has no controller-disconnect flag or session ID in these packets. Its
idle keepalive is 2700 blits, about 45 seconds at 60 Hz or 54 seconds at 50 Hz.
The receiver clears held state after 60 seconds without a newer valid snapshot;
the first recovered snapshot establishes a baseline with held buttons suppressed
until release. Restarting a stream resets this state immediately.

A lost release can therefore repeat until the next snapshot or the 60-second
timeout. A short timeout would also cancel legitimate holds because upstream
does not send frequent unchanged snapshots. At output rates below 45 blits per
second, the watchdog can expire between idle keepalives; release & press again
to resume. Same-frame event-order wrap is rejected, matching upstream. No epoch
field exists to distinguish a restarted sender from delayed packets; recovery
after a counter reset uses the inactivity timeout or an explicit session restart.

## Tests

Changed repository files:

| Paths | Modification |
| --- | --- |
| `src/input/ControllerInput.cpp`, `src/input/ControllerInput.h` | Packet decoder, ordered snapshots, independent state, action coalescing, repeats & reset behavior. |
| `src/input/ControllerReceiver.cpp`, `src/input/ControllerReceiver.h` | Registered UDP receiver, sender filtering, worker ownership & dispatch. |
| `src/input/MpvIpc.cpp`, `src/input/MpvIpc.h` | Nonblocking JSON IPC, bounded pending replies & failure handling. |
| `src/stream/StreamController.cpp`, `src/stream/StreamController.h`, `src/stream/StreamConfig.h` | Session lifecycle ownership & optional controller configuration. |
| `src/app/StreamingCoordinator.cpp`, `src/app/StreamingCoordinator.h` | Pass controller settings into each stream. |
| `src/ui/MainWindow.cpp`, `src/ui/MainWindow.h` | Persisted checkbox & IPC path, disabled during streaming. |
| `tests/ControllerInputTest.cpp`, `CMakeLists.txt` | Unit/integration coverage & build registration. |
| `README.md`, `docs/mpv-controller.md` | Setup instructions, bindings, source references & validation limits. |

With the repository's Qt/PipeWire/LZ4 build dependencies installed:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The controller tests also build without those dependencies:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -Isrc \
  tests/ControllerInputTest.cpp src/input/ControllerInput.cpp \
  src/input/MpvIpc.cpp src/input/ControllerReceiver.cpp \
  -o /tmp/mistercast-controller-test
/tmp/mistercast-controller-test
```

Optional `unit`, `ipc`, or `receiver` arguments select individual groups. The
integration groups use a mock mpv Unix server & loopback UDP sender on an
ephemeral test port. They cover registration, sender filtering, malformed/stale
packets, both controllers, dispatch, hold/release, peer exit, repeated starts &
stops, bounded requests, fragmented replies, timeouts & missing sockets.

Implementation-environment results: warning-enabled C++ compilation & the
`unit` group passed. The existing `ConcurrencyTest.cpp` also compiled & passed.
The `ipc` & `receiver` groups were attempted but socket creation returned
`Operation not permitted`; they have not passed here. The full build could not
run because CMake & pkg-config are missing, & package installation failed with
permission errors. UI integration, real mpv & MiSTer hardware remain untested.

Executed test commands after the controller compilation above:

```sh
/tmp/mistercast-controller-test unit
/tmp/mistercast-controller-test ipc
/tmp/mistercast-controller-test receiver
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -Isrc \
  tests/ConcurrencyTest.cpp src/core/FrameMailbox.cpp src/core/StereoSampleRing.cpp \
  -o /tmp/mistercast-concurrency-test
/tmp/mistercast-concurrency-test
```

`git diff --exit-code` against the base returned zero for `StreamSession.cpp`,
`GroovyMister.{cpp,h}`, `GroovyProtocolCodec.{cpp,h}`, `GroovyVideoEncoder.cpp`,
& the `src/audio`, `src/core`, `src/capture` directories. Compile-time assertions
in the new test also require transport port 32100 & input port 32101.

## Recovery before publication

The earlier local checkout & its unpublished commits were no longer present
when publication was authorized. The feature was reconstructed from the recorded
changes in the conversation. The recovered source compiled without warnings;
its unit tests & the existing concurrency test passed again. The mock socket
tests still stop at socket creation with `Operation not permitted`.
