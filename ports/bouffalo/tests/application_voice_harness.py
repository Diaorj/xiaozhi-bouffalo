# Integration: actual Application + actual port runtime + real Opus queues.
def customize(t):
    t=t.replace('#include "voice_input.h"','#include "voice_input.h"\n#include "application.h"\n#include "wifi_init.h"')
    a=t.index('static std::set<void*> states,buffers;');t=t[:a]+r'''
struct ApplicationFinished {};
static std::atomic<bool> application_finished{false};
static std::thread::id application_owner;
static std::thread network_worker;
static int application_case, network_starts;
static std::atomic<unsigned> permissions{0};
static std::atomic<bool> pause_requested{false};
'''+t[a:]
    t=t.replace('    std::unique_lock<std::mutex> l(g->lock);',r'''    if(application_finished && std::this_thread::get_id()==application_owner)
        throw ApplicationFinished{};
    std::unique_lock<std::mutex> l(g->lock);''')
    t=t.replace('void vTaskDelete(void*) {}',r'''void vTaskDelete(void*) {}
void vTaskPrioritySet(TaskHandle_t,BaseType_t priority) {assert(priority==10);}
BaseType_t uxTaskPriorityGet(TaskHandle_t) {return 10;}''')
    t=t.replace('    if(test_case==20)return -1;',r'''    assert(Application::GetInstance().GetDeviceState()==kDeviceStateListening);
    if(test_case==20)return -1;''')
    t=t.replace('    unsigned offset=captured.load();','''    unsigned offset=captured.load();
    if(application_case==2 && offset>=12800 && !pause_requested.exchange(true))
        Application::GetInstance().StopListening();''')
    t=t.replace('    played.insert(played.end(),pcm,pcm+n);return n;','''    if(application_case==3 && !pause_requested.exchange(true)) {
        Application::GetInstance().StopListening();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    played.insert(played.end(),pcm,pcm+n);return n;''')
    t=t[:t.index('int main(int argc,char** argv)')]+r''' 
struct HookProbe {const ApplicationSessionHooks* original;};
bool StartWifiInitialization(const char* ssid,const char* password,const ApplicationSessionHooks* hooks) {
    assert(ssid && password && hooks && hooks->audio_service==&Application::GetInstance().GetAudioService());
    assert(network_starts++==0);
    network_worker=std::thread([hooks] {
        HookProbe probe{hooks};ApplicationSessionHooks observed=*hooks;observed.context=&probe;
        observed.post_event=[](void* p,ApplicationRuntimeEvent e) {
            auto h=static_cast<HookProbe*>(p)->original;h->post_event(h->context,e);
        };
        observed.take_listening_request=[](void* p) {
            auto h=static_cast<HookProbe*>(p)->original;
            const bool granted=h->take_listening_request(h->context);
            if(granted)++permissions;
            return granted;
        };
        observed.listening_enabled=[](void* p) {
            auto h=static_cast<HookProbe*>(p)->original;return h->listening_enabled(h->context);
        };
        auto post=[&](ApplicationRuntimeEvent e){observed.post_event(observed.context,e);};
        post(ApplicationRuntimeEvent::NetworkReady);post(ApplicationRuntimeEvent::ActivationReady);
        if(application_case) {
            test_case=application_case==1?34:0;
            assert(!RunOriginalWebsocketHandshake(1,WifiAvailable,&observed));
            assert(live_tls==0 && !mic && !capture_pending && !encoder_pending && buffers.empty());
            post(ApplicationRuntimeEvent::Failed);
            if(application_case==1)post(ApplicationRuntimeEvent::Offline);
            for(auto& thread:threads)if(thread.joinable())thread.join();
            if(server_decoder) {opus_decoder_destroy(server_decoder);server_decoder=nullptr;}
            test_case=0;wifi_down=false;closed_audio=0;
            if(application_case==1) {
                post(ApplicationRuntimeEvent::NetworkReady);post(ApplicationRuntimeEvent::ActivationReady);
            } else Application::GetInstance().StartListening();
        }
        assert(RunOriginalWebsocketHandshake(3,WifiAvailable,&observed));
        assert(live_tls==0 && !mic && !capture_pending && !encoder_pending && buffers.empty());
        application_finished=true;
        Application::GetInstance().Schedule([]{});
    });
    return true;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);assert(argc==2);application_case=std::atoi(argv[1]);test_case=0;
    std::atexit([] {assert(state_allocations==state_frees && states.empty() && buffers.empty() && !mic && !capture_pending && !encoder_pending);});
    application_owner=std::this_thread::get_id();
    auto& app=Application::GetInstance();app.Initialize();
    try {app.Run();assert(false);} catch(const ApplicationFinished&) {}
    network_worker.join();for(auto& thread:threads)if(thread.joinable())thread.join();
    assert(network_starts==1 && rounds==(application_case==3?4:3) && finished_audio==3 && requests==(application_case?2:1));
    assert(permissions==unsigned(application_case?4:3));
    assert(state_allocations==state_frees+1 && states.size()==1);
    puts("APPLICATION_VOICE_TEST_OK");
}
'''
    return t
