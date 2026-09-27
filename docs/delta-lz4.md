# Delta-LZ4 video compression

## Diagnosis / objective

The target's clean `main` at `bcb698830119e43a601d90e2226ecd1a7c8954ce`
matched `s3gfaultx/mistercast-linux` exactly. `GroovyMister::sendFrame()` sent
full LZ4 for every progressive frame or extracted interlaced field. This change
adds the receiver's existing delta variant while retaining full LZ4 as the baseline.

## Reference basis

| Repository / revision | Files & symbols inspected | Behavior used |
| --- | --- | --- |
| `s3gfaultx/mistercast-linux` / `bcb698830119e43a601d90e2226ecd1a7c8954ce` | `src/stream/GroovyMister.cpp`: `connect`, `sendFrame`, `sendPayload`, `receiveStatus`, `parseStatus`, `waitSync`; `src/stream/GroovyProtocolCodec.cpp`: `makeInitCommand`, `makeBlitCommand`; identical target's `src/stream/StreamSession.cpp`: `run`; `src/core/FrameFields.cpp`: `extractInterlacedField` | INIT mode 1; full LZ4; 1472-byte UDP payloads; preallocated batching; existing pacing, field extraction, parity selection & ACK consumption. |
| `iequalshane/MiSTerCast` / `b7493f91312e1521cb62e4982561505abfc5b4c1` | `Library/MiSTerCastLib/groovymister.cpp`: `GroovyMister::CmdBlit` | Original content gate, strict 95% size gate, 12/13-byte compressed headers & byte 12 marker. |
| `psakhis/Groovy_MiSTer` / `109908c9ddbd4397ba7aadb193de61fdb8cb34e6` | `hps_linux/src/support/groovy/groovy.cpp`: `process_packet`, `setBlit`, `setBlitLZ4`, `sendACK`, `groovy_FPGA_status`; `rtl/hps_ext.v`: `SET_BLIT_FIELD_LZ4`; `Groovy.sv`: `S_Delta_Prepare`, `S_Delta_Copy`; `rtl/lz4.v`: `lz4_uncompressed_long` assignments | Receiver accepts the 13-byte delta header under LZ4 negotiation, selects the field framebuffer, & adds each reconstructed byte modulo 256. ACK is sent on header receipt, before payload receipt/decompression. |
| `idio-sync/MiSTer_GroovyRelay` / `af3f60d5814aede8800d94589f827caa9c21d1ce` | `internal/groovy/constants.go`; `internal/groovy/builder.go`: `BuildBlitHeaderInto`; `internal/dataplane/plane.go`: `chooseFieldPayload`, `shouldForceDeltaLZ4Full`, `recordDeltaLZ4SendState`, `handleTornPayload`, `rememberSentFieldHistory`, `writeFieldSubDeltaInto`; `internal/fakemister/field_decoder.go`: `FieldDecoder.Decode` | Fixed header storage; successful-send history; separate parity identities; invalidation after torn payloads; 30-opportunity full refresh; test receiver reconstruction. |
| `asdgdg355235/mistercast-wine-linux` / `wine-dll/MiSTerCastLib.cpp` blob `8b70a3b62e223550c99c828e88ec0ef51f648b63` | `send_frame`, `previousFieldPayload_`, `pendingFieldPayload_`, `pendingFieldFrame_`, `deltaFramesSinceFull_` | Explicitly authorized comparison of delayed ACK promotion & refresh policy. Its promotion waits for a later safe frame echo. No code was copied from this port. |

The receiver emits `sendACK(udp_frame, udp_vsync)` inside the command branch
of `process_packet()`. Payloads reach `setBlitLZ4()` afterward. The target also
ignores duplicate `frameEcho` values in `parseStatus()`, so it cannot treat a
later status with the same echo as a reconstruction confirmation. The older
port's delayed promotion is not evidence of a receiver commit acknowledgement;
requiring a later field's echo would also prevent consecutive progressive
frames from immediately using the previous frame as their base.

The implemented policy therefore uses successful local submission, as permitted
by the task. It adds no ACK wait. Header lengths, marker, field separation &
byte arithmetic agree with the inspected receiver. Receiver reconstruction is
not guaranteed by successful UDP submission.

## Change

`GroovyVideoEncoder` owns full-LZ4 output, byte deltas, delta-LZ4 output & two
raw histories. All five buffers are allocated once from `kMaximumFrameBytes`
or `LZ4_compressBound`. Encoding, header creation & history updates perform no
per-frame heap allocation. Returned payload spans remain valid until the next
`prepare()` call; `commit()` is used synchronously for that prepared payload.

The sender always compresses the raw payload first. With valid same-parity
history, it computes `uint8_t(current - previous)` & counts unchanged bytes.
Delta compression runs only when all three strict conditions hold:

