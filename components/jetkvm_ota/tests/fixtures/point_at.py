#!/usr/bin/env python3
"""Prints the place, in an armored OpenPGP key file, of a base64 character
whose six bits all belong to the Ed25519 point of the key's n-th subkey.

    point_at.py <key.asc> <n>

make_fixtures.sh writes the result into root.h, so that the host test can
change one subkey and nothing else.
"""
import base64
import sys

text = open(sys.argv[1]).read()
# The armor headers end at the first empty line; the base64 text has 64
# characters in a line.
start = text.index("\n\n") + 2
lines = text[start:].split("\n")
assert all(len(line) == 64 for line in lines[:-4]), "not 64 characters in a line"
data = base64.b64decode("".join(line for line in lines if not line.startswith(("=", "-"))))

# GnuPG writes a key's packets in the old format with a one-octet length.
at, subkeys = 0, []
while at < len(data):
    assert data[at] & 0xC3 == 0x80, "not an old-format packet with a one-octet length"
    if (data[at] >> 2) & 0x0F == 14:
        subkeys.append(at + 2)
    at += 2 + data[at + 1]

# A subkey's body: version (1 octet), creation time (4), algorithm (1), the
# curve (10), the length of the MPI (2), 0x40, then the 32 octets of the
# point. The first octet at or after the point's tenth that starts a base64
# group has its upper six bits in one character.
octet = subkeys[int(sys.argv[2]) - 1] + 1 + 4 + 1 + 10 + 2 + 1 + 9
octet += -octet % 3
character = octet // 3 * 4
print(start + character + character // 64)
