/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_wire.h"


bool osvc_utf8_valid(const uint8_t *text, size_t length)
{
    size_t i = 0U;
    while (i < length) {
        const uint8_t lead = text[i];
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        size_t extra;
        uint32_t code;
        uint32_t minimum;
        if ((lead & 0xe0U) == 0xc0U) {
            extra = 1U;
            code = lead & 0x1fU;
            minimum = 0x80U;
        } else if ((lead & 0xf0U) == 0xe0U) {
            extra = 2U;
            code = lead & 0x0fU;
            minimum = 0x800U;
        } else if ((lead & 0xf8U) == 0xf0U) {
            extra = 3U;
            code = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (length - i <= extra) {
            return false;
        }
        for (size_t k = 1U; k <= extra; ++k) {
            const uint8_t next = text[i + k];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code = (code << 6) | (next & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU)) {
            return false;
        }
        i += extra + 1U;
    }
    return true;
}

bool osvc_file_name_valid(const uint8_t *name, size_t length)
{
    if (length == 0U || !osvc_utf8_valid(name, length)) {
        return false;
    }
    if ((length == 1U && name[0] == '.') ||
        (length == 2U && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        const uint8_t byte = name[i];
        if (byte < 0x20U || byte == 0x7fU || byte == '/' || byte == '\\') {
            return false;
        }
        /* U+0080 to U+009F: in valid UTF-8, 0xc2 is only ever a lead byte. */
        if (byte == 0xc2U && name[i + 1U] <= 0x9fU) {
            return false;
        }
    }
    return true;
}

void osvc_format_fingerprint(const uint8_t digest[OSVC_FINGERPRINT_LEN],
                             char out[OSVC_FINGERPRINT_TEXT_CAPACITY])
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0U;
    for (size_t i = 0U; i < OSVC_FINGERPRINT_LEN; ++i) {
        if (i > 0U) {
            out[o++] = ':';
        }
        out[o++] = hex[digest[i] >> 4];
        out[o++] = hex[digest[i] & 0x0fU];
    }
    out[o] = '\0';
}

/*
 * Calls visit for the first byte of each NAL unit after a 3- or 4-byte start
 * code, until visit returns false. Returns false when the walk ended without
 * visit stopping it.
 */
typedef bool (*nal_visit_fn)(uint8_t header, void *state);

static bool walk_nal_units(const uint8_t *data, size_t length,
                           nal_visit_fn visit, void *state)
{
    size_t i = 0U;
    while (i + 3U < length) {
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            i += 3U;
        } else if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 0U &&
                   i + 4U < length && data[i + 3U] == 1U) {
            i += 4U;
        } else {
            ++i;
            continue;
        }
        if (i >= length) {
            return false;
        }
        if (!visit(data[i], state)) {
            return true;
        }
    }
    return false;
}

typedef struct {
    osvc_codec_t codec;
    bool keyframe;
} keyframe_state_t;

static bool visit_keyframe(uint8_t header, void *opaque)
{
    keyframe_state_t *state = opaque;
    if (state->codec == OSVC_CODEC_H265) {
        const uint8_t type = (uint8_t)((header >> 1) & 0x3fU);
        if (type <= 31U) {
            state->keyframe = type >= 19U && type <= 21U;
            return false;
        }
        return true;
    }
    const uint8_t type = header & 0x1fU;
    if (type >= 1U && type <= 5U) {
        state->keyframe = type == 5U;
        return false;
    }
    return true;
}

bool osvc_annexb_is_keyframe(osvc_codec_t codec, const uint8_t *data,
                             size_t length)
{
    keyframe_state_t state = { .codec = codec, .keyframe = false };
    (void)walk_nal_units(data, length, visit_keyframe, &state);
    return state.keyframe;
}

typedef struct {
    osvc_codec_t codec;
    bool vps;
    bool sps;
    bool pps;
} parameter_set_state_t;

static bool visit_parameter_sets(uint8_t header, void *opaque)
{
    parameter_set_state_t *state = opaque;
    if (state->codec == OSVC_CODEC_H265) {
        switch ((header >> 1) & 0x3fU) {
        case 32U:
            state->vps = true;
            break;
        case 33U:
            state->sps = true;
            break;
        case 34U:
            state->pps = true;
            break;
        default:
            break;
        }
        return !(state->vps && state->sps && state->pps);
    }
    switch (header & 0x1fU) {
    case 7U:
        state->sps = true;
        break;
    case 8U:
        state->pps = true;
        break;
    default:
        break;
    }
    return !(state->sps && state->pps);
}

bool osvc_annexb_has_parameter_sets(osvc_codec_t codec, const uint8_t *data,
                                    size_t length)
{
    parameter_set_state_t state = { .codec = codec };
    (void)walk_nal_units(data, length, visit_parameter_sets, &state);
    return codec == OSVC_CODEC_H265 ? state.vps && state.sps && state.pps :
                                      state.sps && state.pps;
}
