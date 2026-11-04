#!/usr/bin/env python3
"""Run the task-stack scan of webrtc_stats_json.c against a fake kernel."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
source = (component / 'webrtc_stats_json.c').read_text()
start = source.index('/* The fill byte of an unused task stack')
end = source.index('\nstatic esp_err_t stats_json_create', start)
implementation = source[start:end]
assert 'add_task_stack_budgets(root, detailed);' in source[end:]
# Keep the detailed/idle decision explicit at the shared snapshot entry point.
assert 'detailed = detailed || (!stats.mode_valid && !stats.browser_session_active);' in source[end:]

stubs = r"""
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_SPIRAM 1
/* The firmware's malloc gives internal RAM for a request under 1 KiB:
 * small_bytes has what cJSON asked for in such requests. */
static size_t small_bytes;
static void *counted_malloc(size_t size) { if(size<1024) small_bytes+=size; return malloc(size); }
static void *heap_caps_malloc(size_t size,int caps) { assert(caps==MALLOC_CAP_SPIRAM); return malloc(size); }
static void heap_caps_free(void *memory) { free(memory); }
#define configMAX_TASK_NAME_LEN 16
#define pdTRUE 1
#define pdFALSE 0
#define tskNO_AFFINITY INT32_MAX
typedef int BaseType_t;
typedef int _lock_t;
static void _lock_acquire(_lock_t *lock) { assert(*lock==0); *lock=1; }
static void _lock_release(_lock_t *lock) { assert(*lock==1); *lock=0; }

/* A task of the fake kernel: its stack is `size` bytes at `memory + offset`. */
typedef struct {
 char name[configMAX_TASK_NAME_LEN];
 BaseType_t core_id;
 uint32_t offset, size;
 bool psram, present;
 _Alignas(16) uint8_t memory[16384];
} fake_task_t;
typedef fake_task_t *TaskHandle_t;
typedef struct { void *pxTCB; uint8_t *pxTopOfStack; uint8_t *pxEndOfStack; } TaskSnapshot_t;
typedef struct { unsigned uxCurrentListIndex; void *pxNextListItem; TaskHandle_t pxTaskHandle; } TaskIterator_t;

#define FAKE_TASKS 100
static fake_task_t tasks[FAKE_TASKS];
/* The kernel's lists: a task can be in two of them, and an entry whose
 * control block the kernel does not trust has no handle. */
static fake_task_t *lists[FAKE_TASKS + 2];
static unsigned list_length;
static uint64_t now_us=1000000;
static unsigned kernel_lock, lock_takes, list_reads;
/* Called at each release of the kernel lock, where another task can run. */
static void (*at_release)(unsigned takes);

static int64_t esp_timer_get_time(void) { return (int64_t)now_us; }
/* The take waits lock_wait_us for the other core, with interrupts masked. */
static unsigned lock_wait_us;
static void prvTakeKernelLock(void) { assert(kernel_lock==0); now_us+=lock_wait_us; kernel_lock=1; ++lock_takes; }
static void prvReleaseKernelLock(void)
{
 assert(kernel_lock==1); kernel_lock=0;
 if(at_release) at_release(lock_takes);
}
static int xTaskGetNext(TaskIterator_t *iterator)
{
 assert(kernel_lock); ++list_reads;
 while(iterator->uxCurrentListIndex<list_length) {
  fake_task_t *task=lists[iterator->uxCurrentListIndex++];
  if(task && !task->present) continue;
  iterator->pxTaskHandle=task;
  return 0;
 }
 return -1;
}
static uint8_t *stack_low(const fake_task_t *task) { return (uint8_t *)task->memory+task->offset; }
/* As tasks.c: the highest address is aligned down to 16 bytes. */
static uint8_t *stack_high(const fake_task_t *task)
{ return (uint8_t *)((uintptr_t)(stack_low(task)+task->size-1) & ~(uintptr_t)15); }
static BaseType_t vTaskGetSnapshot(TaskHandle_t task, TaskSnapshot_t *snapshot)
{
 assert(kernel_lock && task->present);
 snapshot->pxEndOfStack=stack_high(task);
 return pdTRUE;
}
/* The hold for the task "mqtt_task" takes 30 us longer than the others. */
static uint8_t *xTaskGetStackStart(TaskHandle_t task)
{
 assert(kernel_lock && task->present);
 if(strcmp(task->name,"mqtt_task")==0) now_us+=30;
 return stack_low(task);
}
static const char *pcTaskGetName(TaskHandle_t task) { assert(kernel_lock && task->present); return task->name; }
static BaseType_t xTaskGetCoreID(TaskHandle_t task) { assert(kernel_lock && task->present); return task->core_id; }
static bool esp_ptr_external_ram(const void *address)
{
 for(unsigned i=0;i<FAKE_TASKS;i++)
  if((const uint8_t *)address>=tasks[i].memory && (const uint8_t *)address<tasks[i].memory+sizeof(tasks[i].memory))
   return tasks[i].psram;
 assert(false); return false;
}
static void add_u64(cJSON *root,const char *name,uint64_t value)
{ cJSON_AddNumberToObject(root,name,(double)value); }

