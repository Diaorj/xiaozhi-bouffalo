"""Exercise the real static speaker worker with a host FreeRTOS simulation."""
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
#include "speaker_worker.h"
}
struct MockEvent { std::mutex lock; std::condition_variable cv; unsigned bits=0; };
struct MockTask { std::mutex lock; std::condition_variable cv; unsigned notices=0; };
static MockEvent event;
static MockTask task;
static unsigned mode, event_creates, task_creates;
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
    assert(depth==1536 && priority==8 && stack && tcb);++task_creates;
    if(mode==1 && task_creates==1)return nullptr;
    std::thread(entry,context).detach();return &task;
}
static void simple_job(void* context) {assert(context==&calls);++calls;}
static void blocked_job(void*) {
    entered=true;while(!release_job)std::this_thread::sleep_for(std::chrono::milliseconds(1));++calls;
}
int main(int argc,char** argv) {
    assert(argc==2);mode=unsigned(std::atoi(argv[1]));
    assert(SpeakerWorkerWait()==-1 && SpeakerWorkerStart(nullptr,nullptr)==-1);
    if(mode)assert(SpeakerWorkerStart(simple_job,&calls)==-1);
    for(unsigned i=0;i<50;++i) {
        assert(SpeakerWorkerStart(simple_job,&calls)==0);
        assert(SpeakerWorkerStart(simple_job,&calls)==-1);
        assert(SpeakerWorkerWait()==0 && calls==i+1 && SpeakerWorkerWait()==-1);
    }
    assert(SpeakerWorkerStart(blocked_job,nullptr)==0);
    while(!entered)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::atomic<bool> waiting{false}, joined{false};
    std::thread waiter([&]{waiting=true;assert(SpeakerWorkerWait()==0);joined=true;});
    while(!waiting)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!joined);
    release_job=true;waiter.join();assert(joined && calls==51);
    assert(task_creates==(mode==1?2:1) && event_creates==(mode==2?2:1));
    puts("STATIC_SPEAKER_REUSE_OK");fflush(stdout);
    // The firmware worker deliberately lives for the process lifetime.
    std::_Exit(0);
}
"""

class SpeakerWorkerTests(unittest.TestCase):
    def test_static_reservation_reuse_join_and_creation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef uint32_t StackType_t;typedef struct {unsigned data[64];} StaticTask_t;typedef struct {unsigned data[32];} StaticEventGroup_t;typedef struct MockTask* TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n#define portMAX_DELAY 0xffffffffu\n')
            (work/'event_groups.h').write_text('#pragma once\n#include "FreeRTOS.h"\ntypedef struct MockEvent* EventGroupHandle_t;typedef unsigned EventBits_t;\nEventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t*);\nEventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,unsigned);\n')
            (work/'task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nunsigned ulTaskNotifyTake(int,unsigned);void xTaskNotifyGive(TaskHandle_t);\nTaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,unsigned,void*,unsigned,StackType_t*,StaticTask_t*);\n')
            (work/'test.cc').write_text(HARNESS)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),'-c',str(PORT/'speaker_worker.c'),'-o',str(work/'worker.o')],check=True)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(work),'-I'+str(PORT),str(work/'test.cc'),str(work/'worker.o'),'-o',str(work/'test')],check=True)
            for mode in range(3):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'test'),str(mode)],capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('STATIC_SPEAKER_REUSE_OK',result.stdout)

if __name__=='__main__':unittest.main()
