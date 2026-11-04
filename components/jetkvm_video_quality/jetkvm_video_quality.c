#include "jetkvm_video_quality.h"

#include <stddef.h>

static uint32_t s_quality_permille = JETKVM_VIDEO_QUALITY_DEFAULT_PERMILLE;
static uint32_t s_revision = 1U;

static bool normalize_factor(double factor, uint32_t *permille)
{
    if (permille == NULL || factor != factor) {
        return false;
    }
    if (factor == 0.0) {
        *permille = JETKVM_VIDEO_QUALITY_AUTO_PERMILLE;
        return true;
    }
    if (factor < 0.1 || factor > 1.0) {
        return false;
    }
    const double scaled = factor * 1000.0;
    if (scaled < (double)JETKVM_VIDEO_QUALITY_MIN_PERMILLE ||
        scaled > (double)JETKVM_VIDEO_QUALITY_MAX_PERMILLE) {
        return false;
    }
    const uint32_t rounded = (uint32_t)(scaled + 0.5);
    if (rounded < JETKVM_VIDEO_QUALITY_MIN_PERMILLE ||
        rounded > JETKVM_VIDEO_QUALITY_MAX_PERMILLE) {
        return false;
    }
    *permille = rounded;
    return true;
}

bool jetkvm_video_quality_set(double factor)
{
    uint32_t permille;
    if (!normalize_factor(factor, &permille)) {
        return false;
    }
    const uint32_t previous = __atomic_exchange_n(
        &s_quality_permille, permille, __ATOMIC_ACQ_REL);
    if (previous != permille) {
        uint32_t revision = __atomic_add_fetch(&s_revision, 1U,
                                               __ATOMIC_ACQ_REL);
        if (revision == 0U) {
            __atomic_store_n(&s_revision, 1U, __ATOMIC_RELEASE);
        }
    }
    return true;
}

double jetkvm_video_quality_get(void)
{
    return (double)jetkvm_video_quality_get_permille() / 1000.0;
}

uint32_t jetkvm_video_quality_get_permille(void)
{
    return __atomic_load_n(&s_quality_permille, __ATOMIC_ACQUIRE);
}

uint32_t jetkvm_video_quality_revision(void)
{
    return __atomic_load_n(&s_revision, __ATOMIC_ACQUIRE);
}
