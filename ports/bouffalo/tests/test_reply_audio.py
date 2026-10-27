"""Use the real SDK Opus, original protocol, and BL AudioCodec with a fake speaker.
The transport simulator emits 24 kHz Opus; decoded output must be 16 kHz PCM.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_ota_check import PORT, ROOT, CJSON
from test_websocket import HARNESS as WS_HARNESS

class ReplyAudioTests(unittest.TestCase):
    def test_full_reply_with_real_sdk_opus(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            opus=ROOT.parents[2]/'components/multimedia/opus'
            cmake=(opus/'CMakeLists.txt').read_text()
            sources=re.findall(r'^    (src/[^\s]+\.c)$',cmake,re.M)
            self.assertGreater(len(sources),100)
            # Compile exactly the SDK's source set using generic fixed-point C.
            (work/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.15)\nproject(host_opus C)\nadd_library(host_opus STATIC\n'+''.join(str(opus/x)+'\n' for x in sources)+')\ntarget_compile_definitions(host_opus PRIVATE HAVE_CONFIG_H FIXED_POINT)\ntarget_compile_options(host_opus PRIVATE -O2 -w)\ntarget_include_directories(host_opus PRIVATE '+ ' '.join(str(opus/x) for x in ['include','include/opus','src','src/celt','src/silk','src/silk/fixed','src/silk/float'])+')\n')
            subprocess.run(['cmake','-S',str(work),'-B',str(work/'build')],check=True,stdout=subprocess.DEVNULL)
            subprocess.run(['cmake','--build',str(work/'build'),'-j4'],check=True,stdout=subprocess.DEVNULL)
            (work/'interfaces').mkdir()
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef unsigned TickType_t;typedef void* TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n#define pdPASS 1\n#define portMAX_DELAY 0xffffffffu\n#define portTICK_PERIOD_MS 1\n#define pdMS_TO_TICKS(x) (x)\n')
            (work/'event_groups.h').write_text('#pragma once\nstruct MockEvent;typedef struct MockEvent* EventGroupHandle_t;typedef unsigned EventBits_t;\nEventGroupHandle_t xEventGroupCreate(void);void vEventGroupDelete(EventGroupHandle_t);EventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);EventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);EventBits_t xEventGroupGetBits(EventGroupHandle_t);EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,unsigned);\n')
            (work/'task.h').write_text('#pragma once\nunsigned xTaskGetTickCount(void);void* xTaskGetCurrentTaskHandle(void);void vTaskDelay(unsigned);void vTaskDelete(void*);int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*);\n')
            (work/'esp_log.h').write_text('#pragma once\n#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf("[%s] " fmt "\\n",tag,##__VA_ARGS__)\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n#define ESP_LOGD(...) ((void)0)\n')
            for header in (ROOT/'third_party/esp-ml307/include').glob('*.h'):
                t=header.read_text().replace('#include <expected>','#include "platform/expected.h"').replace('std::expected<','xiaozhi::Expected<').replace('std::unexpected(','xiaozhi::Unexpected(')
                if header.name in ('tcp.h','web_socket.h'):
                    t=t.replace('#include <string>','#include <string>\n#include <atomic>').replace('bool connected_ = false;','std::atomic<bool> connected_{false};')
                (work/'interfaces'/header.name).write_text(t)
            subprocess.run(['python3',str(PORT/'prepare_websocket.py'),str(ROOT/'third_party/esp-ml307/src/web_socket.cc'),str(work/'web_socket.cc')],check=True)
            t=WS_HARNESS[:WS_HARNESS.index('int main(int argc,char** argv)')]
            t=t.replace('#include "board.h"','#include "board.h"\n#include "audio_codec_bouffalo.h"\n#include "audio_sdk_bridge.h"\n#include "websocket_bootstrap.h"\n#include <opus/opus.h>\n#include <cmath>\n#include "audio/audio_service.h"\n#include "opus_decoder_bouffalo.h"')
            # Speaker bridge is the only simulated audio stage.
            i=t.index('static int mode, live_tls')
            t=t[:i]+r"""
