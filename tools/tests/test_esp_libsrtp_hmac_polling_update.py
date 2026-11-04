#!/usr/bin/env python3
"""Differential checks for the optional register-fed HMAC message update."""

from __future__ import annotations

from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.tests import test_esp_libsrtp_hmac_fused_finish as reference


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "patches/esp_libsrtp-1.0.0-hmac-polling-update.patch"

# The common harness supplies the unchanged IDF SHA streaming implementation
# and OpenSSL compression primitive. Inject only differential checks, not a
# second copy of the candidate's update or padding algorithm.
UPDATE_CHECKS = r'''
    if(argc > 1 && strcmp(argv[1], "fault-body-update") == 0) {
        assert(srtp_hmac_esp_sha1_start(auth->state) == srtp_err_status_ok);
        zero_next_block = 1;
        srtp_hmac_esp_sha1_update(auth->state,data,64);
        return 4;
    }
    static const size_t update_lengths[] = {
        0,1,3,4,15,16,31,32,55,56,63,64,65,127,128,129,511,512,513,1200
    };
    for(unsigned alignment=0;alignment<8;alignment++) {
        for(unsigned pending=0;pending<64;pending++) {
            for(size_t j=0;j<sizeof(update_lengths)/sizeof(update_lengths[0]);j++) {
                assert(srtp_hmac_esp_sha1_start(auth->state) == srtp_err_status_ok);
                srtp_hmac_esp_sha1_ctx_t *state = auth->state;
                state->work.total[0] = 0xffffffc0u + pending;
                state->work.total[1] = 0x12345678u;
                memcpy(state->work.buffer,data,pending);
                esp_sha1_context expected = state->work;
                unsigned locks_before = acquisitions;
                unsigned dma_before = dma_calls;
                size_t length = update_lengths[j];
                assert(srtp_hmac_esp_sha1_update(state,data+alignment,length) == srtp_err_status_ok);
                assert(acquisitions-locks_before == (pending+length >= 64 ? 1u : 0u));
                assert(dma_calls == dma_before);
                assert(owned == 0);
                assert(esp_sha1_update(&expected,data+alignment,length) == 0);
                assert(memcmp(expected.total,state->work.total,sizeof(expected.total)) == 0);
                assert(memcmp(expected.state,state->work.state,sizeof(expected.state)) == 0);
                assert(expected.first_block == state->work.first_block);
                assert(expected.sha_state == state->work.sha_state);
                /* Bytes outside the pending tail are scratch, not live state. */
                size_t tail = expected.total[0] & 63;
                assert(memcmp(expected.buffer,state->work.buffer,tail) == 0);
                uint8_t expected_tag[20], actual_tag[20];
                assert(esp_sha1_finish(&expected,expected_tag) == 0);
                expected = state->outer_seed;
                assert(esp_sha1_update(&expected,expected_tag,20) == 0);
                assert(esp_sha1_finish(&expected,expected_tag) == 0);
                assert(srtp_hmac_esp_sha1_compute(state,NULL,0,20,actual_tag) == srtp_err_status_ok);
                assert(memcmp(expected_tag,actual_tag,20) == 0);
                assert(memcmp(&expected,&state->work,sizeof(expected)) == 0);
            }
        }
    }
    assert(srtp_hmac_esp_sha1_update(auth->state,NULL,0) == srtp_err_status_ok);
    assert(srtp_hmac_esp_sha1_update(auth->state,NULL,1) == srtp_err_status_bad_param);
    assert(srtp_hmac_esp_sha1_update(NULL,data,1) == srtp_err_status_bad_param);
    puts("polling update: alignment, carry, SDK state, no DMA, lock count OK");
'''


class PollingUpdateTest(unittest.TestCase):
    def test_incremental_patch_replays_and_reverses(self):
        with tempfile.TemporaryDirectory(prefix="hmac-polling-patch-") as directory:
            temp = Path(directory)
            source = reference.SOURCE.relative_to(ROOT)
            (temp / source).parent.mkdir(parents=True)
            original = reference.SOURCE.read_bytes()
            (temp / source).write_bytes(original)
            for args in (
                ["git", "apply", "--reverse", "--check", str(ROOT / "patches/esp_libsrtp-1.0.0-hmac-fused-finish.patch")],
                ["git", "apply", "--reverse", "--check", str(PATCH)],
                ["git", "apply", "--reverse", str(PATCH)],
                ["git", "apply", "--check", str(PATCH)],
                ["git", "apply", str(PATCH)],
            ):
                result = subprocess.run(args, cwd=temp, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((temp / source).read_bytes(), original)

    def test_polling_update_against_sdk_streaming_with_sanitizers(self):
        run = subprocess.run
        builds = []
        completed_binaries = set()

        def run_with_polling(args, *positional, **kwargs):
            args = list(args)
            if any(str(arg).endswith("/harness.c") for arg in args):
                harness = Path(next(str(arg) for arg in args if str(arg).endswith("/harness.c")))
                source = harness.read_text()
                if "polling update: alignment" not in source:
                    self.assertEqual(source.count("static int fail_dma;"), 1)
                    source = source.replace("static int fail_dma;", "static int fail_dma;\nstatic unsigned dma_calls;")
                    marker = "assert(owned == 1 && length % 64 == 0 && prefix_length % 64 == 0);"
                    self.assertEqual(source.count(marker), 1)
                    source = source.replace(marker, marker + "\n    dma_calls++;")
                    marker = "    unsigned initial_allocations = allocations;"
                    self.assertEqual(source.count(marker), 1)
                    source = source.replace(marker, UPDATE_CHECKS + "\n" + marker)
                    harness.write_text(source)
                args.insert(1, "-DJETKVM_SRTP_HMAC_POLLING_UPDATE=1")
                builds.append(next(arg for arg in args if str(arg).startswith("-DJETKVM_SRTP_HMAC_FUSED_FINISH=")))
            result = run(args, *positional, **kwargs)
            if len(args) == 1 and Path(args[0]).name in ("hmac-0", "hmac-1") and result.returncode == 0:
                self.assertIn("polling update: alignment", result.stdout)
                completed_binaries.add(Path(args[0]).name)
                fault = run([args[0], "fault-body-update"], capture_output=True, text=True)
                self.assertEqual(fault.returncode, -signal.SIGABRT, fault.stdout + fault.stderr)
            return result

        # Reuse all reference vectors, splits, truncations, padding faults,
        # concurrent sessions and error/zeroization checks with this option on.
        with patch.object(reference.subprocess, "run", side_effect=run_with_polling):
            reference.FusedFinishTest().test_sdk_streaming_reference_vectors_boundaries_locks_and_errors()
        self.assertEqual(set(builds), {
            "-DJETKVM_SRTP_HMAC_FUSED_FINISH=0",
            "-DJETKVM_SRTP_HMAC_FUSED_FINISH=1",
        })
        self.assertEqual(completed_binaries, {"hmac-0", "hmac-1"})


if __name__ == "__main__":
    unittest.main()