/* A new task: the kernel fills the stack, and the task then writes the
 * `used` bytes under its highest address. */
static fake_task_t *create_task(unsigned slot,const char *name,BaseType_t core_id,
                                uint32_t offset,uint32_t size,uint32_t used,bool psram)
{
 fake_task_t *task=&tasks[slot];
 assert(offset+size<=sizeof(task->memory));
 memset(task,0,sizeof(*task));
 snprintf(task->name,sizeof(task->name),"%s",name);
 task->core_id=core_id; task->offset=offset; task->size=size; task->psram=psram; task->present=true;
 memset(stack_low(task),0xa5,size);
 memset(stack_low(task)+size-used,0x11,used);
 return task;
}
static const cJSON *property(const cJSON *root,const char *name)
{ const cJSON *item=cJSON_GetObjectItemCaseSensitive(root,name); assert(item); return item; }
static const cJSON *task_entry(const cJSON *root,const char *name)
{
 const cJSON *entry, *found=NULL;
 cJSON_ArrayForEach(entry,property(root,"taskStacks")) {
  if(strcmp(property(entry,"name")->valuestring,name)==0) { assert(!found); found=entry; }
 }
 return found;
}
static double number(const cJSON *root,const char *task,const char *name)
{ const cJSON *entry=task_entry(root,task); assert(entry); return property(entry,name)->valuedouble; }
static void add_task_stack_budgets(cJSON *root, bool detailed);
/* The answer as its reader gets it: printed and parsed. */
static cJSON *stats(bool detailed)
{
 cJSON *root=cJSON_CreateObject(); add_task_stack_budgets(root,detailed); assert(!kernel_lock);
 char *text=cJSON_PrintUnformatted(root); assert(text);
 cJSON *read=cJSON_Parse(text); assert(read && cJSON_IsArray(property(read,"taskStacks")));
 free(text); cJSON_Delete(root);
 return read;
}
"""
checks = r"""
static void end_worker(unsigned takes) { if(takes==1) tasks[2].present=false; }
/* The worker ends, and a new task gets its control block, with a stack in
 * other memory that has another lowest address. */
static void replace_worker(unsigned takes)
{ if(takes==1) create_task(2,"successor",0,20,492,40,true); }
/* Each visit of a task under the kernel lock takes 7 us longer than the one before. */
static void clock_runs(unsigned takes) { now_us+=7*takes; }