static std::atomic<int> finished_audio{0}, abort_audio{0}, closed_audio{0};
static std::atomic<int> test_case{0};
static std::vector<int16_t> played;
static std::atomic<bool> output_entered{false}, release_output{false};
static std::atomic<int> audio_tasks{0};
static int state_allocations=0,state_frees=0;
static void* allocated_state=nullptr;
extern "C" void MemoryDiagnosticsStage(const char*) {}
extern "C" void* AudioSdkAllocateOpusState(size_t bytes) {
    assert(bytes==size_t(opus_decoder_get_size(1)));
    if(test_case==17)return nullptr;
    assert(!allocated_state && posix_memalign(&allocated_state,32,bytes)==0);
    std::memset(allocated_state,0xa5,bytes);++state_allocations;return allocated_state;
}
extern "C" void AudioSdkFreeOpusState(void* state) {
    assert(state && state==allocated_state);std::free(state);allocated_state=nullptr;++state_frees;
}
static BouffaloAudioCodec test_codec;
extern "C" int AudioSdkOpen(unsigned volume) { assert(volume==15);return test_case==3?-1:0; }
extern "C" int AudioSdkWrite(const int16_t* pcm,size_t n) {
    if(test_case==4)return -1;
    if(test_case==12 || test_case==13) {
        output_entered=true;
        while(!release_output)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    played.insert(played.end(),pcm,pcm+n);return n;
}
extern "C" int AudioSdkFinish() { ++finished_audio;return test_case==9?-1:0; }
extern "C" void AudioSdkAbort() { ++abort_audio; }
"""+t[i:]
            t=t.replace('static int mode, live_tls, requests, hello_count;', 'static int mode, live_tls, requests;')
            t=t.replace('int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* p,unsigned,void* out) {', r"""int xTaskCreate(void(*fn)(void*),const char* name,unsigned,void* p,unsigned,void* out) {
    if(std::strcmp(name,"audio_output")==0 || std::strcmp(name,"opus_codec")==0)++audio_tasks;
    if((test_case==10 && std::strcmp(name,"audio_output")==0) ||
       (test_case==11 && std::strcmp(name,"opus_codec")==0))return 0;""")

            t=t.replace('AudioCodec* GetAudioCodec() override {return nullptr;}','AudioCodec* GetAudioCodec() override {return &test_codec;}')
            # Original codec uses Settings only for optional persistence (not started here).
            i=t.index('class TestNetwork :')
            t=t[:i]+'void Settings::SetInt(const std::string&,int32_t) {}\n'+t[i:]
            t=t.replace('assert(payload.size()<126);return std::string(1,char(opcode))+char(payload.size())+payload;', 'assert(payload.size()<65536);if(payload.size()<126)return std::string(1,char(opcode))+char(payload.size())+payload;return std::string(1,char(opcode))+char(126)+char(payload.size()>>8)+char(payload.size())+payload;')
            i=t.index('        assert(opcode==1);');j=t.index('\n    }\n    return n;',i)
            t=t[:i]+r"""
        assert(opcode==1);
        auto root=cJSON_Parse(payload.c_str());assert(root);
        const char* type=cJSON_GetObjectItem(root,"type")->valuestring;
        if(std::strcmp(type,"hello")==0) {
            queue(t,frame(R"({"type":"hello","transport":"websocket","audio_params":{"sample_rate":24000,"frame_duration":60}})"));
        } else {
            assert(std::strcmp(type,"listen")==0);
            assert(std::strcmp(cJSON_GetObjectItem(root,"state")->valuestring,"detect")==0);
            assert(cJSON_IsString(cJSON_GetObjectItem(root,"text")));
            assert(std::strcmp(cJSON_GetObjectItem(root,"text")->valuestring,"你好小智")==0);
            if(test_case==7) {
                // Exact alert captured from the official service on 2026-10-23.
                queue(t,frame(R"({"type":"alert","status":"ERROR","message":"Detect is only for wake words, do not send long texts.","emotion":"sad"})"));
                cJSON_Delete(root);return n;
            }
            queue(t,frame(R"({"type":"tts","state":"start"})"));
            if(test_case==14) { cJSON_Delete(root);return n; }
            int error=0;
            OpusEncoder* encoder=opus_encoder_create(24000,1,OPUS_APPLICATION_VOIP,&error);assert(error==0);
            int16_t pcm[1440];
            for(unsigned i=0;i<1440;i++)pcm[i]=int16_t(8000*std::sin(i*6.283185307*440/24000));
            unsigned char encoded[4096];
            assert(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(16000))==0);
            for(unsigned i=0;i<6;i++) {
                int bytes=opus_encode(encoder,pcm,1440,encoded,sizeof(encoded));assert(bytes>0 && bytes<4096);
                if(test_case==1) bytes=0;
                std::string audio(reinterpret_cast<char*>(encoded),bytes);
                if(test_case==8)audio=std::string(1,char(255));
                if(test_case==15)continue;
                if(test_case==5) {
                    std::string header(16,0);header[1]=2;header[14]=char(bytes>>8);header[15]=char(bytes);
                    audio=header+audio;
                } else if(test_case==6) {
                    std::string header(4,0);header[2]=char(bytes>>8);header[3]=char(bytes);
                    audio=header+audio;
                }
                queue(t,frame(audio,0x82));
            }
            opus_encoder_destroy(encoder);
            if(test_case==2) {
                // Close before TTS stop: decoder must cancel, not report success.
                queue(t,frame("",0x88));
            } else {
                queue(t,frame(R"({"type":"tts","state":"stop"})"));
                if(test_case==16)queue(t,frame("",0x88));
            }
        }
        cJSON_Delete(root);
"""+t[j:]
            t=t.replace('int32_t Settings::GetInt(const std::string&,int32_t) { return 3; }', 'int32_t Settings::GetInt(const std::string&,int32_t) { return test_case==5?2:test_case==6?3:1; }')
            t=t.replace('assert(request.find("Protocol-Version: 3\\r\\n")!=std::string::npos);', 'assert(request.find("Protocol-Version: "+std::to_string(test_case==5?2:test_case==6?3:1)+"\\r\\n")!=std::string::npos);')
            t=t.replace('    if(mode==12)return 0;','    if(closed_audio)return 0;')
            t+=r"""
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    assert(argc==2);test_case=std::atoi(argv[1]);
    std::atexit([]{assert(state_allocations==state_frees);});
    if(test_case==12 || test_case==13) {
        // Real AudioService: hold the speaker while testing bounded queueing and Stop.
        AudioService audio;
        audio.Initialize(&test_codec,CreateBouffaloOpusDecoder());
        audio.Start();assert(audio.IsRunning() && audio_tasks==2);
        assert(!audio.PushPacketToDecodeQueue(nullptr));
        int error=0;
        auto* encoder=opus_encoder_create(24000,1,OPUS_APPLICATION_VOIP,&error);assert(error==0);
        int16_t pcm[1440]={0};unsigned char encoded[4096];
        int bytes=opus_encode(encoder,pcm,1440,encoded,sizeof(encoded));assert(bytes>0);
        opus_encoder_destroy(encoder);
        auto packet=[&](bool padded=false) {
            auto p=std::make_unique<AudioStreamPacket>();p->sample_rate=24000;p->frame_duration=60;
            p->payload.assign(encoded,encoded+bytes);
            if(padded) {p->payload.resize(4096);assert(opus_packet_pad(p->payload.data(),bytes,4096)==0);}
            return p;
        };
        assert(audio.PushPacketToDecodeQueue(packet()));
        for(int i=0;i<1000 && !output_entered;i++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(output_entered && !audio.IsPlaybackIdle() && !audio.FinishPlayback());
        unsigned accepted=0;
        while(accepted<200 && audio.PushPacketToDecodeQueue(packet(test_case==12)))++accepted;
        assert(accepted>= (test_case==12?16:128) && accepted< (test_case==12?24:136));
        // Stop must wait for the in-flight output to return, then abort/free exactly once.
        std::atomic<bool> stopped{false};
        std::thread stopper([&]{audio.Stop();stopped=true;});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(!stopped);
        release_output=true;stopper.join();assert(!audio.IsRunning());
        assert(abort_audio==1 && finished_audio==0 && !audio.PushPacketToDecodeQueue(packet()));
        audio.Start();assert(audio.IsRunning() && audio.IsPlaybackIdle());audio.ResetDecoder();
        assert(audio.PushPacketToDecodeQueue(packet()));
        for(int i=0;i<1000 && !audio.IsPlaybackIdle();i++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(audio.IsPlaybackIdle() && finished_audio==0);
        assert(audio.FinishPlayback() && finished_audio==1);
        assert(!audio.PushPacketToDecodeQueue(packet()));
        audio.Stop();assert(abort_audio==1);
        for(auto& thread:threads)thread.join();
        puts("FULL_AUDIO_LIFECYCLE_OK");return 0;
    }
    const bool ok=RunOriginalWebsocketHandshake();
    assert(ok==(test_case==0 || test_case==5 || test_case==6 || test_case==16));
    // The worker's success/failure log is checked by the outer test.
    for(int i=0;i<3000 && test_case!=10 && test_case!=11 && test_case!=17 && finished_audio==0 && abort_audio==0;i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    closed_audio=1;
    for(auto& thread:threads)thread.join();
    if(test_case==0 || test_case==5 || test_case==6 || test_case==16) {
        assert(finished_audio==1 && abort_audio==0 && played.size()==6*960);
        int peak=0;for(auto v:played)peak=std::max(peak,std::abs(int(v)));assert(peak>1000);
    }
    puts("FULL_AUDIO_TEST_OK");
}
"""
            (work/'check.cc').write_text(t)
            subprocess.run(['gcc','-std=c99','-c',str(CJSON/'cJSON.c'),'-o',str(work/'json.o')],check=True)
            include=[work,work/'interfaces',PORT,PORT/'compat',ROOT/'main',ROOT/'main/boards/common',ROOT/'main/protocols',ROOT/'third_party/tl-expected/include',CJSON,opus/'include']
            sources=[ROOT/'main/protocols/protocol.cc',ROOT/'main/protocols/websocket_protocol.cc',ROOT/'main/audio/audio_codec.cc',ROOT/'main/audio/audio_service.cc',*[PORT/x for x in ['tcp_bouffalo.cc','websocket_support.cc','opus_decoder_bouffalo.cc','audio_codec_bouffalo.cc','websocket_bootstrap.cc']],work/'web_socket.cc',work/'check.cc',work/'json.o',work/'build/libhost_opus.a']
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-pthread','-DXIAOZHI_PLATFORM_BOUFFALO=1','-DXIAOZHI_AUDIO_PLAYBACK_ONLY=1',*['-I'+str(x) for x in include],*[str(x) for x in sources],'-lm','-o',str(work/'check')],check=True)
            for case in range(18):
                with self.subTest(case=case):
                    result=subprocess.run([str(work/'check'),str(case)],capture_output=True,text=True,timeout=25)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    if case==17:
                        self.assertIn('OPUS_PSRAM_ALLOC_FAILED:',result.stdout)
                        continue
                    if case in (12,13):
                        self.assertIn('FULL_AUDIO_LIFECYCLE_OK',result.stdout)
                        continue
                    if case in (10,11):
                        self.assertIn('AUDIO_WORKER_START_FAILED',result.stdout)
                        continue
                    self.assertIn('AUDIO_SERVICE_READY:',result.stdout)
                    if case==14:self.assertIn('no audio received within 15s',result.stdout)
                    if case==7:
                        self.assertIn('SERVER_ALERT: status=ERROR',result.stdout)
                        self.assertIn('Detect is only for wake words',result.stdout)
                        self.assertIn('server or network error',result.stdout)
                        self.assertNotIn('no audio received within 15s',result.stdout)
                    self.assertIn('FULL_AUDIO_TEST_OK',result.stdout)
                    self.assertEqual('AUDIO_REPLY_DONE:' in result.stdout,case in (0,5,6,16),result.stdout)
                    self.assertEqual('AUDIO_REPLY_FAILED:' in result.stdout,case not in (0,5,6,16),result.stdout)


class PcmRingTests(unittest.TestCase):
    def test_dma_slots_volume_wrap_and_silence(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'check.c').write_text(r"""
#include <assert.h>
#include <stdint.h>
#include "pcm_ring.h"
int main(void) {
    int16_t ring[8]={10000,-10000,32767,-32768,1,-1,5000,-5000};uint32_t out[12];
    assert(PcmRingPack(ring,8,88,80,out,12,20)==8);
    assert((int16_t)out[0]==2000 && (int16_t)out[1]==-2000);
    assert((int16_t)out[2]==6553 && (int16_t)out[3]==-6553);
    for(unsigned i=0;i<12;i++) {assert((uint16_t)out[i]==(uint16_t)(out[i]>>16));if(i>=8)assert(out[i]==0);}
    assert(PcmRingPack(ring,8,90,86,out,6,20)==4);
    assert((int16_t)out[0]==1000 && (int16_t)out[1]==-1000 && (int16_t)out[2]==2000);
    assert(PcmRingPack(ring,8,82,80,out,6,0)==2);for(unsigned i=0;i<6;i++)assert(out[i]==0);
    assert(PcmRingPack(ring,8,100,80,out,6,20)==0);for(unsigned i=0;i<6;i++)assert(out[i]==0);
    assert(PcmRingPack(ring,8,8,0,out,6,20)==6 && out[0]==0); // Fade-in.
    assert(PcmRingPack(NULL,0,1,0,out,6,20)==0);for(unsigned i=0;i<6;i++)assert(out[i]==0);
}
""")
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(PORT),str(PORT/'pcm_ring.c'),str(work/'check.c'),'-o',str(work/'check')],check=True)
            subprocess.run([str(work/'check')],check=True,timeout=5)

if __name__=='__main__':unittest.main()
