#include "cdrom_toc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* 92,807,168-byte OS Services installer ISO: 45,316 blocks of 2048 bytes.
 * Lead-out LBA 45316 is MSF 10:06:16 (45316 + 150 = 45466 frames). */
#define ISO_BLOCKS 45316U
#define SENTINEL 0xeeU

static void expect_toc(const uint8_t cdb[10], uint32_t block_count,
                       uint32_t out_size, const uint8_t *expected,
                       size_t expected_size)
{
    uint8_t out[128];
    memset(out, SENTINEL, sizeof(out));
    assert(out_size <= sizeof(out));
    int32_t length = jetkvm_cdrom_read_toc(cdb, block_count, out, out_size);
    assert(length == (int32_t)expected_size);
    assert(memcmp(out, expected, expected_size) == 0);
    for (size_t i = expected_size; i < sizeof(out); ++i) {
        assert(out[i] == SENTINEL);
    }
}

static void expect_rejected(const uint8_t cdb[10])
{
    uint8_t out[64];
    assert(jetkvm_cdrom_read_toc(cdb, ISO_BLOCKS, out, sizeof(out)) == -1);
}

int main(void)
{
    /* Format 0, LBA addresses: track 1 at 0, lead-out at 45316 (0xb104). */
    const uint8_t toc_lba_cdb[10] = {0x43, 0, 0, 0, 0, 0, 0, 0xff, 0xfe, 0};
    const uint8_t toc_lba[] = {
        0x00, 0x12, 0x01, 0x01,
        0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x14, 0xaa, 0x00, 0x00, 0x00, 0xb1, 0x04,
    };
    expect_toc(toc_lba_cdb, ISO_BLOCKS, 128U, toc_lba, sizeof(toc_lba));

    /* Format 0, MSF addresses: track 1 at 00:02:00, lead-out at 10:06:16. */
    const uint8_t toc_msf_cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 1, 0xff, 0xfe, 0};
    const uint8_t toc_msf[] = {
        0x00, 0x12, 0x01, 0x01,
        0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x14, 0xaa, 0x00, 0x00, 0x0a, 0x06, 0x10,
    };
    expect_toc(toc_msf_cdb, ISO_BLOCKS, 128U, toc_msf, sizeof(toc_msf));

    /* macOS finds CD media with a 4-byte format 0 request. */
    const uint8_t header_cdb[10] = {0x43, 0, 0, 0, 0, 0, 0, 0x00, 0x04, 0};
    expect_toc(header_cdb, ISO_BLOCKS, 128U, toc_lba, 4U);

    /* The device buffer limits the response below the allocation length. */
    expect_toc(toc_lba_cdb, ISO_BLOCKS, 10U, toc_lba, 10U);

    /* Track 0xaa returns only the lead-out. */
    const uint8_t lead_out_cdb[10] = {0x43, 0, 0, 0, 0, 0, 0xaa, 0, 12, 0};
    const uint8_t lead_out[] = {
        0x00, 0x0a, 0x01, 0x01,
        0x00, 0x14, 0xaa, 0x00, 0x00, 0x00, 0xb1, 0x04,
    };
    expect_toc(lead_out_cdb, ISO_BLOCKS, 128U, lead_out, sizeof(lead_out));

    /* Linux sr multisession probe: format 1 in byte 9 bits 7-6. */
    const uint8_t session_cdb[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 12, 0x40};
    const uint8_t session[] = {
        0x00, 0x0a, 0x01, 0x01,
        0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    expect_toc(session_cdb, ISO_BLOCKS, 128U, session, sizeof(session));

    /* macOS full TOC request: MSF, format 2 in byte 9 bits 7-6, 0xfffe. */
    const uint8_t full_cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 0, 0xff, 0xfe, 0x80};
    const uint8_t full[] = {
        0x00, 0x2e, 0x01, 0x01,
        0x01, 0x14, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x14, 0x00, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x14, 0x00, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x06, 0x10,
        0x01, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    };
    expect_toc(full_cdb, ISO_BLOCKS, 128U, full, sizeof(full));

    /* The MMC format field in byte 2 selects the same full TOC, and it
     * takes precedence over byte 9. */
    const uint8_t full_mmc_cdb[10] = {0x43, 0x02, 0x02, 0, 0, 0, 0, 0xff, 0xfe, 0x40};
    expect_toc(full_mmc_cdb, ISO_BLOCKS, 128U, full, sizeof(full));

    /* Full TOC with LBA addresses: A2 and track 1 store the LBA in the
     * ZERO, PMIN, PSEC and PFRAME bytes. */
    const uint8_t full_lba_cdb[10] = {0x43, 0, 0x02, 0, 0, 0, 0, 0, 0x30, 0};
    const uint8_t full_lba[] = {
        0x00, 0x2e, 0x01, 0x01,
        0x01, 0x14, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x14, 0x00, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x14, 0x00, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb1, 0x04,
        0x01, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    expect_toc(full_lba_cdb, ISO_BLOCKS, 128U, full_lba, sizeof(full_lba));

    /* An 8 GB image is past 255:59:74; the MSF lead-out stops there. */
    const uint8_t big_cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 0xaa, 0, 12, 0};
    const uint8_t big[] = {
        0x00, 0x0a, 0x01, 0x01,
        0x00, 0x14, 0xaa, 0x00, 0x00, 0xff, 0x3b, 0x4a,
    };
    expect_toc(big_cdb, 4000000U, 128U, big, sizeof(big));

    /* There is no track 2 and no session 2, and no PMA (format 3). */
    const uint8_t track_two_cdb[10] = {0x43, 0, 0, 0, 0, 0, 2, 0, 20, 0};
    expect_rejected(track_two_cdb);
    const uint8_t session_two_cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 2, 0, 48, 0x80};
    expect_rejected(session_two_cdb);
    const uint8_t pma_cdb[10] = {0x43, 0x02, 0x03, 0, 0, 0, 0, 0, 48, 0};
    expect_rejected(pma_cdb);

    return 0;
}
