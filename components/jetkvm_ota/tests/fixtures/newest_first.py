#!/usr/bin/env python3
"""Writes an armored OpenPGP key whose last subkey has two binding
signatures, the newer one last, with these two the other way round.

    newest_first.py <key.asc> > <key-newest-first.asc>

GnuPG drops the older binding when it is imported after the newer one, so
make_fixtures.sh cannot get this order from an import.
"""
import base64
import sys

text = open(sys.argv[1]).read()
lines = text[text.index("\n\n") + 2:].split("\n")
data = base64.b64decode("".join(line for line in lines if not line.startswith(("=", "-"))))

# GnuPG writes a key's packets in the old format with a one-octet length.
packets, at = [], 0
while at < len(data):
    assert data[at] & 0xC3 == 0x80, "not an old-format packet with a one-octet length"
    packets.append(data[at:at + 2 + data[at + 1]])
    at += len(packets[-1])


def created(packet):
    """The creation time of a version 4 subkey binding signature."""
    body = packet[2:]
    assert (packet[0] >> 2) & 0x0F == 2 and body[0] == 4 and body[1] == 0x18, "not a subkey binding"
    area, at = body[6:6 + int.from_bytes(body[4:6], "big")], 0
    while at < len(area):
        if area[at + 1] == 2:
            return int.from_bytes(area[at + 2:at + 6], "big")
        at += 1 + area[at]
    raise AssertionError("no creation time")


assert created(packets[-2]) < created(packets[-1]), "the newer binding is not the last packet"
packets[-2], packets[-1] = packets[-1], packets[-2]
data = b"".join(packets)

crc = 0xB704CE
for octet in data:
    crc ^= octet << 16
    for _ in range(8):
        crc <<= 1
        if crc & 0x1000000:
            crc ^= 0x1864CFB
encoded = base64.b64encode(data).decode()
print("-----BEGIN PGP PUBLIC KEY BLOCK-----\n")
print("\n".join(encoded[i:i + 64] for i in range(0, len(encoded), 64)))
print("=" + base64.b64encode((crc & 0xFFFFFF).to_bytes(3, "big")).decode())
print("-----END PGP PUBLIC KEY BLOCK-----")
