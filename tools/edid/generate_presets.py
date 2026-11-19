#!/usr/bin/env python3
"""Generate the Mini's EDID presets. Each lists every mode the Mini captures.

"JetKVM Default" carries upstream JetKVM's manufacturer ID "JET" and monitor
name "JetKVM v1": OS Services finds the KVM's display by that ID. "DELL D2721H" carries
the identity of upstream JetKVM's DELL D2721H preset (vendor, product, serial,
date, size and name) for hosts that expect a known monitor; its modes are the
Mini's, because the monitor's own 1920x1080p60 cannot be captured.

Preferred timing: 1080p30. CEA timings: 1080p30 (VIC 34), 720p30 (VIC 62),
720p60 (VIC 4) and 640x480p60 (VIC 1, which CTA-861 requires of every sink). Other timings: `cvt 1024 768 30`, `cvt 1280 800 30`,
`cvt 1280 1024 30`, `cvt 1440 900 30` and `cvt 1680 1050 30`. CVT clock
quantization makes these 29.83..29.95 Hz.
Established timings: 640x480p60 and 800x600p60 alone. At 30 Hz their pixel
clocks would be below the 25 MHz that HDMI can carry, and a source that is
not offered them scales its 640x480 or 800x600 picture (firmware set-up,
boot loader) to the preferred timing instead. No standard timings or
inferred GTF modes add other rates.
Audio uses USB, so neither preset advertises HDMI audio or speakers.

The CTA extension says what a capture device wants from a PC, as upstream
JetKVM's EDID does: IT formats are shown underscanned (a source otherwise
may shrink or crop the desktop for a television's overscan), the first
detailed timing is the native one, and a Video Capability Data Block lets the
source choose the quantization range and say so in its AVI InfoFrame.
"""
from pathlib import Path
import argparse

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'components/jetkvm_hdmi_bridge/edid_presets_data.h'
# clock in 10 kHz; active, sync start/end, total; separate-sync polarity.
MODES = {
    '1080p30': (7425, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 0x1e),
    '720p60': (7425, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 0x1e),
    '1024x768p30': (3000, 1024, 1056, 1152, 1280, 768, 771, 775, 784, 0x1c),
    '1280x800p30': (3900, 1280, 1312, 1440, 1600, 800, 803, 809, 817, 0x1c),
    '1280x1024p30': (5000, 1280, 1312, 1440, 1600, 1024, 1027, 1034, 1045, 0x1c),
    '1440x900p30': (4925, 1440, 1480, 1616, 1792, 900, 903, 909, 919, 0x1c),
    '1680x1050p30': (6775, 1680, 1728, 1896, 2112, 1050, 1053, 1059, 1071, 0x1c),
}

def dtd(name, image_mm):
    clock, w, hs, he, ht, h, vs, ve, vt, flags = MODES[name]
    wmm, hmm = image_mm
    hb, vb, hf, hw, vf, vw = ht-w, vt-h, hs-w, he-hs, vs-h, ve-vs
    return bytes([clock & 255, clock >> 8, w & 255, hb & 255,
                  ((w >> 8) << 4) | (hb >> 8), h & 255, vb & 255,
                  ((h >> 8) << 4) | (vb >> 8), hf & 255, hw & 255,
                  ((vf & 15) << 4) | (vw & 15),
                  ((hf >> 8) << 6) | ((hw >> 8) << 4) | ((vf >> 4) << 2) | (vw >> 4),
                  wmm & 255, hmm & 255, ((wmm >> 8) << 4) | (hmm >> 8), 0, 0, flags])

def descriptor(tag, text):
    return bytes([0, 0, 0, tag, 0]) + text.encode().ljust(13, b' ')[:13]

# identity: vendor/product/serial/week/year, size in cm, name and serial text,
# image size in mm for the detailed timings (it has to round to the cm size).
IDENTITIES = {
    'default': ('28b40100010000001c24', '341d', 'JetKVM v1\n', 'P4TC358743\n', (509, 286)),
    'dell_d2721h': ('10ac132045393639201e', '3c22',
                    'DELL D2721H\n', '3S5GQ23\n', (597, 336)),
}

def preset(identity='default'):
    ids, size, name, serial, image_mm = IDENTITIES[identity]
    base = bytearray.fromhex('00ffffffffffff00' + ids + '010380' + size + '780a5ec0a4594a9825205054')
    base += bytes([0x21, 0, 0])  # established: 640x480p60 and 800x600p60
    base += b'\x01\x01' * 8  # standard timings cannot encode refresh below 60 Hz
    base += dtd('1080p30', image_mm)
    base += descriptor(0xfc, name)
    # No range/formula descriptor: publish discrete timings only.
    base += bytes([0, 0, 0, 0x10, 0]) + bytes(13)
    base += descriptor(0xff, serial)
    base += bytes([1, 0])
    base[127] = -sum(base) & 255
    # CTA lists supported progressive video VICs; audio uses USB.
    svds = [0x22, 0x3e, 0x04, 0x01]  # 1080p30, 720p30, 720p60, 640x480p60
    blocks = bytes([0x40 | len(svds), *svds,
                    0x67, 0x03, 0x0c, 0, 0x10, 0, 0, 0x21,
                    # Video Capability: YCC and RGB quantization selectable;
                    # IT and CE formats both over- and underscanned.
                    0xe2, 0x00, 0xcf])
    # Underscan IT formats, YCbCr 4:4:4 and 4:2:2, one native detailed
    # timing; no Basic Audio.
    extension = bytearray([2, 3, 4+len(blocks), 0xb1]) + blocks
    for mode in ('1024x768p30', '1280x800p30', '1280x1024p30', '1440x900p30',
                 '1680x1050p30'):
        extension += dtd(mode, image_mm)
    extension += bytes(128-len(extension))
    extension[127] = -sum(extension) & 255
    assert len(base) == len(extension) == 128
    return bytes(base + extension)

def render():
    text = '/* Generated by tools/edid/generate_presets.py; do not edit bytes by hand. */\n#pragma once\n\n'
    for identity in IDENTITIES:
        b = preset(identity)
        text += f'static const uint8_t s_edid_{identity}[256] = {{\n'
        text += '\n'.join('    '+', '.join(f'0x{x:02x}' for x in b[i:i+8])+',' for i in range(0,256,8))
        text += '\n};\n\n'
    return text.rstrip() + "\n"

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.check:
        assert OUTPUT.read_text() == render(), 'regenerate the EDID header'
    else:
        OUTPUT.write_text(render())