```text
full_lz4_size / raw_size > 0.05
unchanged_bytes / raw_size > 0.20
unchanged_bytes / raw_size > 0.90 - full_lz4_size / raw_size
```

All comparisons use 64-bit integer cross-products. Delta wins only when
`delta_lz4_size * 100 < full_lz4_size * 95`; equality falls back to full LZ4.
There is no raw or duplicate-field fallback. INIT byte 1 remains 1.

History is promoted only after the header & every payload datagram have been
submitted. Progressive mode uses slot 0. Interlaced fields use independent slots
0 & 1, each with byte count, width, field height & interlace identity. Geometry
or payload-size mismatches invalidate history. Close/reconnect clears both
slots; all modeline switches in this transport occur through reconnect.

`kFullRefreshInterval = 30` matches GroovyRelay's `interval - 1` threshold:
a full field, up to 29 same-parity deltas, then another full field. Every full
submission resets that parity's counter. A candidate dropped before its
command changes neither history nor the resync counter.

Failed header or payload submission invalidates both histories. A torn payload
marks the stream broken & subsequent commands require reconnect, because UDP
payload chunks have no per-chunk framing. Audio submission failures also clear
video history because audio uses the same unframed transport. Audio formatting,
packetization & cadence are unchanged.

The codec preserves the original 12-byte encoder & adds an overload returning
a fixed 13-byte buffer with its transmitted length. Bytes 0 through 11 retain
their original encoding. Delta appends `0x01` at byte 12.

Diagnostics-only logging adds cumulative `full_lz4`, `delta_lz4`,
`delta_content_rejected`, `delta_size_rejected`, `full_resyncs` &
`history_invalidations`. Send/resync counters count successful submissions;
rejection counters count compression decisions, including subsequently dropped
candidates. Connection startup records one history invalidation. Compression
time includes delta generation, both compression attempts when applicable, &
the successful raw-history copy. `transmittedBytes` records the selected payload.

## Files changed

- `src/stream/GroovyVideoEncoder.h`, `src/stream/GroovyVideoEncoder.cpp`: new encoder & history state.
- `src/stream/GroovyMister.h`, `src/stream/GroovyMister.cpp`: encoder integration, submission-bound history, failure invalidation & counters.
- `src/stream/GroovyProtocolCodec.h`, `src/stream/GroovyProtocolCodec.cpp`: fixed-storage delta header overload.
- `src/stream/StreamDiagnostics.h`, `src/stream/StreamSession.cpp`, `src/ui/MainWindow.cpp`: diagnostics propagation & logging.
- `tests/FrameProcessorTest.cpp`: full/delta header goldens.
- `tests/GroovyMisterTest.cpp`: arithmetic, selection, histories, refresh, UDP reconstruction & injected failures.
- `CMakeLists.txt`: encoder sources, LZ4 test linkage & test-only syscall wrappers.
- `docs/delta-lz4.md`: reference basis, behavior & validation.

## Validation

Release build used GCC 13.3.0, Qt 6.8.3, PipeWire 1.0.5 & LZ4 1.9.4.
Build dependencies were installed into the execution workspace; no repository
version requirement was lowered. With those dependency paths configured:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Configuration & build succeeded. CTest passed all four targets:
`frame_processor`, `groovy_mister`, `concurrency`, `portal_response`.

Tests preserve the existing 12-byte golden & interlaced protocol/ACK test.
Added coverage checks the 13-byte header, modulo-256 wraparound, decompression
plus reconstruction, first field behavior, parity separation, progressive mode,
useful deltas, content/size rejection, periodic refresh, natural-full counter
reset, geometry changes, invalid payload sizes, unsubmitted candidates & mode
reset. Progressive/interlaced loopback tests reassemble actual UDP datagrams,
inflate full/delta payloads & compare every reconstructed field byte.

Test-only linker wrappers inject command failure, failure after the BLIT header,
failure after one payload chunk, & a pre-command queue drop. They otherwise
forward to the real Linux syscalls. Failure tests verify send counters, history
invalidation, broken-stream rejection & full refresh of both parities after
reconnect. No production syscall hooks were added.

Diff review found no edits to payload slicing/batching/pacing, field selection,
modeline encoding, capture or normal audio behavior. New vector allocation is
confined to encoder construction; BLIT headers use stack storage.

## Hardware check

Real MiSTer hardware has not been tested. Check progressive & interlaced modes
with a mostly static detailed image plus small moving regions, then high-motion
content. Observe `delta_lz4`, fallback counts & `full_resyncs`; verify image
reconstruction, field cadence & audio continuity. Reconnect & switch modelines,
including modes with the same payload size, to check that both parity bases
restart with full fields.

Periodic full fields reduce dependence on earlier delta frames; they do not
prove delivery or guarantee recovery from arbitrary packet loss or receiver
framing loss. The receiver's ACK semantics are settled from source; sustained
FPGA reconstruction timing & loss behavior still require hardware validation.
