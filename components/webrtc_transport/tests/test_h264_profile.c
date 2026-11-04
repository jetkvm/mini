#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "h264_profile.h"

/* The video section of a Firefox 158 offer with OpenH264, shortened. */
static const char firefox_offer[] =
    "v=0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 120 124 121 125 126 127 97 98 105 106 103 104 99 100\r\n"
    "a=recvonly\r\n"
    "a=fmtp:126 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1\r\n"
    "a=fmtp:97 profile-level-id=42e01f;level-asymmetry-allowed=1\r\n"
    "a=fmtp:105 profile-level-id=42001f;level-asymmetry-allowed=1;packetization-mode=1\r\n"
    "a=fmtp:103 profile-level-id=42001f;level-asymmetry-allowed=1\r\n"
    "a=fmtp:99 profile=0;level-idx=9;tier=0\r\n"
    "a=fmtp:120 max-fs=12288;max-fr=60\r\n"
    "a=fmtp:127 apt=126\r\n"
    "a=rtpmap:126 H264/90000\r\n"
    "a=rtpmap:97 H264/90000\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 109\r\n"
    "a=fmtp:109 profile-level-id=aaaaaa\r\n"
    "a=rtpmap:109 opus/48000/2\r\n";

/* The Mini's answer to that offer, from esp_peer 1.5.5. */
static const char mini_answer[] =
    "v=0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 126\r\n"
    "a=rtpmap:126 H264/90000\r\n"
    "a=fmtp:126 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n"
    "a=sendonly\r\n"
    "a=mid:0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 109\r\n"
    "a=rtpmap:109 opus/48000/2\r\n";

/* Chrome offers Main for some payload types; esp_peer's answer is right there. */
static const char chrome_offer[] =
    "m=video 9 UDP/TLS/RTP/SAVPF 102 39\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
    "a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n";

static const char chrome_answer[] =
    "m=video 9 UDP/TLS/RTP/SAVPF 39\r\n"
    "a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n";

int main(void)
{
    webrtc_h264_offer_profiles_t profiles;
    webrtc_h264_offer_profiles_parse(firefox_offer, strlen(firefox_offer), &profiles);
    /* 126, 97, 105, 103; AV1, VP8, RTX and the audio section have none. */
    assert(profiles.count == 4);
    assert(profiles.payloads[0].payload_type == 126);
    assert(memcmp(profiles.payloads[0].profile_level_id, "42e01f", 6) == 0);
    assert(profiles.payloads[2].payload_type == 105);
    assert(memcmp(profiles.payloads[2].profile_level_id, "42001f", 6) == 0);

    char answer[sizeof(mini_answer)];
    memcpy(answer, mini_answer, sizeof(answer));
    assert(webrtc_h264_answer_use_offer_profiles(answer, strlen(answer), &profiles));
    assert(strlen(answer) == strlen(mini_answer));
    assert(strstr(answer,
                  "a=fmtp:126 level-asymmetry-allowed=1;packetization-mode=1;"
                  "profile-level-id=42e01f\r\n"));
    assert(!strstr(answer, "4d001f"));
    /* A second pass has nothing to change. */
    assert(!webrtc_h264_answer_use_offer_profiles(answer, strlen(answer), &profiles));

    webrtc_h264_offer_profiles_parse(chrome_offer, strlen(chrome_offer), &profiles);
    assert(profiles.count == 2);
    char chrome[sizeof(chrome_answer)];
    memcpy(chrome, chrome_answer, sizeof(chrome));
    assert(!webrtc_h264_answer_use_offer_profiles(chrome, strlen(chrome), &profiles));
    assert(strcmp(chrome, chrome_answer) == 0);

    /* A payload type that the offer did not give a profile for stays. */
    webrtc_h264_offer_profiles_parse("m=video 9 X 126\r\n", 17, &profiles);
    assert(profiles.count == 0);
    memcpy(answer, mini_answer, sizeof(answer));
    assert(!webrtc_h264_answer_use_offer_profiles(answer, strlen(answer), &profiles));
    assert(strcmp(answer, mini_answer) == 0);

    /* A short or truncated value is not a profile-level-id. */
    const char truncated[] = "m=video 9 X 126\r\na=fmtp:126 profile-level-id=42e0";
    webrtc_h264_offer_profiles_parse(truncated, strlen(truncated), &profiles);
    assert(profiles.count == 0);

    puts("h264 profile tests passed");
    return 0;
}
