"""Run the actual Application event loop with a deterministic worker boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_ota_check import PORT, ROOT, CJSON

HARNESS = r"""
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include "application.h"

struct Finished {};
struct MockEvent {std::mutex mutex; std::condition_variable cv; unsigned bits=0;};
static int mode, grants, rounds, starts, events;
static std::thread::id owner;
static std::atomic<bool> finished{false};
static std::thread worker;
extern "C" {
EventGroupHandle_t xEventGroupCreate() {return new MockEvent;}
void vEventGroupDelete(EventGroupHandle_t p) {delete p;}
EventBits_t xEventGroupSetBits(EventGroupHandle_t p,EventBits_t bits) {
    std::lock_guard<std::mutex> lock(p->mutex);p->bits|=bits;p->cv.notify_all();return p->bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t p,EventBits_t mask,int clear,int,unsigned) {
    std::unique_lock<std::mutex> lock(p->mutex);
    if (!p->cv.wait_for(lock,std::chrono::milliseconds(100),[&]{return (p->bits&mask) || finished;}))
        assert(false && "Application did not make progress");
    if(finished && !p->bits)throw Finished{};
    auto bits=p->bits&mask;if(clear)p->bits&=~bits;return bits;
}
TickType_t xTaskGetTickCount() {static unsigned ticks=0;return ++ticks;}
void vTaskPrioritySet(TaskHandle_t,BaseType_t priority) {assert(priority==10);}
BaseType_t uxTaskPriorityGet(TaskHandle_t) {return 10;}
}
// Audio ownership/operation is checked separately with real AudioService/Opus.
AudioService::AudioService() {}
AudioService::~AudioService() {}

class FakeRuntime : public ApplicationRuntime {
    Application* app=nullptr;
    bool enabled=true;
public:
    bool Start(Application& a,AudioService& audio,std::unique_ptr<Protocol>& protocol) override {
        assert(!protocol);
        assert(std::this_thread::get_id()==owner && &audio==&a.GetAudioService());
        app=&a;++starts;
        // All callbacks originate off the owner thread. Before Run(), they must
        // only queue work, not change the state from starting.
        worker=std::thread([&] {
            app->PostRuntimeEvent(ApplicationRuntimeEvent::NetworkReady);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::ActivationReady);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::Connecting);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::SessionReady);
        });
        worker.join();assert(app->GetDeviceState()==kDeviceStateStarting);
        return true;
    }
    void SetListeningEnabled(bool value) override {
        assert(std::this_thread::get_id()==owner);enabled=value;
        if(mode==2 && !value) {
            assert(rounds==0);
            app->StartListening(); // Paused/resumed from the same public API.
        }
    }
    bool ListeningEnabled() const override {return enabled;}
    bool IsVoiceDetected() const override {return false;}
    void DiscardListeningRequest() override {assert(std::this_thread::get_id()==owner);}
    void AllowListening() override {
        assert(std::this_thread::get_id()==owner && enabled);
        assert(app->GetDeviceState()==kDeviceStateListening);++grants;
        if(mode==2 && grants==1) {app->StopListening();return;}
        if(mode==1 && grants==1) {
            app->PostRuntimeEvent(ApplicationRuntimeEvent::Failed);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::Offline);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::NetworkReady);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::ActivationReady);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::Connecting);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::SessionReady);
            return;
        }
        if(++rounds>3) {finished=true;return;}
        worker=std::thread([&] {
            ++events;app->PostRuntimeEvent(ApplicationRuntimeEvent::VoiceStarted);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::WaitingReply);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::Speaking);
            app->PostRuntimeEvent(ApplicationRuntimeEvent::PlaybackFinished);
        });worker.join();
    }
};
std::unique_ptr<ApplicationRuntime> CreateApplicationRuntime() {return std::make_unique<FakeRuntime>();}
int main(int argc,char** argv) {
    assert(argc==2);mode=std::atoi(argv[1]);owner=std::this_thread::get_id();
    auto& app=Application::GetInstance();app.Initialize();app.Initialize();
    try {app.Run();assert(false);} catch(const Finished&) {}
    assert(starts==1 && events==3 && rounds==4);
    assert(grants==(mode==0?4:5));
    assert(app.GetDeviceState()==kDeviceStateListening);
    puts("APPLICATION_TEST_OK");
}
"""

class ApplicationTests(unittest.TestCase):
    def test_real_main_loop_worker_events_and_listening_control(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'freertos').mkdir()
            (work/'interfaces').mkdir()
            (work/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef unsigned TickType_t;typedef void* TaskHandle_t;typedef int BaseType_t;\n#define BIT0 1\n#define BIT1 2\n#define pdTRUE 1\n#define pdFALSE 0\n#define pdMS_TO_TICKS(x) (x)\n#define portMAX_DELAY 0xffffffffu\n')
            (work/'freertos/event_groups.h').write_text('#pragma once\nstruct MockEvent;typedef MockEvent* EventGroupHandle_t;typedef unsigned EventBits_t;\nextern "C" {EventGroupHandle_t xEventGroupCreate();void vEventGroupDelete(EventGroupHandle_t);EventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,unsigned);}\n')
            (work/'freertos/task.h').write_text('#pragma once\nextern "C" {TickType_t xTaskGetTickCount();void vTaskPrioritySet(TaskHandle_t,BaseType_t);BaseType_t uxTaskPriorityGet(TaskHandle_t);}\n')
            (work/'esp_log.h').write_text('#pragma once\n#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf("[%s] " fmt "\\n",tag,##__VA_ARGS__)\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n')
            for header in (ROOT/'third_party/esp-ml307/include').glob('*.h'):
                text=header.read_text().replace('#include <expected>','#include "platform/expected.h"').replace('std::expected<','xiaozhi::Expected<').replace('std::unexpected(','xiaozhi::Unexpected(')
                (work/'interfaces'/header.name).write_text(text)
            (work/'check.cc').write_text(HARNESS)
            includes=[work,work/'interfaces',PORT/'compat',ROOT/'main',ROOT/'main/audio',ROOT/'main/protocols',ROOT/'main/boards/common',ROOT/'third_party/tl-expected/include',CJSON]
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-DXIAOZHI_PLATFORM_BOUFFALO=1','-DXIAOZHI_APPLICATION_PLATFORM_BACKEND=1','-DXIAOZHI_AUDIO_PLATFORM_BACKEND=1','-DXIAOZHI_AUTO_CHAT=1','-DXIAOZHI_VOICE_INPUT_TEST=1',*['-I'+str(p) for p in includes],str(ROOT/'main/application.cc'),str(ROOT/'main/application_platform.cc'),str(ROOT/'main/device_state_machine.cc'),str(work/'check.cc'),'-o',str(work/'check')],check=True)
            for mode in range(3):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'check'),str(mode)],capture_output=True,text=True,timeout=8)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('APPLICATION_TEST_OK',result.stdout)
                    self.assertNotIn('Invalid state transition',result.stdout)
                    self.assertEqual(result.stdout.count('APP_RESUME_LISTENING:'),3)

if __name__=='__main__':unittest.main()
