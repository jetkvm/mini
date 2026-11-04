#!/usr/bin/env python3
"""The SDIO bus lock of ESP-Hosted's host driver passes to a task that waits.

Compiles sdio_bus_take() from the patch against pthread stand-ins for the
FreeRTOS calls and runs a read task and a write task that take the bus back
to back. While one waits, the other may take the bus at most twice more.
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp_hosted-2.12.3-sdio-bus-handoff.patch"
PATCH = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

assert f"apply_managed_component_patch(patches/{PATCH_NAME})" in CMAKE

added = [line[1:] for line in PATCH.splitlines()
         if line.startswith("+") and not line.startswith("+++")]
source = "\n".join(added)
start = source.index("enum { SDIO_SIDE_RX")
end = source.index("#define SDIO_DRV_LOCK()")
take = source[start:end]

# Only the write task takes the bus as the write side.
assert "+\t\tSDIO_DRV_LOCK_TX();" in PATCH
assert "-\t\tSDIO_DRV_LOCK();" in PATCH
assert PATCH.count("SDIO_DRV_LOCK_TX();") == 1

HARNESS = r"""
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define pdTRUE 1
#define pdFALSE 0
#define HOSTED_BLOCK_MAX (-1)
/* One tick here is long enough that only a stuck task can run out a turn. */
#define TICK_MS 100

typedef struct { pthread_mutex_t m; pthread_cond_t c; int given; } sem_t_;
typedef sem_t_ *SemaphoreHandle_t;

static __attribute__((unused)) int xSemaphoreTake(SemaphoreHandle_t s, int ticks)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    long long ns = until.tv_nsec + (long long)ticks * TICK_MS * 1000000LL;
    until.tv_sec += ns / 1000000000LL;
    until.tv_nsec = ns % 1000000000LL;
    pthread_mutex_lock(&s->m);
    int rc = 0;
    while (!s->given && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&s->c, &s->m, &until);
    int ok = s->given;
    s->given = 0;
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

static __attribute__((unused)) int xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    s->given = 1;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
static void *sdio_bus_lock = &bus;
static volatile uint32_t taken[2];
static __thread int self_side;
static uint32_t most_taken_while_waiting[2];

static int lock_mutex(void *m, int timeout)
{
    (void)timeout;
    const int other = !self_side;
    const uint32_t other_before = __atomic_load_n(&taken[other], __ATOMIC_SEQ_CST);
    pthread_mutex_lock((pthread_mutex_t *)m);
    const uint32_t passed = __atomic_load_n(&taken[other], __ATOMIC_SEQ_CST) - other_before;
    if (passed > most_taken_while_waiting[self_side])
        most_taken_while_waiting[self_side] = passed;
    __atomic_add_fetch(&taken[self_side], 1, __ATOMIC_SEQ_CST);
    return 0;
}

static int unlock_mutex(void *m)
{
    return pthread_mutex_unlock((pthread_mutex_t *)m);
}

static struct { int (*_h_lock_mutex)(void *, int); int (*_h_unlock_mutex)(void *); } funcs = {
    lock_mutex, unlock_mutex,
};
static struct { typeof(funcs) *funcs; } g_h = { &funcs };

TAKE_SOURCE

static void spin_us(int us)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000 < us);
}

static volatile int stop;
static int write_rounds;
static int write_pause_us;

/* Without the handoff the write task may not get the bus for minutes. */
static void *reader(void *arg)
{
    (void)arg;
    self_side = SDIO_SIDE_RX;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec > 10)
            break;
        sdio_bus_take(SDIO_SIDE_RX);
        spin_us(30);
        unlock_mutex(sdio_bus_lock);
    }
    return NULL;
}

static void *writer(void *arg)
{
    (void)arg;
    self_side = SDIO_SIDE_TX;
    for (int i = 0; i < write_rounds; i++) {
        if (write_pause_us)
            spin_us(write_pause_us);
        sdio_bus_take(SDIO_SIDE_TX);
        spin_us(10);
        unlock_mutex(sdio_bus_lock);
    }
    stop = 1;
    return NULL;
}

static void run(const char *name, int rounds, int pause_us)
{
    pthread_t r, w;
    stop = 0;
    taken[0] = taken[1] = 0;
    most_taken_while_waiting[0] = most_taken_while_waiting[1] = 0;
    write_rounds = rounds;
    write_pause_us = pause_us;
    pthread_create(&r, NULL, reader, NULL);
    pthread_create(&w, NULL, writer, NULL);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    printf("%s: read %u, write %u, most read takes while write waited %u, "
           "most write takes while read waited %u\n", name, taken[0], taken[1],
           most_taken_while_waiting[SDIO_SIDE_TX], most_taken_while_waiting[SDIO_SIDE_RX]);
    if (taken[1] != (uint32_t)rounds || taken[0] == 0) exit(2);
    if (most_taken_while_waiting[SDIO_SIDE_TX] > 2) exit(3);
    if (most_taken_while_waiting[SDIO_SIDE_RX] > 2) exit(4);
}

int main(void)
{
    for (int side = 0; side < SDIO_SIDES; side++) {
        sdio_bus_turn[side] = calloc(1, sizeof(sem_t_));
        pthread_mutex_init(&sdio_bus_turn[side]->m, NULL);
        pthread_cond_init(&sdio_bus_turn[side]->c, NULL);
    }
    run("a write now and then", 300, 500);
    run("both back to back", 3000, 0);
    return 0;
}
"""

compiler = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc")
assert compiler, "a host C compiler is needed"
with tempfile.TemporaryDirectory() as tmp:
    c_path = Path(tmp) / "handoff.c"
    c_path.write_text(HARNESS.replace("TAKE_SOURCE", take), encoding="utf-8")
    binary = Path(tmp) / "handoff"
    subprocess.run([compiler, "-std=gnu11", "-O2", "-Wall", "-Werror", "-pthread",
                    str(c_path), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    print(result.stdout, end="")
    assert result.returncode == 0, f"handoff harness failed ({result.returncode})"

print("ESP-Hosted SDIO bus handoff patch: PASS")
