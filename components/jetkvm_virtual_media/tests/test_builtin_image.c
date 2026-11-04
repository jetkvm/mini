#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "builtin_image.h"

/* 64 bytes: "AB" at 4, "CDE" at 20, "F" at 63; zero everywhere else. */
static const jetkvm_builtin_image_run_t runs[] = {
    { 4U, 2U },
    { 20U, 3U },
    { 63U, 1U },
};
static const uint8_t data[] = { 'A', 'B', 'C', 'D', 'E', 'F' };
static const jetkvm_builtin_image_t image = {
    .size = 64U,
    .run_count = 3U,
    .runs = runs,
    .data = data,
};

static void expected(uint8_t out[64])
{
    memset(out, 0, 64);
    memcpy(out + 4, "AB", 2);
    memcpy(out + 20, "CDE", 3);
    out[63] = 'F';
}

static void test_every_range_matches_the_whole_image(void)
{
    uint8_t whole[64];
    expected(whole);
    for (size_t offset = 0U; offset <= sizeof(whole); ++offset) {
        for (size_t size = 0U; offset + size <= sizeof(whole); ++size) {
            uint8_t buffer[66];
            memset(buffer, 0xa5, sizeof(buffer));
            assert(jetkvm_builtin_image_read(&image, offset, buffer + 1, size));
            assert(memcmp(buffer + 1, whole + offset, size) == 0);
            /* Nothing is written outside the range. */
            assert(buffer[0] == 0xa5 && buffer[1 + size] == 0xa5);
        }
    }
}

static void test_bytes_outside_the_runs_are_zero(void)
{
    uint8_t buffer[16];
    memset(buffer, 0xff, sizeof(buffer));
    assert(jetkvm_builtin_image_read(&image, 30U, buffer, sizeof(buffer)));
    for (size_t i = 0U; i < sizeof(buffer); ++i) {
        assert(buffer[i] == 0U);
    }
}

static void test_a_range_past_the_end_is_refused(void)
{
    uint8_t buffer[8];
    memset(buffer, 0xa5, sizeof(buffer));
    assert(!jetkvm_builtin_image_read(&image, 60U, buffer, 5U));
    assert(!jetkvm_builtin_image_read(&image, 65U, buffer, 0U));
    assert(!jetkvm_builtin_image_read(&image, UINT64_MAX, buffer, 1U));
    assert(!jetkvm_builtin_image_read(&image, 1U, buffer, SIZE_MAX));
    for (size_t i = 0U; i < sizeof(buffer); ++i) {
        assert(buffer[i] == 0xa5);
    }
    assert(jetkvm_builtin_image_read(&image, 64U, buffer, 0U));
    assert(!jetkvm_builtin_image_read(&image, 0U, NULL, 1U));
    assert(!jetkvm_builtin_image_read(NULL, 0U, buffer, 1U));
}

int main(void)
{
    test_every_range_matches_the_whole_image();
    test_bytes_outside_the_runs_are_zero();
    test_a_range_past_the_end_is_refused();
    puts("built-in image reads: OK");
    return 0;
}
