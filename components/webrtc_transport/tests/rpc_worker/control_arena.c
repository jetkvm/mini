/* The worker's control arena for host tests. A job (with its input) and a
 * reply buffer are one block each: a test that ends with control_arena_blocks
 * at zero freed every one of them, and ASan reports a second free. New blocks
 * hold no zeros, as a reused heap block would not. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"
#include "jetkvm_memory_policy.h"

size_t control_arena_blocks;
size_t control_arena_fail_size;

void *jetkvm_memory_policy_control_alloc(size_t size)
{
    if (size == 0 || size == control_arena_fail_size) return NULL;
    void *memory = malloc(size);
    if (!memory) return NULL;
    memset(memory, 0xA5, size);
    ++control_arena_blocks;
    return memory;
}

void jetkvm_memory_policy_control_free(void *memory)
{
    if (!memory) return;
    assert(control_arena_blocks > 0);
    --control_arena_blocks;
    free(memory);
}

/* The worker's semaphore and tick count (compat/freertos/semphr.h). */
void (*semaphore_wait_hook)(void);
static TickType_t ticks;

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer)
{
    buffer->token = 0;
    return buffer;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->token = 1;
    return pdTRUE;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    if (!semaphore->token && semaphore_wait_hook) {
        void (*hook)(void) = semaphore_wait_hook;
        semaphore_wait_hook = NULL;
        hook();
    }
    if (semaphore->token) {
        semaphore->token = 0;
        return pdTRUE;
    }
    ticks += wait;
    return pdFALSE;
}

TickType_t xTaskGetTickCount(void) { return ticks; }
