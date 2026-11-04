#!/usr/bin/env python3
"""Writes the hash that a detached OpenPGP signature signs: the hash of the
file followed by the signature's trailer (RFC 9580 5.2.4).

    signed_digest.py <file> <file.sig> > <digest>

For a version 4 signature over SHA-512 in one packet with a two-octet
header, which is what GnuPG writes for an Ed25519 key.
"""
import hashlib
import sys

body = open(sys.argv[2], "rb").read()[2:]
assert body[0] == 4 and body[3] == 10, "not a version 4 signature over SHA-512"
# The hashed part: six octets and the hashed subpackets, whose length is in
# octets 4 and 5.
hashed = body[:6 + int.from_bytes(body[4:6], "big")]
digest = hashlib.sha512()
with open(sys.argv[1], "rb") as file:
    while block := file.read(1 << 20):
        digest.update(block)
digest.update(hashed + b"\x04\xff" + len(hashed).to_bytes(4, "big"))
sys.stdout.buffer.write(digest.digest())
