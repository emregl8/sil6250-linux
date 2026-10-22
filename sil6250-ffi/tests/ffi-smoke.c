/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <assert.h>
#include <math.h>
#include <sil6250_ffi.h>

int
main(void)
{
    const uint8_t image[SIL6250_IMAGE_SIZE] = {0};
    const uint8_t malformed[] = {0xff, 0xff, 0xff, 0xff};
    uint8_t output[] = {0xa5, 0xa5};
    float quality = sil6250_frame_quality(image, sizeof image);

    assert(isfinite(quality) && quality >= 0.0f && quality <= 1.0f);
    assert(!sil6250_engine_set_control(NULL, 1000, NULL, NULL));
    assert(!sil6250_gallery_valid(NULL, 0));
    assert(!sil6250_gallery_valid(malformed, sizeof malformed));
    assert(sil6250_frame_matches(image, sizeof image, malformed, sizeof malformed, 5) == -1);
    assert(sil6250_frame_matches(image, sizeof image, malformed, sizeof malformed, 0) == -1);
    assert(sil6250_frame_matches(NULL, sizeof image, malformed, sizeof malformed, 5) == -1);
    assert(sil6250_frame_extract(image, sizeof image, output, 0) == -1);
    assert(output[0] == 0xa5 && output[1] == 0xa5);
    Sil6250Probe *probe = sil6250_probe_new(image, sizeof image);
    assert(probe != NULL);
    assert(sil6250_gallery_new(malformed, sizeof malformed) == NULL);
    assert(sil6250_probe_matches(probe, NULL, 5) == -1);
    sil6250_probe_free(probe);
    sil6250_probe_free(NULL);
    sil6250_gallery_free(NULL);
    return 0;
}
