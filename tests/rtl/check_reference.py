#!/usr/bin/env python3
"""Drive and check the original CRT-adjust RTL from its pinned revision."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

FILES = {
    'rtl/crt_adjust.sv': 'e66c4bae75ca86606c134fa82e57f63274331422a2c8931188495dfeac5afc21',
    'rtl/crt_adjust_sys.sv': '5adac3ebf3616716d00d05332c1fc9cc3e02deb576b06d9b24946e21058cdd82',
    'rtl/crt_vsize.sv': '77e5baa2651b13009be588cec472ab4570913d974b465d392afdcf9c2ed8a496',
    'examples/core_side_snippet.v': '33348159c307005680841a75e7a62608f8b23e3de42bf9806ef00ca3c7ca2f3d'
}
REVISION = 'c682de9f4acc61d8f4c7779efb48149d3baa3a8e'


def compile_test(args, tmp, top, mode):
    exe = tmp / top
    subprocess.run([args.iverilog, '-g2012', '-I', str(tmp), f'-DPOSITION_MODE={mode}',
                    '-s', top, '-o', str(exe), *[str(args.rtl / f) for f in FILES if f.endswith('.sv')],
                    str(Path(__file__).with_name('crt_geometry_tb.sv'))], check=True)
    return exe


def simulate(args, exe, **parameters):
    log = subprocess.check_output([args.vvp, str(exe),
                                  *(f'+{k}={v}' for k, v in parameters.items())], text=True)
    return {kind: [list(map(int, line.split()[1:])) for line in log.splitlines()
                   if line.startswith(kind + ' ')] for kind in ('H', 'V', 'RGB')}


def horizontal(args, exe, mode, fixtures):
    count, grid_differences, split_windows, unsigned_offsets = 0, 0, 0, 0
    cases = [(h, 0, 28, 0) for h in range(-16, 16)]
    cases += [(h, x, start, v) for h in (-16, 0, 15) for x in (-48, -7, 9, 48)
              for start, v in ((54, -16), (19, 15))]
    for h, shift, start, v in cases:
        period = 64 + h
        trace = simulate(args, exe, h=h, x=shift, start=start, v=v)['H']
        assert trace, (mode, h, shift)
        # Keep the complete sampled line after the reference restart. The first
        # read tick resets rdcnt, and the memory and RGB registers each add a tick.
        line = [row for row in trace if row[0] > 0]
        first = next(i for i in range(len(line)-1) if line[i][0] > line[i+1][0]) + 1 \
            if any(a[0] > b[0] for a, b in zip(line, line[1:])) else 0
        line = line[first:]
        segment = [row for row in line if 0 < row[0] < 2100]
        segment = segment[next(i for i, row in enumerate(segment) if row[1] == 0):]
        assert segment
        # The pinned RTL's unsigned zero arm in the conditional zero-extends
        # the signed 9-bit port. Preserve signed positioning in the host.
        offset = shift % 512 if mode else 0
        unsigned_offsets += mode == 1 and shift < 0
        for cycle, read, address, low, high, _, *_ in segment:
            assert address == (read - offset) % 1024
            assert (4 * cycle) % period < 4, (cycle, period)
        if any(row[4] - row[3] != 32 for row in segment):
            # SYNCSHIFT can put the line reference inside active video. Its
            # hb1/hb0 then straddle the counter reset, unlike the host raster.
            assert mode == 0 and start <= shift % 136 <= start + 32, (mode, h, shift, start)
            split_windows += 1
            continue
        # RGB and HB sample the stage written on the previous read tick.
        # The trace itself is sampled before that edge, so compare to two
        # ticks back. Exclude the initial line-buffer/phase swap.
        for i in range(2, len(segment)):
            _, read, _, low, high, blank, *_ = segment[i - 2]
            visible = low + offset <= read < high + offset and not blank
            assert bool(segment[i][6]) == (not visible)
        active = [row for row in segment if row[1] >= 2 and not row[6]]
        for a, b in zip(active, active[1:]):
            assert b[0] > a[0] and b[9] == a[9] + 1, (mode, h, shift, start, a, b)
        if active and active[0][9] == 0 and active[-1][9] == 31:
            anchor = active[0][0]
            for x in range(min(32, (32 * period + 63) // 64)):
                actual = max(row[9] for row in active if row[0] <= anchor + x * 16)
                ideal = x * 64 // period
                assert abs(actual - ideal) <= 1
                grid_differences += actual != ideal
                fixtures.append(('H', 32, h, x, ideal, 0))
            count += 1
    print(f'H mode={mode} cases={len(cases)} complete_windows={count} '
          f'split_sync_windows={split_windows} unsigned_offsets={unsigned_offsets} '
          f'quantized_grid_differences={grid_differences}', flush=True)


def cabinet(args, exe, fixtures):
    tests = [(n, s, 0) for n in (224, 240, 288) for s in (-7, -1, 1, 7)]
    tests += [(240, 7, 1), (240, 0, 0)]
    count = 0
    for n, s, tight in tests:
        trace = simulate(args, exe, n=n, s=s, tight=tight)
        rows = trace['V']
        assert trace['RGB'], (n, s, tight)
        if not s:
            assert not rows, 'neutral RTL should bypass'
            continue
        m = n + 3*s
        k = 256*n//m
        # The tight case runs through native VSync. There is no equivalent
        # host output window past the active height.
        rows = rows[next(i for i, row in enumerate(rows) if row[4] == 0):]
        assert len(rows) == m, (n, s, tight, len(rows), m)
        for expected_row, (active, eff, height, step, j, source, weight, position, delay) in enumerate(rows):
            assert (active, eff, height, step, j) == (n, -3*s, m, k, expected_row)
            assert (source, weight) == (j*k//256, j*k%256 if j*k//256 < n-1 else 0)
            frame_offset = n + 32 if tight and j + 22 >= n + 32 else 0
            assert position == (j + max(0, -3*s) + 2 - frame_offset) % 1024
            assert bool(delay) == bool(tight)
            host_y = j + max(0, -3*s)
            if host_y < n:
                fixtures.append(('V', n, s, host_y, source, weight))
                count += 1
    print(f'Cabinet cases={len(tests)} host_coordinate_rows={count}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rtl', type=Path)
    parser.add_argument('iverilog')
    parser.add_argument('vvp')
    parser.add_argument('host_test')
    args = parser.parse_args()
    for path, sha in FILES.items():
        assert hashlib.sha256((args.rtl / path).read_bytes()).hexdigest() == sha, (REVISION, path)
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        example = (args.rtl / 'examples/core_side_snippet.v').read_text()
        (tmp / 'read_ce.vh').write_text(example[example.index('wire hs_ref;'):example.index('// ─── 4.')])
        fixtures = []
        for mode in (0, 1):
            horizontal(args, compile_test(args, tmp, 'crt_horizontal_tb', mode), mode, fixtures)
        cabinet(args, compile_test(args, tmp, 'crt_cabinet_tb', 1), fixtures)
        fixture = tmp / 'geometry-coordinate-trace.txt'
        fixture.write_text(''.join(' '.join(map(str, sample)) + '\n' for sample in fixtures))
        subprocess.run([args.host_test, str(fixture)], check=True)


if __name__ == '__main__':
    main()
