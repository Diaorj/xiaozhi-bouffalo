"""Exercise the real retained WRAM encoder worker with a host FreeRTOS simulation."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import PORT

HARNESS = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
extern "C" {
#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"
#include "audio_encoder_worker.h"
}
struct MockEvent { std::mutex lock; std::condition_variable cv; unsigned bits=0; };
struct MockTask { std::mutex lock; std::condition_variable cv; unsigned notices=0; };
static MockEvent event;
static MockTask task;
static unsigned mode, event_creates, task_creates, stack_allocations;
alignas(16) static unsigned char fake_stack[32768];
extern "C" size_t kfree_size(uint32_t heap) {assert(heap==3);return mode==3?40000:65536;}
extern "C" size_t kmax_free_size(uint32_t heap) {assert(heap==3);return mode==4?30000:60000;}
extern "C" void* kmalloc(size_t n,uint32_t flags) {
    assert(n==32768 && flags==(3|0x30));++stack_allocations;return fake_stack;
}
static std::atomic<unsigned> calls{0};
static std::atomic<bool> entered{false}, release_job{false};
extern "C" EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* storage) {
    assert(storage);++event_creates;
    if(mode==2 && event_creates==1)return nullptr;
    return &event;
}
extern "C" EventBits_t xEventGroupSetBits(EventGroupHandle_t g,EventBits_t bits) {
    std::lock_guard<std::mutex> guard(g->lock);g->bits|=bits;g->cv.notify_all();return g->bits;
}
extern "C" EventBits_t xEventGroupClearBits(EventGroupHandle_t g,EventBits_t bits) {
    std::lock_guard<std::mutex> guard(g->lock);unsigned old=g->bits;g->bits&=~bits;return old;
}
extern "C" EventBits_t xEventGroupWaitBits(EventGroupHandle_t g,EventBits_t bits,int,int,unsigned) {
    std::unique_lock<std::mutex> guard(g->lock);
    g->cv.wait(guard,[&]{return (g->bits&bits)==bits;});return g->bits;
}
extern "C" unsigned ulTaskNotifyTake(int clear,unsigned timeout) {
    assert(clear && timeout==portMAX_DELAY);
    std::unique_lock<std::mutex> guard(task.lock);
    task.cv.wait(guard,[]{return task.notices>0;});unsigned n=task.notices;task.notices=0;return n;
}
extern "C" void xTaskNotifyGive(TaskHandle_t handle) {
    assert(handle==&task);std::lock_guard<std::mutex> guard(task.lock);
    ++task.notices;task.cv.notify_all();
}
extern "C" TaskHandle_t xTaskCreateStatic(void(*entry)(void*),const char*,unsigned depth,
                                          void* context,unsigned priority,StackType_t* stack,
                                          StaticTask_t* tcb) {
    assert(depth==8192 && priority==6 && stack && tcb);++task_creates;
    if(mode==1 && task_creates==1)return nullptr;
    std::thread(entry,context).detach();return &task;
}
static void simple_job(void* context) {assert(context==&calls);++calls;}
static void blocked_job(void*) {
    entered=true;while(!release_job)std::this_thread::sleep_for(std::chrono::milliseconds(1));++calls;
}
int main(int argc,char** argv) {
    TaskHandle_t handle=nullptr;
    assert(argc==2);mode=unsigned(std::atoi(argv[1]));
    assert(AudioEncoderWorkerWait()==-1 && AudioEncoderWorkerStart(nullptr,nullptr,&handle)==-1);
    if(mode>=3) {
        assert(AudioEncoderWorkerStart(simple_job,&calls,&handle)==-1 && stack_allocations==0);
        puts("WRAM_GUARD_OK");fflush(stdout);std::_Exit(0);
    }
    if(mode)assert(AudioEncoderWorkerStart(simple_job,&calls,&handle)==-1);
    for(unsigned i=0;i<50;++i) {
        assert(AudioEncoderWorkerStart(simple_job,&calls,&handle)==0);
        assert(AudioEncoderWorkerStart(simple_job,&calls,&handle)==-1);
        assert(AudioEncoderWorkerWait()==0 && calls==i+1 && AudioEncoderWorkerWait()==-1);
    }
    assert(AudioEncoderWorkerStart(blocked_job,nullptr,&handle)==0);
    while(!entered)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::atomic<bool> waiting{false}, joined{false};
    std::thread waiter([&]{waiting=true;assert(AudioEncoderWorkerWait()==0);joined=true;});
    while(!waiting)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!joined);
    release_job=true;waiter.join();assert(joined && calls==51);
    assert(stack_allocations==1);
    assert(task_creates==(mode==1?2:1) && event_creates==(mode==2?2:1));
    puts("WRAM_ENCODER_REUSE_OK");fflush(stdout);
    // The firmware worker deliberately lives for the process lifetime.
    std::_Exit(0);
}
"""

class AudioEncoderWorkerTests(unittest.TestCase):
    def test_static_reservation_reuse_join_and_creation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef uint32_t StackType_t;typedef struct {unsigned data[64];} StaticTask_t;typedef struct {unsigned data[32];} StaticEventGroup_t;typedef struct MockTask* TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n#define portMAX_DELAY 0xffffffffu\n')
            (work/'event_groups.h').write_text('#pragma once\n#include "FreeRTOS.h"\ntypedef struct MockEvent* EventGroupHandle_t;typedef unsigned EventBits_t;\nEventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t*);\nEventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,unsigned);\n')
            (work/'task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nunsigned ulTaskNotifyTake(int,unsigned);void xTaskNotifyGive(TaskHandle_t);\nTaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,unsigned,void*,unsigned,StackType_t*,StaticTask_t*);\n')
            (work/'mm.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#define MM_HEAP_WRAM_0 3\n#define MM_FLAG_HEAP_WRAM_0 3\n#define MM_FLAG_ALIGN_16BYTE 0x30\nsize_t kfree_size(uint32_t);size_t kmax_free_size(uint32_t);void* kmalloc(size_t,uint32_t);\n')
            (work/'log.h').write_text('#pragma once\n#define LOG_I(...) ((void)0)\n#define LOG_E(...) ((void)0)\n')
            (work/'test.cc').write_text(HARNESS)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),'-c',str(PORT/'audio_encoder_worker.c'),'-o',str(work/'worker.o')],check=True)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(work),'-I'+str(PORT),str(work/'test.cc'),str(work/'worker.o'),'-o',str(work/'test')],check=True)
            for mode in range(5):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'test'),str(mode)],capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('WRAM_GUARD_OK' if mode>=3 else 'WRAM_ENCODER_REUSE_OK',result.stdout)

if __name__=='__main__':unittest.main()
