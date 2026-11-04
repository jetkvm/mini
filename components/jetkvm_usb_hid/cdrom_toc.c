#include "cdrom_toc.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ADR 1 (Q sub-channel has position data), control 4 (data track). */
#define TOC_ADR_CONTROL_DATA 0x14U
#define TOC_LEAD_OUT 0xAAU
#define TOC_FULL_DESCRIPTOR_BYTES 11U
/* Header and the A0, A1, A2 and track 1 full-TOC descriptors. */
#define TOC_MAX_RESPONSE_BYTES (4U + 4U * TOC_FULL_DESCRIPTOR_BYTES)

/* An MSF address counts 75 frames per second from the start of the
 * two-second pre-gap. The minute field holds at most 255, so an image of
 * more than about 2.2 GB reports the largest address and not one that wraps
 * to a small value. */
static void store_address(uint8_t dest[4], bool msf, uint32_t lba)
{
    if (msf) {
        const uint64_t max_frames = (255U * 60U + 59U) * 75U + 74U;
        uint64_t frames = (uint64_t)lba + 150U;
        if (frames > max_frames) {
            frames = max_frames;
        }
        dest[0] = 0U;
        dest[1] = (uint8_t)(frames / (60U * 75U));
        dest[2] = (uint8_t)((frames / 75U) % 60U);
        dest[3] = (uint8_t)(frames % 75U);
    } else {
        dest[0] = (uint8_t)(lba >> 24);
        dest[1] = (uint8_t)(lba >> 16);
        dest[2] = (uint8_t)(lba >> 8);
        dest[3] = (uint8_t)lba;
    }
}

int32_t jetkvm_cdrom_read_toc(const uint8_t cdb[10], uint32_t block_count,
                              uint8_t *out, uint32_t out_size)
{
    const bool msf = (cdb[1] & 0x02U) != 0U;
    const uint8_t track_or_session = cdb[6];
    const uint32_t allocation = ((uint32_t)cdb[7] << 8) | cdb[8];
    uint8_t format = cdb[2] & 0x0fU;
    if (format == 0U) {
        format = (uint8_t)(cdb[9] >> 6);
    }

    uint8_t response[TOC_MAX_RESPONSE_BYTES] = {0};
    size_t length = 4U;
    response[2] = 1U; /* first track (formats 0, 1) or session (format 2) */
    response[3] = 1U; /* last track or session */

    switch (format) {
    case 0U: /* TOC: track 1 and the lead-out, or only the lead-out */
        if (track_or_session > 1U && track_or_session != TOC_LEAD_OUT) {
            return -1;
        }
        if (track_or_session <= 1U) {
            response[length + 1U] = TOC_ADR_CONTROL_DATA;
            response[length + 2U] = 1U;
            store_address(&response[length + 4U], msf, 0U);
            length += 8U;
        }
        response[length + 1U] = TOC_ADR_CONTROL_DATA;
        response[length + 2U] = TOC_LEAD_OUT;
        store_address(&response[length + 4U], msf, block_count);
        length += 8U;
        break;
    case 1U: /* Session information: the first track of the last session */
        response[length + 1U] = TOC_ADR_CONTROL_DATA;
        response[length + 2U] = 1U;
        store_address(&response[length + 4U], msf, 0U);
        length += 8U;
        break;
    case 2U: { /* Full TOC of session 1: points A0, A1, A2 and track 1 */
        if (track_or_session > 1U) {
            return -1;
        }
        static const uint8_t points[] = {0xa0U, 0xa1U, 0xa2U, 0x01U};
        for (size_t i = 0U; i < sizeof(points); ++i) {
            uint8_t *descriptor = &response[length];
            descriptor[0] = 1U; /* session */
            descriptor[1] = TOC_ADR_CONTROL_DATA;
            descriptor[3] = points[i];
            if (points[i] == 0xa0U || points[i] == 0xa1U) {
                /* PMIN: the first (A0) or last (A1) track. A0's PSEC 0
                 * is the CD-DA or CD-ROM disc type. */
                descriptor[8] = 1U;
            } else {
                /* ZERO, PMIN, PSEC, PFRAME: the lead-out (A2) or track
                 * start, as Linux f_mass_storage stores them. */
                store_address(&descriptor[7], msf,
                              points[i] == 0xa2U ? block_count : 0U);
            }
            length += TOC_FULL_DESCRIPTOR_BYTES;
        }
        break;
    }
    default: /* PMA, ATIP and CD-TEXT do not exist on a pressed CD image */
        return -1;
    }

    response[0] = (uint8_t)((length - 2U) >> 8);
    response[1] = (uint8_t)(length - 2U);
    uint32_t copied = (uint32_t)length;
    if (copied > allocation) {
        copied = allocation;
    }
    if (copied > out_size) {
        copied = out_size;
    }
    memcpy(out, response, copied);
    return (int32_t)copied;
}
