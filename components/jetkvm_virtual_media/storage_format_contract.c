/* SPDX-License-Identifier: MIT */

#include "storage_format_contract.h"

jetkvm_virtual_media_format_result_t jetkvm_storage_format_admit(
    const jetkvm_storage_format_inputs_t *inputs)
{
    if (!inputs->initialized) {
        return JETKVM_VIRTUAL_MEDIA_FORMAT_NOT_READY;
    }
    if (!inputs->card_present) {
        return JETKVM_VIRTUAL_MEDIA_FORMAT_NO_CARD;
    }
    if (inputs->image_mounted) {
        return JETKVM_VIRTUAL_MEDIA_FORMAT_IMAGE_MOUNTED;
    }
    if (inputs->upload_has_writer || inputs->upload_close_failed) {
        return JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE;
    }
    return JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED;
}
