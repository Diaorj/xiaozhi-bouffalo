"""Check native pthread initialization, wakeups, and the actual SDK zero-init failure."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import ROOT, SDK

NATIVE = r"""
#include <cassert>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>
#include "platform/condition_variable.h"
static int mode;
static std::atomic<int> initializations{0};
extern "C" int __real_pthread_cond_init(pthread_cond_t*,const pthread_condattr_t*);
extern "C" int __real_pthread_cond_wait(pthread_cond_t*,pthread_mutex_t*);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __wrap_pthread_cond_init(pthread_cond_t* c,const pthread_condattr_t* a) {
    ++initializations;return mode==1?ENOMEM:__real_pthread_cond_init(c,a);
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c,pthread_mutex_t* m) {
    return mode==2?EINVAL:__real_pthread_cond_wait(c,m);
}
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* c) {
    return mode==3?ENOMEM:__real_pthread_cond_broadcast(c);
}
int main(int argc,char** argv) {
    assert(argc==2);mode=argv[1][0]-'0';
    struct Guarded {
        uint64_t before=0x12345678abcdef01ULL;
        xiaozhi::ConditionVariable condition;
        uint64_t after=0x87654321fedcba10ULL;
    } guarded;
#ifdef XIAOZHI_PLATFORM_BOUFFALO
    assert(initializations==1); // Must call the SDK initializer, never prebuilt C++ constructor.
#endif
    std::mutex mutex;
    if(mode==2) {std::unique_lock<std::mutex> lock(mutex);guarded.condition.wait(lock,[]{return false;});}
    if(mode==3)guarded.condition.notify_all();
    bool done=false;unsigned consumed=0;std::deque<unsigned> jobs;
    std::vector<std::thread> workers;
    for(int i=0;i<2;++i)workers.emplace_back([&] {
        for(;;) {
            std::unique_lock<std::mutex> lock(mutex);
            guarded.condition.wait(lock,[&]{return done || !jobs.empty();});
            if(jobs.empty() && done)break;
            jobs.pop_front();++consumed;
        }
    });
    guarded.condition.notify_all(); // A spurious wake must recheck the predicate.
    for(unsigned n=0;n<500;++n) {
        {std::lock_guard<std::mutex> lock(mutex);jobs.push_back(n);}
        guarded.condition.notify_all();
        std::this_thread::yield();
    }
    {std::lock_guard<std::mutex> lock(mutex);done=true;}
    guarded.condition.notify_all();
    for(auto& t:workers)t.join();
    assert(consumed==500);
    assert(guarded.before==0x12345678abcdef01ULL && guarded.after==0x87654321fedcba10ULL);
}
"""

FACADE = r"""
#ifndef MOCK_SDK_POSIX_H
#define MOCK_SDK_POSIX_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void* TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define eNoAction 0
#define posixconfigENABLE_PTHREAD_MUTEXATTR_T 0
#define posixconfigENABLE_PTHREAD_MUTEX_T 0
#define posixconfigENABLE_PTHREAD_COND_T 1
#define posixconfigENABLE_SEM_T 0
#define posixconfigENABLE_PTHREAD_BARRIER_T 0
#define posixconfigPTHREAD_COND_MAX_WAITERS 4
#include "FreeRTOS_POSIX_internal.h"
typedef pthread_cond_internal_t SdkCond;
typedef struct {int unused;} SdkMutex;
typedef void* SdkCondAttr;
void* pvPortMalloc(size_t);
void vPortFree(void*);
void vTaskSuspendAll(void);
void xTaskResumeAll(void);
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int xTaskNotify(TaskHandle_t,unsigned,int);
int xTaskNotifyWait(unsigned,unsigned,void*,unsigned);
unsigned uxTaskPriorityGet(TaskHandle_t);
#endif
"""

DECLARATIONS = r"""
#include "FreeRTOS_POSIX.h"
int SdkInit(SdkCond*,const SdkCondAttr*);
int SdkDestroy(SdkCond*);
int SdkWait(SdkCond*,SdkMutex*);
int SdkTimedWait(SdkCond*,SdkMutex*,const struct timespec*);
int SdkBroadcast(SdkCond*);
int SdkSignal(SdkCond*);
int MockMutexLock(SdkMutex*);
int MockMutexUnlock(SdkMutex*);
"""

SDK_CHECK = r"""
#include "FreeRTOS_POSIX/pthread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
void* malloc(size_t);
void free(void*);
static int notifications,waits,locks,unlocks;
void* pvPortMalloc(size_t n){return malloc(n);}
void vPortFree(void* p){free(p);}
void vTaskSuspendAll(void){}
void xTaskResumeAll(void){}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}
unsigned uxTaskPriorityGet(TaskHandle_t t){(void)t;return 1;}
int xTaskNotify(TaskHandle_t t,unsigned n,int action){(void)t;(void)n;(void)action;++notifications;return 1;}
int xTaskNotifyWait(unsigned a,unsigned b,void* c,unsigned d){(void)a;(void)b;(void)c;(void)d;++waits;return pdPASS;}
int MockMutexLock(SdkMutex* m){(void)m;++locks;return 0;}
int MockMutexUnlock(SdkMutex* m){(void)m;++unlocks;return 0;}
int UTILS_AbsoluteTimespecToDeltaTicks(const struct timespec* a,const struct timespec* b,TickType_t* ticks){(void)a;(void)b;*ticks=0;return 0;}
int main(void) {
    SdkCond zero={0};SdkMutex mutex={0};
    // Exactly what the old prebuilt C++ constructor leaves in the native fields.
    assert(SdkWait(&zero,&mutex)==ENOMEM && waits==0);
    assert(SdkBroadcast(&zero)==ENOMEM);
    SdkCond initialized={0};
    assert(SdkInit(&initialized,NULL)==0);
    assert(initialized.tasksLenth==4 && initialized.xIsInitialized==pdTRUE);
    assert(SdkWait(&initialized,&mutex)==0 && waits==1 && locks==1 && unlocks==1);
    initialized.xTasksWaiting[0]=(void*)1;
    assert(SdkBroadcast(&initialized)==0 && notifications==1 && initialized.xTasksWaiting[0]==NULL);
    assert(SdkDestroy(&initialized)==0);
    puts("ACTUAL_SDK_ZERO_INIT_FAILS_EXPLICIT_INIT_PASSES");
}
"""

class ConditionVariableTests(unittest.TestCase):
    def test_platform_wait_broadcast_and_error_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);source=work/'check.cc';source.write_text(NATIVE)
            base=['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                  '-I'+str(ROOT/'main'),str(source),'-Wl,--wrap=pthread_cond_init',
                  '-Wl,--wrap=pthread_cond_wait','-Wl,--wrap=pthread_cond_broadcast']
            for platform in (False,True):
                output=work/('sdk' if platform else 'standard')
                subprocess.run([*base,*(['-DXIAOZHI_PLATFORM_BOUFFALO=1'] if platform else []),'-o',str(output)],check=True)
                subprocess.run([str(output),'0'],check=True,timeout=5)
                if platform:
                    for mode,name,error in [('1','init',12),('2','wait',22),('3','broadcast',12)]:
                        r=subprocess.run([str(output),mode],capture_output=True,text=True,timeout=5)
                        self.assertNotEqual(r.returncode,0)
                        self.assertIn(f'CONDITION_VARIABLE_FAILED: {name} ret={error}',r.stderr)

    def test_actual_sdk_rejects_zero_initializer(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);(work/'FreeRTOS_POSIX').mkdir()
            (work/'FreeRTOS_POSIX.h').write_text(FACADE)
            (work/'FreeRTOS_POSIX/pthread.h').write_text(DECLARATIONS)
            (work/'FreeRTOS_POSIX/errno.h').write_text('#include <errno.h>\n')
            (work/'FreeRTOS_POSIX/utils.h').write_text('#include "FreeRTOS_POSIX.h"\nint UTILS_AbsoluteTimespecToDeltaTicks(const struct timespec*,const struct timespec*,TickType_t*);\n')
            (work/'iot_doubly_linked_list.h').write_text('')
            (work/'atomic_rtos.h').write_text('')
            (work/'check.c').write_text(SDK_CHECK)
            names={'pthread_cond_t':'SdkCond','pthread_mutex_t':'SdkMutex','pthread_condattr_t':'SdkCondAttr',
                   'pthread_cond_init':'SdkInit','pthread_cond_destroy':'SdkDestroy',
                   'pthread_cond_wait':'SdkWait','pthread_cond_timedwait':'SdkTimedWait',
                   'pthread_cond_broadcast':'SdkBroadcast','pthread_cond_signal':'SdkSignal',
                   'pthread_mutex_lock':'MockMutexLock','pthread_mutex_unlock':'MockMutexUnlock'}
            posix=SDK/'components/os/freertos/posix'
            subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
                            *['-D'+k+'='+v for k,v in names.items()],'-I'+str(work),'-I'+str(posix/'include'),
                            str(posix/'source/FreeRTOS_POSIX_pthread_cond.c'),str(work/'check.c'),'-o',str(work/'check')],check=True)
            r=subprocess.run([str(work/'check')],check=True,capture_output=True,text=True,timeout=5)
            self.assertIn('ACTUAL_SDK_ZERO_INIT_FAILS_EXPLICIT_INIT_PASSES',r.stdout)

if __name__=='__main__':unittest.main()
