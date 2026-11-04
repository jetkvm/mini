#!/usr/bin/env python3
"""Executable crash/failure model for persisted upload identity semantics."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import struct
import unittest
import zlib


MAGIC = b"JKUPID01"
VERSION = 1
RECORD = struct.Struct("<8sIIQ64sI")


def valid_filename(filename: str) -> bool:
    """Model the filesystem-alias checks relevant to upload artifacts."""
    return bool(filename) and filename[-1] not in {" ", "."} and ".." not in filename


def encode_identity(size: int, digest: str) -> bytes:
    if size <= 0 or len(digest) != 64:
        raise ValueError("invalid identity")
    bytes.fromhex(digest)
    payload = struct.pack(
        "<8sIIQ64s", MAGIC, VERSION, 0, size, digest.lower().encode("ascii")
    )
    return payload + struct.pack("<I", zlib.crc32(payload))


def decode_identity(record: bytes | None) -> tuple[int, str] | None:
    if record is None or len(record) != RECORD.size:
        return None
    magic, version, reserved, size, digest_bytes, stored_crc = RECORD.unpack(record)
    if (
        magic != MAGIC
        or version != VERSION
        or reserved != 0
        or size <= 0
        or zlib.crc32(record[:-4]) != stored_crc
    ):
        return None
    try:
        digest = digest_bytes.decode("ascii")
        if len(digest) != 64 or digest != digest.lower():
            return None
        bytes.fromhex(digest)
    except (UnicodeDecodeError, ValueError):
        return None
    return size, digest


@dataclass
class StorageModel:
    incomplete_size: int | None = None
    identity: bytes | None = None
    temporary: bytes | None = None
    final_exists: bool = False
    live_identity: tuple[int, str] | None = None
    image_open: bool = False
    cleanup_pending: bool = False

    def start(self, size: int, digest: str) -> int:
        requested = (size, digest.lower())
        if self.final_exists:
            raise RuntimeError("final path already exists")
        if self.live_identity is not None:
            if self.live_identity != requested:
                raise RuntimeError("different live upload")
            return self.incomplete_size or 0

        existing = self.incomplete_size or 0
        stored = decode_identity(self.identity)
        if existing > 0 and stored != requested:
            raise RuntimeError("non-empty prefix lacks exact identity")
        if existing == 0 and stored != requested:
            # Empty/no-payload orphans carry no prefix. Publication is
            # sidecar.tmp -> sync/close -> same-directory rename.
            self.identity = None
            self.temporary = encode_identity(*requested)
            self.identity = self.temporary
            self.temporary = None
        else:
            self.temporary = None
        if self.incomplete_size is None:
            self.incomplete_size = 0
        self.live_identity = requested
        return existing

    def abort(self) -> None:
        self.live_identity = None

    def finish(self, *, rename_succeeds: bool, identity_unlink_succeeds: bool) -> bool:
        if self.live_identity is None:
            raise RuntimeError("no live upload")
        size, _ = self.live_identity
        if self.incomplete_size != size:
            raise RuntimeError("incomplete payload")
        self.live_identity = None
        if not rename_succeeds:
            return False
        self.final_exists = True
        self.incomplete_size = None
        if identity_unlink_succeeds:
            self.identity = None
        else:
            self.cleanup_pending = True
        # Publication is already complete and cannot be rolled back merely
        # because cleanup of hidden metadata failed.
        return True

    def service_metadata_cleanup(self, *, succeeds: bool) -> None:
        if not self.cleanup_pending:
            return
        if not succeeds:
            raise RuntimeError("metadata cleanup still pending")
        self.identity = None
        self.cleanup_pending = False

    def delete_incomplete(self, filename: str = "image.iso.incomplete") -> None:
        if not valid_filename(filename):
            raise ValueError("invalid filename alias")
        lowered = filename.lower()
        if lowered.endswith(".incomplete.identity") or lowered.endswith(
            ".incomplete.identity.tmp"
        ):
            raise ValueError("reserved metadata path")
        if self.live_identity is not None or self.image_open:
            raise RuntimeError("live FIL")
        if not lowered.endswith(".incomplete"):
            raise ValueError("not an incomplete upload")
        self.incomplete_size = None
        self.identity = None
        self.temporary = None

    def listed_names(self) -> list[str]:
        names = ["image.iso"] if self.final_exists else []
        # .incomplete remains user-visible for resume/delete compatibility;
        # identity and temporary metadata never do.
        if self.incomplete_size is not None:
            names.append("image.iso.incomplete")
        return names


@dataclass
class MetadataFilModel:
    role: str | None = None
    open: bool = False
    cleanup_pending: bool = False
    temporary_exists: bool = False

    def begin(self, role: str) -> None:
        if self.role is not None:
            raise RuntimeError("static FIL already occupied")
        self.role = role
        self.open = True
        self.temporary_exists = role == "temp"

    def close(self, *, succeeds: bool, cleanup_temporary: bool) -> bool:
        if not succeeds:
            if self.role == "temp":
                self.cleanup_pending = True
            return False
        self.open = False
        self.cleanup_pending = cleanup_temporary
        if cleanup_temporary:
            self.temporary_exists = False
        self.role = None
        return True

    def service(self, *, close_succeeds: bool, unlink_succeeds: bool = True) -> bool:
        if self.open and not close_succeeds:
            return False
        self.open = False
        if self.cleanup_pending and not unlink_succeeds:
            return False
        if self.cleanup_pending:
            self.temporary_exists = False
        self.role = None
        self.cleanup_pending = False
        return True


class UploadIdentityModelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.digest = hashlib.sha256(b"whole file A").hexdigest()
        self.other = hashlib.sha256(b"whole file B").hexdigest()

    def test_nonzero_prefix_requires_exact_valid_identity(self) -> None:
        for identity in (
            None,
            encode_identity(99, self.digest),
            encode_identity(100, self.other),
            b"corrupt",
        ):
            model = StorageModel(incomplete_size=50, identity=identity)
            with self.assertRaises(RuntimeError):
                model.start(100, self.digest)
            self.assertEqual(model.incomplete_size, 50)
            self.assertIs(model.identity, identity)

        valid = StorageModel(
            incomplete_size=50, identity=encode_identity(100, self.digest)
        )
        self.assertEqual(valid.start(100, self.digest), 50)

    def test_live_resume_also_matches_digest(self) -> None:
        model = StorageModel()
        model.start(100, self.digest)
        with self.assertRaises(RuntimeError):
            model.start(100, self.other)

    def test_empty_orphan_reestablishes_identity(self) -> None:
        model = StorageModel(
            incomplete_size=0, identity=encode_identity(10, self.other)
        )
        self.assertEqual(model.start(100, self.digest), 0)
        self.assertEqual(decode_identity(model.identity), (100, self.digest))

    def test_temporary_only_crash_is_republished(self) -> None:
        model = StorageModel(
            temporary=encode_identity(100, self.digest), incomplete_size=0
        )
        self.assertEqual(model.start(100, self.digest), 0)
        self.assertIsNone(model.temporary)
        self.assertEqual(decode_identity(model.identity), (100, self.digest))

    def test_crc_bit_flip_never_authorizes_resume(self) -> None:
        record = bytearray(encode_identity(100, self.digest))
        for byte in range(len(record)):
            for bit in range(8):
                corrupt = record.copy()
                corrupt[byte] ^= 1 << bit
                model = StorageModel(incomplete_size=50, identity=bytes(corrupt))
                with self.assertRaises(RuntimeError):
                    model.start(100, self.digest)

    def test_rename_failure_keeps_recoverable_pair(self) -> None:
        model = StorageModel()
        model.start(100, self.digest)
        model.incomplete_size = 100
        self.assertFalse(
            model.finish(rename_succeeds=False, identity_unlink_succeeds=True)
        )
        self.assertFalse(model.final_exists)
        self.assertEqual(model.start(100, self.digest), 100)

    def test_published_final_is_success_even_if_cleanup_fails(self) -> None:
        model = StorageModel()
        model.start(100, self.digest)
        model.incomplete_size = 100
        self.assertTrue(
            model.finish(rename_succeeds=True, identity_unlink_succeeds=False)
        )
        self.assertTrue(model.final_exists)
        self.assertIsNotNone(model.identity)
        self.assertTrue(model.cleanup_pending)
        with self.assertRaises(RuntimeError):
            model.service_metadata_cleanup(succeeds=False)
        model.service_metadata_cleanup(succeeds=True)
        self.assertIsNone(model.identity)
        self.assertFalse(model.cleanup_pending)
        with self.assertRaises(RuntimeError):
            model.start(100, self.digest)

    def test_delete_incomplete_cleans_all_identity_artifacts(self) -> None:
        model = StorageModel(
            incomplete_size=25,
            identity=encode_identity(100, self.digest),
            temporary=b"stale",
        )
        model.delete_incomplete("IMAGE.ISO.INCOMPLETE")
        self.assertIsNone(model.incomplete_size)
        self.assertIsNone(model.identity)
        self.assertIsNone(model.temporary)
        self.assertNotIn("identity", " ".join(model.listed_names()))

    def test_delete_rejects_reserved_names_and_every_live_fil(self) -> None:
        with self.assertRaises(ValueError):
            StorageModel().delete_incomplete(
                "IMAGE.ISO.INCOMPLETE.IDENTITY"
            )
        with self.assertRaises(ValueError):
            StorageModel().delete_incomplete(
                "IMAGE.ISO.INCOMPLETE.IDENTITY.TMP"
            )
        for aliased in (
            "IMAGE.ISO.INCOMPLETE.",
            "IMAGE.ISO.INCOMPLETE ",
            "IMAGE.ISO.INCOMPLETE.IDENTITY.",
            "IMAGE.ISO.INCOMPLETE.IDENTITY.TMP ",
        ):
            with self.subTest(aliased=aliased), self.assertRaises(ValueError):
                StorageModel().delete_incomplete(aliased)

        mounted = StorageModel(incomplete_size=0, image_open=True)
        with self.assertRaises(RuntimeError):
            mounted.delete_incomplete("unrelated.INCOMPLETE")

        uploading = StorageModel()
        uploading.start(100, self.digest)
        with self.assertRaises(RuntimeError):
            uploading.delete_incomplete("unrelated.INCOMPLETE")

    def test_metadata_close_failure_retains_static_fil(self) -> None:
        read = MetadataFilModel()
        read.begin("read")
        self.assertFalse(read.close(succeeds=False, cleanup_temporary=False))
        self.assertTrue(read.open)
        self.assertEqual(read.role, "read")
        self.assertFalse(read.service(close_succeeds=False))
        self.assertTrue(read.service(close_succeeds=True))

        temporary = MetadataFilModel()
        temporary.begin("temp")
        self.assertFalse(
            temporary.close(succeeds=False, cleanup_temporary=False)
        )
        self.assertTrue(temporary.open)
        self.assertTrue(temporary.cleanup_pending)
        self.assertTrue(temporary.temporary_exists)
        # A live temp is never unlinked. Cleanup happens only after close.
        self.assertFalse(temporary.service(close_succeeds=False))
        self.assertTrue(temporary.temporary_exists)
        self.assertFalse(
            temporary.service(close_succeeds=True, unlink_succeeds=False)
        )
        self.assertFalse(temporary.open)
        self.assertTrue(temporary.cleanup_pending)
        self.assertTrue(temporary.service(close_succeeds=True))
        self.assertFalse(temporary.temporary_exists)


if __name__ == "__main__":
    unittest.main()
