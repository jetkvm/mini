"""Exercise the production upload allocation after successful admission."""
from pathlib import Path
import os
import re
import resource
import subprocess
import tempfile
import unittest


def function(source, name):
    start = re.search(r"static bool " + name + r"\([^;]*?\)\s*\{", source)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define UPLOAD_DMA_ALIGNMENT 128U
#define UPLOAD_BUFFER_CAPS 1U
#define UPLOAD_FALLBACK_BUFFER_CAPS 2U
#define ESP_LOGW(...) ((void)0)
typedef struct { bool active; unsigned domain; } jetkvm_memory_policy_reservation_t;
typedef struct { unsigned domain; } jetkvm_memory_policy_request_t;
static unsigned internal_calls, psram_calls, frees, releases, reservations;
static unsigned failure_index;
static bool fail_internal, fail_psram, deny_psram;
static void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    assert(alignment == UPLOAD_DMA_ALIGNMENT && size == UPLOAD_ALLOCATION_SIZE);
    if (caps == UPLOAD_BUFFER_CAPS) {
        internal_calls++;
        if (fail_internal && internal_calls == failure_index) return NULL;
    } else {
        assert(caps == UPLOAD_FALLBACK_BUFFER_CAPS);
        assert(reservations || internal_calls == 0U);
        psram_calls++;
        if (fail_psram && psram_calls == failure_index) return NULL;
    }
    return malloc(size);
}
static void heap_caps_free(void *ptr) { if (ptr) { frees++; free(ptr); } }
static void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *r) {
    if (r->active) { r->active = false; releases++; }
}
static bool jetkvm_memory_policy_reserve(const char *owner,
    const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *r) {
    assert(strcmp(owner, "http-storage-upload-psram") == 0);
    assert(!r->active && frees == failure_index - 1U && request->domain == 2U);
    reservations++;
    if (deny_psram) return false;
    r->active = true; r->domain = request->domain;
    return true;
}
'''
SCENARIOS = r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    failure_index = (unsigned)atoi(argv[2]);
    assert(failure_index >= 1U && failure_index <= UPLOAD_ALLOCATION_COUNT);
    const bool preferred = strcmp(argv[1], "preferred") == 0;
    const bool initially_external = strcmp(argv[1], "external-fail") == 0;
    bool external = initially_external;
    fail_internal = !preferred;
    fail_psram = external || strcmp(argv[1], "fallback-fail") == 0;
    deny_psram = strcmp(argv[1], "denied") == 0;
    uint8_t *allocations[UPLOAD_ALLOCATION_COUNT] = {0};
    jetkvm_memory_policy_reservation_t r = {.active = true, .domain = external ? 2U : 1U};
    jetkvm_memory_policy_request_t request = {.domain = 2U};
    bool result = allocate_upload_buffers_with_fallback(allocations, &external, &r, &request);
    assert(result == (!fail_psram && !deny_psram));
    if (preferred) {
        assert(internal_calls == UPLOAD_ALLOCATION_COUNT && psram_calls == 0U);
        assert(releases == 0U && reservations == 0U && !external);
    } else if (initially_external) {
        assert(internal_calls == 0U && psram_calls == failure_index && reservations == 0U);
    } else {
        assert(internal_calls == failure_index && releases == 1U && reservations == 1U);
        assert(psram_calls == (deny_psram ? 0U :
                               fail_psram ? failure_index : UPLOAD_ALLOCATION_COUNT));
        assert(external == !deny_psram);
    }
    assert(r.active == !deny_psram);
    for (unsigned i = 0; i < UPLOAD_ALLOCATION_COUNT; ++i) {
        assert((allocations[i] != NULL) == result);
        heap_caps_free(allocations[i]);
    }
    jetkvm_memory_policy_release(&r);
    const unsigned partial = failure_index - 1U;
    assert(frees == (preferred ? UPLOAD_ALLOCATION_COUNT :
                     initially_external || deny_psram ? partial :
                     fail_psram ? 2U * partial : partial + UPLOAD_ALLOCATION_COUNT));
    assert(!r.active);
    assert(releases == (preferred || initially_external || deny_psram ? 1U : 2U));
    return 0;
}
'''


class UploadAllocationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(os.environ.get(
            "UPLOAD_SOURCE", Path(__file__).parents[1] / "jetkvm_virtual_media_http.c")).read_text()
        test_source = Path(cls.temp.name) / "allocation.c"
        test_source.write_text(MODEL + function(source, "allocate_upload_buffers") +
                               function(source, "allocate_upload_buffers_with_fallback") + SCENARIOS)
        cls.binaries = []
        # The shipped pipeline shares one 48 KiB allocation; the legacy
        # geometry exercises cleanup after a partially allocated 24 KiB pair.
        for count, size in ((1, 49152), (2, 24576)):
            binary = Path(cls.temp.name) / f"allocation-{count}"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            f"-DUPLOAD_ALLOCATION_COUNT={count}U",
                            f"-DUPLOAD_ALLOCATION_SIZE={size}U",
                            str(test_source), "-o", str(binary)], check=True)
            cls.binaries.append((count, size, binary))

    def scenario(self, name):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        for count, size, binary in self.binaries:
            for failure_index in range(1, count + 1):
                with self.subTest(count=count, size=size, failure_index=failure_index):
                    result = subprocess.run([str(binary), name, str(failure_index)],
                                            capture_output=True, text=True,
                                            preexec_fn=no_core)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_preferred_allocation(self): self.scenario("preferred")
    def test_admitted_but_fragmented_internal_heap(self): self.scenario("fallback")
    def test_psram_admission_denied(self): self.scenario("denied")
    def test_psram_allocation_fails(self): self.scenario("fallback-fail")
    def test_existing_psram_failure_does_not_retry(self): self.scenario("external-fail")


if __name__ == "__main__":
    unittest.main()
