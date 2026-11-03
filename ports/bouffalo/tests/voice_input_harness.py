# Simulated capture/network, actual original queues and real Opus, over many rounds.
def customize(t):
    t=t.replace('#include "audio_sdk_bridge.h"','#include "audio_sdk_bridge.h"\n#include "voice_input.h"\n#include "audio_dma_worker.h"\n#include "audio_encoder_worker.h"\n#include <set>')
    a=t.index('static int state_allocations='); b=t.index('static BouffaloAudioCodec test_codec;',a)
    t=t[:a]+r'''
static std::set<void*> states,buffers;
static int state_allocations=0,state_frees=0,rounds=0,cloud_starts=0;
static std::atomic<bool> mic{false},force_disconnect{false};
static std::atomic<unsigned> captured{0};
static unsigned uploaded=0,decoded_input=0;
static int input_peak=0;
static OpusDecoder* server_decoder=nullptr;
static std::thread capture_thread,encoder_thread;
static bool capture_pending=false,encoder_pending=false;
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
extern "C" void* AudioSdkAllocateBuffer(size_t n) {
    if(test_case==22)return nullptr;
    void* p=malloc(n);assert(p);buffers.insert(p);return p;
}
extern "C" void AudioSdkFreeBuffer(void* p) {
    if(p) {assert(buffers.erase(p)==1);free(p);}
}
extern "C" int AudioEncoderWorkerStart(void(*job)(void*),void* context,TaskHandle_t* handle) {
    if(test_case==24)return -1;
    assert(!encoder_pending);encoder_pending=true;*handle=reinterpret_cast<void*>(2);
    encoder_thread=std::thread(job,context);return 0;
}
extern "C" int AudioEncoderWorkerWait() {
    assert(encoder_pending);encoder_thread.join();encoder_pending=false;return 0;
}
extern "C" int AudioDmaWorkerStart(void (*job)(void*),void* context) {
    assert(!capture_pending);capture_pending=true;
    capture_thread=std::thread(job,context);return 0;
}
extern "C" int AudioDmaWorkerWait() {
    assert(capture_pending);capture_thread.join();capture_pending=false;return 0;
}
'''+t[b:]
    t=t.replace('extern "C" int AudioSdkOpen(unsigned volume) { assert(volume==15);',
                'extern "C" int AudioSdkOpen(unsigned volume) { assert(!mic && !capture_pending && !encoder_pending && states.size()==1); assert(volume==15);')
    t=t.replace('extern "C" int AudioSdkRecordOpen() { return -1; }',r'''extern "C" int AudioSdkRecordOpen() {
    if(test_case==20)return -1;
    assert(!mic);captured=0;mic=true;return 0;
}''')
    t=t.replace('extern "C" int AudioSdkRead(int16_t*,size_t) { return -1; }',r'''extern "C" int AudioSdkRead(int16_t* pcm,size_t n) {
    assert(mic && n==160);
    if(test_case==21 && captured>=10000)return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    unsigned offset=captured.load();
    bool voice=offset>=9600 && (offset<25600 || test_case==31);
    if(test_case==32)voice=(offset>=6400 && offset<7680) || (offset>=48000 && offset<64000);
    if(test_case==33) {voice=false;if(offset>=48000)force_disconnect=true;}
    for(size_t i=0;i<n;i++)pcm[i]=voice?int16_t(9000*std::sin((offset+i)*6.283185307*440/16000)):0;
    captured+=n;return n;
}''')
    t=t.replace('extern "C" void AudioSdkRecordClose() {}','extern "C" void AudioSdkRecordClose() {assert(mic);mic=false;}')
    t=t.replace('extern "C" int AudioEncoderWorkerStart(void(*)(void*),void*,TaskHandle_t*) {return -1;}','')
    t=t.replace('extern "C" int AudioEncoderWorkerWait() {return -1;}','')
    t=t.replace('    if(closed_audio)return 0;','    if(closed_audio || force_disconnect)return 0;')
    t=t.replace('        assert(opcode==1);',r'''
        if(opcode==2) {
            assert(captured>=960);
            if(uploaded==0)assert(mic); // The first packet must be sent while recording.
            if(test_case==25 && uploaded==3)return -1;
            if(test_case==26 && uploaded==3)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if(test_case==27 && uploaded==3)queue(t,frame("",0x88));
            assert(uploaded<167);
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
                assert(mic && captured>=12160 && server_decoder==nullptr);
                if(test_case==32)assert(captured>=50560); // The earlier 80 ms click was ignored.
                ++cloud_starts;uploaded=decoded_input=0;input_peak=0;
                int error=0;server_decoder=opus_decoder_create(16000,1,&error);assert(error==0);
                cJSON_Delete(root);return n;
            }
            assert(std::strcmp(state,"stop")==0);
            assert(uploaded>10 && decoded_input==uploaded*960 && input_peak>1000);
            if(test_case==31)assert(uploaded==167); // Exact ten-second clip + final pad.
            assert(!mic && !capture_pending && !encoder_pending && states.size()==1);
            assert(buffers.empty());
            opus_decoder_destroy(server_decoder);server_decoder=nullptr;++rounds;
            queue(t,frame(R"({"type":"stt","text":"test utterance"})"));''')
    t=t[:t.index('int main(int argc,char** argv)')]+r'''
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);assert(argc==2);test_case=std::atoi(argv[1]);
    std::atexit([] {assert(state_allocations==state_frees && states.empty() && buffers.empty() && !mic && !capture_pending && !encoder_pending);});
    const bool expected=test_case==0 || test_case==30 || test_case==31 || test_case==32;
    const bool ok=RunOriginalWebsocketHandshake(test_case==30?20:1);
    assert(ok==expected);
    assert(live_tls==0 && !mic && !capture_pending && !encoder_pending && buffers.empty());
    for(auto& thread:threads)thread.join();
    if(server_decoder)opus_decoder_destroy(server_decoder);
    if(expected) {
        int n=test_case==30?20:1;
        assert(finished_audio==n && played.size()==size_t(n)*6*960 && rounds==n && cloud_starts==n);
        assert(requests==1 && close_count==1); // One authenticated connection across all rounds.
        assert(state_allocations==n+1 && state_frees==n); // One retained decoder, no growing state.
    }
    if(test_case>=20 && test_case!=30 && test_case!=31 && test_case!=32)assert(audio_tasks==0);
    if(test_case==33)assert(cloud_starts==0 && uploaded==0); // Quiet waiting never asks the server.
    puts("VOICE_TEST_OK");
}
'''
    return t
