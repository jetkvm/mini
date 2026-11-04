/* Writes the built-in installer disc to stdout through the firmware's read
 * path, in reads of changing sizes and alignments, as a host reads it. */
#include <stdio.h>

#include "builtin_image.h"

int main(void)
{
    static uint8_t buffer[16384];
    static const size_t sizes[] = { 512U, 2048U, 16384U, 1U, 4095U, 7U };
    const jetkvm_builtin_image_t *image = &jetkvm_builtin_installer_image;
    uint64_t offset = 0U;
    for (size_t i = 0U; offset < image->size; ++i) {
        size_t size = sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];
        if (size > image->size - offset) {
            size = (size_t)(image->size - offset);
        }
        if (!jetkvm_builtin_image_read(image, offset, buffer, size) ||
            fwrite(buffer, 1U, size, stdout) != size) {
            return 1;
        }
        offset += size;
    }
    return 0;
}
