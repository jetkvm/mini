#!/usr/bin/env python3
"""Compile the production reset state machine against fault-injected storage."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
source = (root/'components/jetkvm_factory_reset/jetkvm_factory_reset.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
typedef int esp_err_t, nvs_handle_t, portMUX_TYPE;
typedef void *TaskHandle_t;
typedef struct { size_t size; } esp_partition_t;
typedef struct { char namespace_name[16]; } nvs_entry_info_t;
typedef unsigned *nvs_iterator_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NVS_INVALID_LENGTH 4
#define ESP_ERR_NVS_TYPE_MISMATCH 5
#define ESP_ERR_INVALID_STATE 6
#define ESP_LOGW(...) ((void)0)
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_ANY 0
#define ESP_PARTITION_TYPE_DATA 0
#define ESP_PARTITION_SUBTYPE_ANY 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 99999
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define eSetValueWithOverwrite 1
static const char *names[] = {"jk_reset", "jk_access", "wifi_prov", "jkvm_cloud", "jk_network", "jk_display"};
static bool populated[6], staged_populated[6], create_fail, save_fail, storage_fail;
static uint32_t blob[4], staged_blob[4];
static unsigned cursor, erased, storage_wipes, notification, progress_calls;
static int erase_fail_at, get_error, open_error, commit_fail_at, commits;
static bool discard_fail, partition_missing, alloc_fail;
/* Three 64 KiB blocks and a trailing 4 KiB one. */
#define STORAGE_SIZE (3u * 65536u + 4096u)
static uint8_t flash[STORAGE_SIZE];
static int read_fail_block;
static bool flash_blank(void) { for (size_t i = 0; i < STORAGE_SIZE; ++i) if (flash[i] != 0xff) return false; return true; }
static jmp_buf restart;
static int nvs_open(const char *name, int mode, int *h) {
    (void)mode;
    if (open_error) return open_error;
    for (int i = 0; i < 6; ++i) if (!strcmp(names[i], name)) { *h=i; return 0; }
    return 1;
}
static void nvs_close(int h) { (void)h; }
static int nvs_set_blob(int h, const char *key, const void *p, size_t n) {
    assert(h==0 && !strcmp(key,"intent") && n==sizeof(blob));
    if (save_fail) return 1;
    memcpy(staged_blob,p,n);staged_populated[0]=true;return 0;
}
static int nvs_get_blob(int h, const char *key, void *p, size_t *n) {
    (void)key;assert(h==0 && *n==sizeof(blob));
    if (get_error) return get_error;
    if (!populated[0]) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(p,blob,*n);return 0;
}
static int nvs_commit(int h) {
    if (++commits==commit_fail_at) return 1;
    populated[h]=staged_populated[h];
    if(h==0) memcpy(blob,staged_blob,sizeof(blob));
    return 0;
}
static int nvs_erase_key(int h, const char *key) {
    (void)key;assert(h==0);if(discard_fail)return 1;staged_populated[0]=false;return 0;
}
static int nvs_erase_all(int h) {
    assert(h>0 && populated[0] && progress_calls);
    if (++erased==(unsigned)erase_fail_at) return 1;
    staged_populated[h]=false;return 0;
}
static int seek(nvs_iterator_t *p) {
    while (cursor<6 && !populated[cursor]) ++cursor;
    *p=cursor<6?&cursor:NULL;return *p?0:ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_entry_find(const char *partition, const char *ns, int type, nvs_iterator_t *p) {
    assert(!strcmp(partition,"nvs") && !ns && !type);cursor=0;return seek(p);
}
static int nvs_entry_next(nvs_iterator_t *p) { ++cursor;return seek(p); }
static void nvs_entry_info(nvs_iterator_t p, nvs_entry_info_t *i) { strcpy(i->namespace_name,names[*p]); }
static void nvs_release_iterator(nvs_iterator_t p) { (void)p; }
static const esp_partition_t *esp_partition_find_first(int type,int sub,const char *label) {
    (void)type;(void)sub;assert(!strcmp(label,"storage"));
    static const esp_partition_t p={STORAGE_SIZE};return partition_missing?NULL:&p;
}
__attribute__((unused)) static int esp_partition_read(const esp_partition_t *p,size_t start,void *out,size_t length) {
    assert(start%4096==0 && length && start+length<=p->size && populated[0] && progress_calls);
    if (read_fail_block>=0 && start/65536==(size_t)read_fail_block) return 1;
    memcpy(out,flash+start,length);return 0;
}
static int esp_partition_erase_range(const esp_partition_t *p,size_t start,size_t length) {
    assert(start%4096==0 && length%4096==0 && length && start+length<=p->size);
    assert(populated[0] && progress_calls);
    ++storage_wipes;if(storage_fail)return 1;
    memset(flash+start,0xff,length);return 0;
}
__attribute__((unused)) static void *heap_caps_malloc(size_t size,unsigned caps) {
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return alloc_fail?NULL:malloc(size);
}
__attribute__((unused)) static void heap_caps_free(void *p) { free(p); }
static void esp_restart(void) { longjmp(restart,1); }
static void vTaskDelay(unsigned ticks) { assert(ticks>=1000); }
static unsigned ulTaskNotifyTake(int clear,unsigned timeout) { (void)clear;(void)timeout;return notification; }
static void vTaskDeleteWithCaps(void *task) { (void)task; }
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,
    void *arg,int priority,void **task,unsigned caps) {
    (void)fn;(void)name;(void)arg;(void)priority;assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    assert(stack==4096);if(create_fail)return 0;*task=(void *)1;return 1;
}
static void xTaskNotify(void *task,unsigned value,int mode) { (void)task;(void)mode;notification=value; }
'''
main = r'''
static void power_on(void) {
    memset(&s_intent,0,sizeof(s_intent));s_busy=false;s_restart_task=NULL;
    atomic_store(&s_state,RESET_IDLE);progress_calls=0;
    memcpy(staged_populated,populated,sizeof(populated));memcpy(staged_blob,blob,sizeof(blob));
}
static void reset_fixture(void) {
    memset(populated,1,sizeof(populated));populated[0]=false;memset(blob,0,sizeof(blob));
    erased=storage_wipes=notification=0;
    erase_fail_at=get_error=open_error=commit_fail_at=commits=0;
    create_fail=save_fail=storage_fail=discard_fail=partition_missing=alloc_fail=false;
    /* Data at the start of block 0, in the third 4 KiB chunk of block 1 and
     * in the trailing short block; block 2 is blank. */
    memset(flash,0xff,sizeof(flash));flash[100]=0;flash[65536+8192+5]=0xfe;
    flash[STORAGE_SIZE-1]=0x7f;read_fail_block=-1;
    power_on();
}
static void progress(void) {
    assert(jetkvm_factory_reset_pending());++progress_calls;
}
static void cleanup_boot(void) {
    power_on();assert(!jetkvm_factory_reset_boot_begin(progress));
    assert(progress_calls==1 && populated[0] && jetkvm_factory_reset_pending());
    for(unsigned i=1;i<6;++i) assert(!populated[i]);
}
int main(void) {
    reset_fixture();
    assert(!jetkvm_factory_reset_boot_begin(progress)&&!progress_calls&&!erased);
    assert(!jetkvm_factory_reset_finish(0));
    assert(!jetkvm_factory_reset_request(NULL));
    assert(notification==0 && !erased && populated[0]);
    assert(blob[0]==1 && blob[1]==3 && !blob[2] && !blob[3]);
    jetkvm_factory_reset_dispatch_complete();assert(notification==1 && !s_restart_task);
    if (!setjmp(restart)) { restart_worker(NULL);assert(false); }
    notification=0;jetkvm_factory_reset_dispatch_complete();assert(!notification);
    assert(jetkvm_factory_reset_request(NULL));
    cleanup_boot();assert(erased==5 && storage_wipes==3 && flash_blank());
    assert(!jetkvm_factory_reset_finish(0)&&!populated[0]&&!jetkvm_factory_reset_pending());
    power_on();assert(!jetkvm_factory_reset_boot_begin(progress)&&!progress_calls);
    assert(storage_wipes==3); /* later boots do not erase again */

    /* Storage: only blocks holding data are erased; an unreadable block is
     * erased too; without a read buffer the whole partition is erased. */
    reset_fixture();memset(flash,0xff,sizeof(flash));
    assert(!jetkvm_factory_reset_request(NULL));cleanup_boot();
    assert(storage_wipes==0 && flash_blank());
    reset_fixture();read_fail_block=2;
    assert(!jetkvm_factory_reset_request(NULL));cleanup_boot();
    assert(storage_wipes==4 && flash_blank());
    reset_fixture();alloc_fail=true;
    assert(!jetkvm_factory_reset_request(NULL));cleanup_boot();
    assert(storage_wipes==1 && flash_blank());

    /* Legacy interrupted TF/radio/local phases all preserve the card. No TF
     * functions are linked; the namespace list includes C5's c5_clean owner. */
    for(unsigned phase=1;phase<=3;++phase) {
        reset_fixture();populated[0]=true;blob[0]=1;blob[1]=phase;blob[2]=1;blob[3]=123;
        cleanup_boot();assert(!jetkvm_factory_reset_finish(0)&&!populated[0]);
    }
    /* Each failed local stage retains durable intent and can resume after
     * power loss, including after NVS was erased but storage was not. */
    for(unsigned failure=0;failure<6;++failure) {
        reset_fixture();assert(!jetkvm_factory_reset_request(NULL));
        if(failure==0) open_error=1;
        if(failure==1) erase_fail_at=2;
        if(failure==2) commit_fail_at=commits+2;
        if(failure==3) partition_missing=true;
        if(failure==4) storage_fail=true;
        if(failure==5) get_error=99;
        power_on();assert(jetkvm_factory_reset_boot_begin(progress)!=0);
        assert(populated[0]&&jetkvm_factory_reset_pending()&&jetkvm_factory_reset_error());
        open_error=erase_fail_at=commit_fail_at=get_error=0;partition_missing=storage_fail=false;
        cleanup_boot();assert(!jetkvm_factory_reset_finish(0)&&!populated[0]);
    }
    /* C5 failure, and failures opening/erasing/committing the final marker.
     * Main republishes the C5 failure; that must not accidentally complete. */
    for(unsigned failure=0;failure<4;++failure) {
        reset_fixture();assert(!jetkvm_factory_reset_request(NULL));cleanup_boot();
        open_error=failure==1;discard_fail=failure==2;
        if(failure==3) commit_fail_at=commits+1;
        assert(jetkvm_factory_reset_finish(failure==0?9:0)!=0);
        assert(jetkvm_factory_reset_finish(9)==9&&populated[0]&&jetkvm_factory_reset_error());
        assert(jetkvm_factory_reset_finish(0)==ESP_ERR_INVALID_STATE&&populated[0]);
        open_error=commit_fail_at=0;discard_fail=false;
        cleanup_boot();assert(!jetkvm_factory_reset_finish(0)&&!populated[0]);
    }
    reset_fixture();create_fail=true;assert(jetkvm_factory_reset_request(NULL));
    assert(!populated[0]&&!s_busy&&!jetkvm_factory_reset_pending());
    for(unsigned failure=0;failure<2;++failure) {
        reset_fixture();save_fail=failure==0;commit_fail_at=failure==1?1:0;
        assert(jetkvm_factory_reset_request(NULL));assert(notification==2&&!populated[0]);
        restart_worker(NULL);assert(!s_busy&&!jetkvm_factory_reset_pending());
    }
    for(unsigned invalid=0;invalid<6;++invalid) {
        reset_fixture();populated[0]=true;blob[0]=1;blob[1]=3;
        if(invalid==0) blob[0]=999;
        if(invalid==1) blob[1]=0;
        if(invalid==2) blob[1]=4;
        if(invalid==3) blob[2]=2;
        if(invalid==4) get_error=ESP_ERR_NVS_INVALID_LENGTH;
        if(invalid==5) get_error=ESP_ERR_NVS_TYPE_MISMATCH;
        power_on();assert(!jetkvm_factory_reset_boot_begin(progress));
        assert(!populated[0]&&populated[1]&&!progress_calls&&!erased&&!storage_wipes);
    }
    for(unsigned failure=0;failure<2;++failure) {
        reset_fixture();populated[0]=true;blob[0]=999;blob[1]=1;
        discard_fail=failure==0;commit_fail_at=failure==1?1:0;
        power_on();assert(jetkvm_factory_reset_boot_begin(progress)!=0);
        assert(populated[0]&&populated[1]&&!erased&&!storage_wipes);
    }
    puts("Factory reset: one reboot, blank storage blocks skipped, preserved TF, legacy intent, progress ordering, durable radio/commit failure and power-loss retry: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.c').write_text(prefix+source+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
