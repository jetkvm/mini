#include "jetkvm_video_quality.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>

int main(void)
{
    /* Auto is the default, as in upstream new configurations. */
    assert(jetkvm_video_quality_get_permille() ==
           JETKVM_VIDEO_QUALITY_AUTO_PERMILLE);
    assert(jetkvm_video_quality_get() == 0.0);
    const uint32_t initial_revision = jetkvm_video_quality_revision();

    assert(jetkvm_video_quality_set(0.5));
    assert(jetkvm_video_quality_get_permille() == 500U);
    assert(jetkvm_video_quality_revision() == initial_revision + 1U);

    assert(jetkvm_video_quality_set(0.5004));
    assert(jetkvm_video_quality_get_permille() == 500U);
    assert(jetkvm_video_quality_revision() == initial_revision + 1U);
    assert(jetkvm_video_quality_set(0.5006));
    assert(jetkvm_video_quality_get_permille() == 501U);
    assert(jetkvm_video_quality_revision() == initial_revision + 2U);

    /* Only exact 0 is Auto; the gap below the manual floor stays invalid. */
    assert(!jetkvm_video_quality_set(0.05));
    assert(!jetkvm_video_quality_set(0.099));
    assert(!jetkvm_video_quality_set(-0.0001));
    assert(!jetkvm_video_quality_set(1.001));
    assert(!jetkvm_video_quality_set(NAN));
    assert(jetkvm_video_quality_get_permille() == 501U);

    assert(jetkvm_video_quality_set(0.0));
    assert(jetkvm_video_quality_get_permille() ==
           JETKVM_VIDEO_QUALITY_AUTO_PERMILLE);
    assert(jetkvm_video_quality_get() == 0.0);
    assert(jetkvm_video_quality_revision() == initial_revision + 3U);

    assert(jetkvm_video_quality_set(1.0));
    assert(jetkvm_video_quality_get_permille() == 1000U);
    return 0;
}
