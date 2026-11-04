"""Decode the emitted firmware bytes and check the advertised mode contract."""
import re
import unittest
from tools.edid.generate_presets import OUTPUT, render

class PresetTests(unittest.TestCase):
    def test_firmware_modes(self):
        text = OUTPUT.read_text()
        self.assertEqual(text, render())
        arrays = re.findall(r's_edid_(\w+)\[256\] = \{(.*?)\};', text, re.S)
        self.assertEqual([name for name, _ in arrays], ['default', 'dell_d2721h'])
        for name, body in arrays:
            data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', body))
            self.assertEqual(len(data), 256)
            # Manufacturer ID: three 5-bit letters, big-endian. "JET" is the
            # JetKVM ID by which OS Services finds the KVM's display.
            pnp = int.from_bytes(data[8:10], 'big')
            self.assertFalse(pnp & 0x8000)
            letters = ''.join(chr(64 + (pnp >> shift & 31)) for shift in (10, 5, 0))
            self.assertEqual(letters, 'DEL' if name == 'dell_d2721h' else 'JET')
            # Monitor name: the 0xfc display descriptor, ended by 0x0a.
            names = [data[i+5:i+18].split(b'\n')[0].decode()
                     for i in range(54, 126, 18) if data[i:i+5] == bytes([0, 0, 0, 0xfc, 0])]
            self.assertEqual(names, ['DELL D2721H' if name == 'dell_d2721h' else 'JetKVM v1'])
            self.assertEqual(data[126], 1)
            self.assertEqual(sum(data[:128]) % 256, 0)
            self.assertEqual(sum(data[128:]) % 256, 0)
            self.assertEqual(data[35:38], bytes(3))
            self.assertEqual(data[38:54], b'\x01\x01' * 8)
            self.assertFalse(data[24] & 1)  # no inferred GTF modes
            ext = data[128:]
            self.assertEqual(ext[:2], b'\x02\x03')
            self.assertFalse(ext[3] & 0x40, f'{name} advertises Basic Audio')
            vics = []
            pos = 4
            while pos < ext[2]:
                tag, size = ext[pos] >> 5, ext[pos] & 31
                self.assertNotIn(tag, (1, 4), f'{name} advertises audio or speakers')
                self.assertLessEqual(pos + 1 + size, ext[2])
                block = ext[pos+1:pos+1+size]
                if tag == 2:
                    vics.extend(x & 127 for x in block)
                pos += size + 1
            self.assertEqual(pos, ext[2])
            self.assertEqual(vics, [34, 62, 4])  # 1080p30, 720p30, 720p60
            modes = []
            for d in [data[54:72]] + [ext[i:i+18] for i in range(ext[2], 110, 18)]:
                clock = int.from_bytes(d[:2], 'little') * 10000
                if not clock:
                    continue
                w, hb = d[2] + ((d[4] & 240) << 4), d[3] + ((d[4] & 15) << 8)
                h, vb = d[5] + ((d[7] & 240) << 4), d[6] + ((d[7] & 15) << 8)
                fps = clock / ((w + hb) * (h + vb))
                self.assertGreater(fps, 29.5)
                self.assertLessEqual(fps, 60 if (w, h) == (1280, 720) else 30)
                self.assertFalse(d[17] & 128)  # no interlaced modes
                modes.append((w, h))
            self.assertEqual(modes[0], (1920,1080))
            self.assertEqual(set(modes[1:]), {(1024,768), (1280,800), (1280,1024)})

if __name__ == '__main__':
    unittest.main()
