#include "diagnostics_zip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls, fail_at;
static bool write_chunk(const void *data, size_t size, void *ctx)
{
    if (++calls == fail_at) return false;
    return !ctx || fwrite(data, 1, size, ctx) == size;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const unsigned char bytes[] = {0, 1, 255, 42, 0};
    const diagnostics_zip_entry_t entries[] = {
        {"diagnostics.json", "{\"schemaVersion\":1}", 19},
        {"empty.txt", NULL, 0}, {"binary.dat", bytes, sizeof(bytes)}
    };
    FILE *file = fopen(argv[1], "wb"); assert(file);
    assert(diagnostics_zip_write(entries, 3, write_chunk, file));
    fclose(file);
    unsigned full_calls = calls;
    for (fail_at = 1; fail_at <= full_calls; ++fail_at) {
        calls = 0;
        assert(!diagnostics_zip_write(entries, 3, write_chunk, NULL));
        assert(calls == fail_at);
    }
    diagnostics_zip_entry_t bad = {"x", NULL, 1};
    calls = 0;
    assert(!diagnostics_zip_write(&bad, 1, write_chunk, NULL));
    assert(!diagnostics_zip_write(entries, 0, write_chunk, NULL));
    assert(calls == 0);
    bad.data = bytes;
    bad.size = UINT32_MAX;
    assert(!diagnostics_zip_write(&bad, 1, write_chunk, NULL));
    bad.size = SIZE_MAX;
    assert(!diagnostics_zip_write(&bad, 1, write_chunk, NULL));
    assert(calls == 0); /* Reject impossible ZIP32 sizes before reading data. */
    puts("ZIP transfer failures, invalid inputs and ZIP32 size bounds: PASS");
}
