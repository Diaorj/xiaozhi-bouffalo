# Extend the common wire/audio harness with capture and upstream assertions.
def customize(t):
    t=t.replace('#include "audio_sdk_bridge.h"','#include "audio_sdk_bridge.h"\n#include "voice_input.h"\n#include <set>')
    a=t.index('static int state_allocations='); b=t.index('static BouffaloAudioCodec test_codec;',a)
    t=t[:a]+r'''
static std::set<void*> states,buffers;
static int state_allocations=0,state_frees=0;
static bool mic=false;
static unsigned captured=0,uploaded=0,decoded_input=0;
static int input_peak=0;
static OpusDecoder* server_decoder=nullptr;
extern "C" void MemoryDiagnosticsStage(const char*) {}
extern "C" void* AudioSdkAllocateOpusState(size_t bytes) {
    assert(bytes==size_t(opus_decoder_get_size(1)) || bytes==size_t(opus_encoder_get_size(1)));
    if(test_case==23 && bytes==size_t(opus_encoder_get_size(1)))return nullptr;
    void* p=nullptr;assert(posix_memalign(&p,32,bytes)==0);
    states.insert(p);++state_allocations;return p;
}
extern "C" void AudioSdkFreeOpusState(void* p) {
    assert(states.erase(p)==1);free(p);++state_frees;
}
extern "C" void* AudioSdkAllocateBuffer(size_t bytes) {
    if(test_case==22)return nullptr;
    void* p=malloc(bytes);assert(p);buffers.insert(p);return p;
}
extern "C" void AudioSdkFreeBuffer(void* p) {
    if(p) {assert(buffers.erase(p)==1);free(p);}
}
'''+t[b:]
    t=t.replace('extern "C" int AudioSdkRecordOpen() { return -1; }',r'''extern "C" int AudioSdkRecordOpen() {
    if(test_case==20)return -1;
    assert(!mic);mic=true;return 0;
}''')
    t=t.replace('extern "C" int AudioSdkRead(int16_t*,size_t) { return -1; }',r'''extern "C" int AudioSdkRead(int16_t* pcm,size_t n) {
    assert(mic && n==160);
    if(test_case==21 && captured>=10000)return -1;
    for(size_t i=0;i<n;i++)pcm[i]=int16_t(9000*std::sin((captured+i)*6.283185307*440/16000));
    captured+=n;return n;
}''')
    t=t.replace('extern "C" void AudioSdkRecordClose() {}','extern "C" void AudioSdkRecordClose() {assert(mic);mic=false;}')
    t=t.replace('if((test_case==10', 'if((test_case==24 && std::strcmp(name,"opus_encode")==0) || (test_case==10')
    t=t.replace('        assert(opcode==1);',r'''
        if(opcode==2) {
            assert(!mic && captured==80000);
            if(test_case==25 && uploaded==3)return -1;
            assert(uploaded<84);
            int16_t decoded[960];
            int count=opus_decode(server_decoder,reinterpret_cast<const unsigned char*>(payload.data()),payload.size(),decoded,960,0);
            assert(count==960);decoded_input+=count;
            for(int16_t sample:decoded)input_peak=std::max(input_peak,std::abs(int(sample)));
            ++uploaded;continue;
        }
        assert(opcode==1);''')
    old='''            assert(std::strcmp(cJSON_GetObjectItem(root,"state")->valuestring,"detect")==0);
            assert(cJSON_IsString(cJSON_GetObjectItem(root,"text")));
            assert(std::strcmp(cJSON_GetObjectItem(root,"text")->valuestring,"你好小智")==0);'''
    assert old in t
    t=t.replace(old,r'''
            const char* state=cJSON_GetObjectItem(root,"state")->valuestring;
            if(std::strcmp(state,"start")==0) {
                assert(std::strcmp(cJSON_GetObjectItem(root,"mode")->valuestring,"manual")==0);
                assert(states.size()==1 && buffers.size()==1 && !mic);
                int error=0;server_decoder=opus_decoder_create(16000,1,&error);assert(error==0);
                cJSON_Delete(root);return n;
            }
            assert(std::strcmp(state,"stop")==0);
            assert(uploaded==84 && decoded_input==80640 && input_peak>1000);
            opus_decoder_destroy(server_decoder);server_decoder=nullptr;
            queue(t,frame(R"({"type":"stt","text":"test utterance"})"));''')
    t=t[:t.index('int main(int argc,char** argv)')]+r'''
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);assert(argc==2);test_case=std::atoi(argv[1]);
    std::atexit([] {assert(state_allocations==state_frees && states.empty() && buffers.empty() && !mic);});
    const bool ok=RunOriginalWebsocketHandshake();
    assert(ok==(test_case==0));
    assert(live_tls==0 && buffers.empty() && !mic);
    for(auto& thread:threads)thread.join();
    if(server_decoder)opus_decoder_destroy(server_decoder);
    if(test_case==0)assert(finished_audio==1 && played.size()==6*960 && uploaded==84);
    if(test_case>=20 && test_case<=24)assert(live_tls==0 && uploaded==0 && audio_tasks==0);
    puts("VOICE_TEST_OK");
}
'''
    return t
