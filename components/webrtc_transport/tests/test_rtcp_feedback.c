#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp_feedback.h"

int main(void)
{
    const uint8_t pli[] = {0x81, 206, 0, 2, 0, 0, 0, 7, 0, 0, 0, 1};
    assert(webrtc_rtcp_has_video_pli(pli, sizeof(pli), 1));
    assert(!webrtc_rtcp_has_video_pli(pli, sizeof(pli), 2));
    assert(!webrtc_rtcp_has_video_pli(NULL, 0, 1));
    for (size_t size = 0; size < sizeof(pli); size++) {
        assert(!webrtc_rtcp_has_video_pli(pli, size, 1));
    }
    uint8_t compound[20] = {0x80, 201, 0, 1, 0, 0, 0, 7};
    memcpy(compound + 8, pli, sizeof(pli));
    assert(webrtc_rtcp_has_video_pli(compound, sizeof(compound), 1));
    compound[0] = 0x40;  /* Wrong RTCP version. */
    assert(!webrtc_rtcp_has_video_pli(compound, sizeof(compound), 1));
    uint8_t padded[16];
    memcpy(padded, pli, sizeof(pli));
    padded[0] |= 0x20;
    padded[3] = 3;
    memset(padded + 12, 0, 4);
    padded[15] = 4;
    assert(webrtc_rtcp_has_video_pli(padded, sizeof(padded), 1));
    padded[15] = 0;
    assert(!webrtc_rtcp_has_video_pli(padded, sizeof(padded), 1));
    padded[15] = 13;
    assert(!webrtc_rtcp_has_video_pli(padded, sizeof(padded), 1));
    uint8_t malformed_tail[16];
    memcpy(malformed_tail, pli, sizeof(pli));
    memset(malformed_tail + 12, 0, 4);
    assert(!webrtc_rtcp_has_video_pli(malformed_tail, sizeof(malformed_tail), 1));
    uint8_t other[12];
    memcpy(other, pli, sizeof(pli));
    other[0] = 0x82; /* SLI, not PLI. */
    assert(!webrtc_rtcp_has_video_pli(other, sizeof(other), 1));
    other[0] = 0x81;
    other[1] = 205; /* Transport feedback, not payload feedback. */
    assert(!webrtc_rtcp_has_video_pli(other, sizeof(other), 1));
    /* Compound RR (other source first, then video) + PLI. */
    uint8_t rr[8 + 2 * 24 + sizeof(pli)] = {0x82, 201, 0, 13, 0, 0, 0, 9};
    rr[8 + 3] = 5;                                     /* block 0: SSRC 5 */
    rr[8 + 4] = 99;
    uint8_t *video = rr + 8 + 24;
    video[3] = 1;                                      /* block 1: SSRC 1 */
    video[4] = 13;                                     /* 13/256 lost */
    video[5] = 0xff; video[6] = 0xff; video[7] = 0xfe; /* cumulative -2 */
    video[9] = 1; video[10] = 0x86; video[11] = 0xa0;  /* seq 0x186a0 */
    memcpy(rr + 8 + 48, pli, sizeof(pli));
    webrtc_rtcp_report_block_t block = {0};
    assert(webrtc_rtcp_find_report_block(rr, sizeof(rr), 1, &block));
    assert(block.fraction_lost == 13 && block.cumulative_lost == -2 &&
           block.highest_sequence == 0x186a0);
    assert(!webrtc_rtcp_find_report_block(rr, sizeof(rr), 2, &block));
    video[7] = 0x40; video[6] = 0; video[5] = 0;
    assert(webrtc_rtcp_find_report_block(rr, sizeof(rr), 1, &block) && block.cumulative_lost == 64);
    /* SR: blocks start after the 20-byte sender info. */
    uint8_t sr[28 + 24] = {0x81, 200, 0, 12, 0, 0, 0, 9};
    sr[28 + 3] = 1; sr[28 + 4] = 7; sr[28 + 11] = 42;
    assert(webrtc_rtcp_find_report_block(sr, sizeof(sr), 1, &block) &&
           block.fraction_lost == 7 && block.highest_sequence == 42);
    /* A report count that overruns the packet length is malformed. */
    uint8_t short_rr[8 + 24] = {0x82, 201, 0, 7, 0, 0, 0, 9};
    short_rr[8 + 3] = 1;
    assert(!webrtc_rtcp_find_report_block(short_rr, sizeof(short_rr), 1, &block));
    for (size_t size = 1; size < sizeof(sr); size++) {
        assert(!webrtc_rtcp_find_report_block(sr, size, 1, &block));
    }
    puts("authenticated RTCP PLI and report block parser: OK");
    return 0;
}
