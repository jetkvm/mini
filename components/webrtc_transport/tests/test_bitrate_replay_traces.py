#!/usr/bin/env python3
"""Replay recorded Wi-Fi Mini sessions through the bitrate controller.

Both traces were recorded at 1080p30 (7 Mbit/s ceiling, a GNOME Activities
toggle every 2.5 s for 120 s). On the clean LAN the receiver lost no packets while the
sender dropped 1-2 frames per second and Chrome's estimate fell with every
scene change: the controller must not cut. Behind a 1.5 Mbit/s policer the
receiver lost about 600 packets: the controller must cut. The replay is
open-loop (the recorded bytes were sent under the recorded targets).
"""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
traces = component / 'tests' / 'traces'

with tempfile.TemporaryDirectory(prefix='bitrate-replay-') as directory:
    binary = Path(directory) / 'bitrate_replay'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(component), str(component / 'adaptive_bitrate.c'),
                    str(component / 'tests' / 'bitrate_replay.c'), '-o', str(binary)],
                   check=True)

    def replay(name):
        out = subprocess.run([str(binary), str(traces / name)], check=True,
                             capture_output=True, text=True).stdout
        line = next(l for l in out.splitlines() if l.startswith('replayed'))
        cuts = int(re.search(r'cuts\s+(\d+)', line)[1])
        below = float(re.search(r'\(\s*([\d.]+)%\)', line)[1])
        return cuts, below

    cuts, below = replay('wifi-clean-motion.csv')
    assert cuts == 0 and below == 0.0, (cuts, below)
    cuts, below = replay('wifi-policer-1500k-motion.csv')
    assert cuts >= 10 and below > 50.0, (cuts, below)
print('bitrate replay of recorded Wi-Fi sessions: clean holds, policer cuts: OK')