int main(void)
{
 /* The unused bytes of a stack, from every alignment of its lowest address
  * and with every count of unused bytes around the word-wise part. */
 static _Alignas(4) uint8_t memory[64];
 for(unsigned low=0;low<4;low++) for(unsigned size=0;size<=40;size++) for(unsigned unused=0;unused<=size;unused++) {
  /* The memory over the stack has the fill byte too: a read past the
   * stack's end counts it. A fill byte over the first written byte is not
   * unused stack. */
  memset(memory,0xa5,sizeof(memory));
  memset(memory+low+unused,0x5a,size-unused);
  if(unused+2<=size) memory[low+unused+1]=0xa5;
  assert(task_stack_unused_bytes(memory+low,memory+low+size)==unused);
 }

 /* Before the first scan the answer has no list and asks the kernel nothing. */
 create_task(0,"main",0,0,1024,300,false);
 create_task(1,"mqtt_task",tskNO_AFFINITY,0,1024,1000,true);
 create_task(2,"worker",1,4,500,100,false);
 create_task(3,"tiT",tskNO_AFFINITY,12,996,0,false);
 create_task(4,"tiT",tskNO_AFFINITY,0,256,236,true);
 /* "main" is in two lists, and one entry has no handle. */
 lists[0]=&tasks[0]; lists[1]=&tasks[1]; lists[2]=NULL; lists[3]=&tasks[2];
 lists[4]=&tasks[0]; lists[5]=&tasks[3]; lists[6]=&tasks[4]; list_length=7;
 cJSON *root=stats(false);
 const cJSON *detail=property(root,"taskStackDetails");
 assert(cJSON_IsTrue(property(detail,"cached")) && cJSON_IsFalse(property(detail,"available")));
 assert(cJSON_IsNull(property(detail,"sampledAtUs")) && cJSON_IsNull(property(detail,"ageMs")));
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==0);
 assert(lock_takes==0 && list_reads==0);
 cJSON_Delete(root);

 /* A detailed answer has every task once, with the size and the unused
  * bytes of its stack and the memory that the stack is in. */
 at_release=clock_runs; lock_wait_us=5;
 root=stats(true);
 at_release=NULL; lock_wait_us=0;
 detail=property(root,"taskStackDetails");
 assert(cJSON_IsFalse(property(detail,"cached")) && cJSON_IsTrue(property(detail,"available")));
 assert(property(detail,"sampledAtUs")->valuedouble==1000000);
 assert(cJSON_IsFalse(property(detail,"truncated")));
 /* One hold of the kernel lock for the list and one per task. */
 assert(lock_takes==6);
 assert(property(detail,"scanUs")->valuedouble==7*(1+2+3+4+5+6)+6*5+30);
 /* The longest masked time is that of one hold with its wait for the lock,
  * and not the time between two holds. */
 assert(property(detail,"maxMaskedUs")->valuedouble==5+30);
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==5);
 assert(number(root,"main","configuredBytes")==1024);
 assert(number(root,"main","highWatermarkBytes")==724);
 assert(number(root,"main","peakUsedBytes")==300);
 assert(number(root,"main","coreId")==0);
 assert(strcmp(property(task_entry(root,"main"),"stackMemory")->valuestring,"internal")==0);
 assert(number(root,"mqtt_task","highWatermarkBytes")==24);
 assert(number(root,"mqtt_task","coreId")==-1);
 assert(strcmp(property(task_entry(root,"mqtt_task"),"stackMemory")->valuestring,"psram")==0);
 /* 500 bytes are no multiple of 16: the kernel's records give 496. */
 assert(number(root,"worker","configuredBytes")==496);
 assert(number(root,"worker","highWatermarkBytes")==400);
 assert(number(root,"worker","coreId")==1);
 /* Two tasks have the name "tiT": one never ran, one has 20 bytes left. */
 unsigned tit=0; const cJSON *entry;
 cJSON_ArrayForEach(entry,property(root,"taskStacks")) {
  if(strcmp(property(entry,"name")->valuestring,"tiT")!=0) continue;
  ++tit;
  double size=property(entry,"configuredBytes")->valuedouble;
  double unused=property(entry,"highWatermarkBytes")->valuedouble;
  assert((size==992 && unused==980) || (size==256 && unused==20));
 }
 assert(tit==2);
 cJSON_Delete(root);

 /* A routine answer repeats the last scan and asks the kernel nothing. */
 now_us=6000000; tasks[2].present=false; create_task(0,"main",1,0,1024,900,false);
 unsigned takes=lock_takes, reads=list_reads;
 for(unsigned i=0;i<20;i++) {
  root=stats(false);
  detail=property(root,"taskStackDetails");
  assert(cJSON_IsTrue(property(detail,"cached")));
  assert(property(detail,"sampledAtUs")->valuedouble==1000000);
  assert(property(detail,"ageMs")->valuedouble==5000);
  assert(cJSON_GetArraySize(property(root,"taskStacks"))==5);
  assert(number(root,"main","highWatermarkBytes")==724 && number(root,"main","coreId")==0);
  cJSON_Delete(root);
 }
 assert(lock_takes==takes && list_reads==reads);

 /* The next detailed answer has the tasks of its own moment. */
 root=stats(true);
 assert(property(property(root,"taskStackDetails"),"sampledAtUs")->valuedouble==6000000);
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==4);
 assert(task_entry(root,"worker")==NULL);
 assert(number(root,"main","highWatermarkBytes")==124 && number(root,"main","coreId")==1);
 cJSON_Delete(root);

 /* A task that ends between the list and the read of its stack is left
  * out, and its stack is not read: the fake kernel asserts on a call for a
  * task that is not present. */
 create_task(2,"worker",1,4,500,100,false);
 at_release=end_worker; lock_takes=0;
 root=stats(true);
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==4);
 assert(task_entry(root,"worker")==NULL);
 cJSON_Delete(root);

 /* A new task that got the control block of one that ended in that time is
  * in the list as itself: no number of the old task is in its entry. */
 create_task(2,"worker",1,4,500,100,false);
 at_release=replace_worker; lock_takes=0;
 root=stats(true);
 at_release=NULL;
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==5);
 assert(task_entry(root,"worker")==NULL);
 assert(number(root,"successor","configuredBytes")==480);
 assert(number(root,"successor","highWatermarkBytes")==452);
 assert(number(root,"successor","coreId")==0);
 assert(strcmp(property(task_entry(root,"successor"),"stackMemory")->valuestring,"psram")==0);
 cJSON_Delete(root);

 /* A stack is read in one hold of the kernel lock, whatever its size:
  * between two holds its task could write under the bytes already counted. */
 create_task(5,"large",0,0,16384,16384-9000,true);
 lists[7]=&tasks[5]; list_length=8;
 lock_takes=0;
 root=stats(true);
 assert(lock_takes==1+6);
 assert(number(root,"large","configuredBytes")==16384 && number(root,"large","highWatermarkBytes")==9000);
 cJSON_Delete(root);

 /* A name with bytes that JSON would need an escape for stays readable. */
 create_task(5,"a\"b\\c\td\x80",0,0,256,16,false);
 root=stats(true);
 assert(number(root,"a?b?c?d?","highWatermarkBytes")==240);
 cJSON_Delete(root);

 /* The list takes the same internal RAM for 12 tasks and for 96: what
  * cJSON allocates in requests under 1 KiB does not grow with the tasks. */
 cJSON_Hooks hooks={counted_malloc,free};
 cJSON_InitHooks(&hooks);
 size_t small[2];
 for(unsigned round=0;round<2;round++) {
  list_length=round==0 ? 12 : FAKE_TASKS;
  for(unsigned i=0;i<list_length;i++) { create_task(i,"many",0,0,64,i%48,false); lists[i]=&tasks[i]; }
  small_bytes=0;
  root=cJSON_CreateObject(); add_task_stack_budgets(root,true);
  small[round]=small_bytes;
  cJSON_Delete(root);
 }
 assert(small[0]>0 && small[1]==small[0]);
 cJSON_InitHooks(NULL);

 /* More tasks than the list holds: the answer says that it is cut. */
 for(unsigned i=0;i<FAKE_TASKS;i++) { create_task(i,"many",0,0,64,i%48,false); lists[i]=&tasks[i]; }
 list_length=FAKE_TASKS;
 root=stats(true);
 assert(cJSON_IsTrue(property(property(root,"taskStackDetails"),"truncated")));
 assert(cJSON_GetArraySize(property(root,"taskStacks"))==TASK_STACK_MAX_TASKS);
 cJSON_Delete(root);
 puts("task stacks: every task once, sizes and unused bytes from the kernel's records, ended tasks left out, routine answers from the last scan: OK");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='webrtc-task-stack-') as directory:
    temp = Path(directory)
    (temp / 'test.c').write_text(stubs + implementation + checks)
    cjson = repo / 'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-fsanitize=address,undefined',
                    '-I', str(cjson), str(temp / 'test.c'), str(cjson / 'cJSON.c'),
                    '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
