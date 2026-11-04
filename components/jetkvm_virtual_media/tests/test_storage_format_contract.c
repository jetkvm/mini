#include <assert.h>
#include <stdio.h>

#include "storage_format_contract.h"

/* A started card with nothing open. */
static jetkvm_storage_format_inputs_t idle(void)
{
    return (jetkvm_storage_format_inputs_t){
        .initialized = true,
        .card_present = true,
    };
}

static void test_idle_card_is_formatted(void)
{
    jetkvm_storage_format_inputs_t inputs = idle();
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED);
}

static void test_each_refusal(void)
{
    jetkvm_storage_format_inputs_t inputs = idle();
    inputs.initialized = false;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_NOT_READY);

    inputs = idle();
    inputs.card_present = false;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_NO_CARD);

    inputs = idle();
    inputs.image_mounted = true;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_IMAGE_MOUNTED);

    inputs = idle();
    inputs.upload_has_writer = true;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE);

    inputs = idle();
    inputs.upload_close_failed = true;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE);
}

/* The reason given is the one the user acts on first: a missing card
 * before what is open on it, a mounted image before an upload. */
static void test_reason_order(void)
{
    jetkvm_storage_format_inputs_t inputs = {
        .initialized = true,
        .card_present = false,
        .image_mounted = true,
        .upload_has_writer = true,
    };
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_NO_CARD);
    inputs.card_present = true;
    assert(jetkvm_storage_format_admit(&inputs) ==
           JETKVM_VIRTUAL_MEDIA_FORMAT_IMAGE_MOUNTED);
}

int main(void)
{
    test_idle_card_is_formatted();
    test_each_refusal();
    test_reason_order();
    puts("storage format contract tests passed");
    return 0;
}
