"""Validate allocation-free diagnostics, nested task callers, and fatal OOM reporting."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import PORT

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "memory_diagnostics.h"
static int mode,depth,allocations;
static void* current=(void*)1;
void* __wrap_pvPortMalloc(size_t);
void* __wrap_kmalloc(size_t,uint32_t);
void vApplicationMallocFailedHook(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
const char* pcTaskGetName(TaskHandle_t task){return task==(void*)1?"audio_output":"other_task";}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task){(void)task;return 256;}
uintptr_t bflb_irq_save(void){return depth++;}
void bflb_irq_restore(uintptr_t flags){assert(depth==int(flags)+1);depth=flags;}
size_t kfree_size(uint32_t heap){return heap==2?7000:4000000;}
size_t kmax_free_size(uint32_t heap){return heap==2?6000:3900000;}
void* __real_kmalloc(size_t n,uint32_t flags) {
    (void)flags;++allocations;
    if(n==6144 || mode==3)return NULL;
    return malloc(n);
}
void* __real_pvPortMalloc(size_t n) {
    if(mode==2 && current==(void*)1) {
        current=(void*)2;void* p=__wrap_pvPortMalloc(16);assert(p);free(p);current=(void*)1;
    }
    void* p=__wrap_kmalloc(n,2);
    if(!p)vApplicationMallocFailedHook();
    return p;
}
int main(int argc,char** argv) {
    assert(argc==2);mode=atoi(argv[1]);setbuf(stdout,NULL);
    MemoryDiagnosticsStage("before_speaker_task_create");assert(allocations==0 && depth==0);
    if(mode==0){void* p=__wrap_pvPortMalloc(16);assert(p);free(p);assert(depth==0);puts("DIAG_OK");}
    else if(mode==3){assert(__wrap_kmalloc(70000,0x41)==NULL);assert(depth==0);puts("PSRAM_FAILURE_RETURNED");}
    else{(void)__wrap_pvPortMalloc(6144);assert(0);}
}
""".replace('int(flags)','(int)flags')

class MemoryDiagnosticsTests(unittest.TestCase):
    def test_caller_task_size_heap_and_stack_reporting(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef void* TaskHandle_t;typedef uint32_t StackType_t;\n')
            (work/'task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nTaskHandle_t xTaskGetCurrentTaskHandle(void);const char* pcTaskGetName(TaskHandle_t);unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);\n')
            (work/'mm.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#define MM_HEAP_OCRAM_0 2\n#define MM_HEAP_PSRAM_0 1\nsize_t kfree_size(uint32_t);size_t kmax_free_size(uint32_t);\n')
            (work/'bflb_irq.h').write_text('#include <stdint.h>\nuintptr_t bflb_irq_save(void);void bflb_irq_restore(uintptr_t);\n')
            (work/'check.c').write_text(HARNESS)
            exe=work/'check'
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),str(PORT/'memory_diagnostics.c'),str(work/'check.c'),'-o',str(exe)],check=True)
            for case in range(4):
                with self.subTest(case=case):
                    result=subprocess.run([str(exe),str(case)],capture_output=True,text=True,timeout=5)
                    self.assertIn('ocram_free=7000 ocram_largest=6000',result.stdout)
                    self.assertIn('psram_free=4000000 psram_largest=3900000 stack_low_water_bytes=1024',result.stdout)
                    if case in (1,2):
                        self.assertNotEqual(result.returncode,0)
                        self.assertRegex(result.stdout,r'RTOS_ALLOC_FAILED: bytes=6144 task=audio_output caller=0x[0-9a-f]+ stage=before_speaker_task_create tracked=1')
                    else:
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        self.assertIn('DIAG_OK' if case==0 else 'PSRAM_FAILURE_RETURNED',result.stdout)
                    if case==3:self.assertIn('KMALLOC_FAILED: bytes=70000 flags=0x41',result.stdout)

if __name__=='__main__':unittest.main()
