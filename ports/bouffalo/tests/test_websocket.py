"""Run the real XiaoZhi protocol + upstream WebSocket + Bouffalo TLS task.
Only the SDK TLS I/O and FreeRTOS scheduler are simulated; no live credentials.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import PORT, ROOT, CJSON

HARNESS = r"""
#include <cassert>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include "board.h"
#include "protocols/websocket_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "tcp_bouffalo.h"
#include "websocket_support.h"

static int mode, live_tls, requests, hello_count;
static std::atomic<int> pong_count{0};
static std::vector<std::thread> threads;
static thread_local void* task_id = nullptr;
struct MockEvent { std::mutex lock; std::condition_variable changed; unsigned bits=0; };
extern "C" {
EventGroupHandle_t xEventGroupCreate() { return new MockEvent; }
void vEventGroupDelete(EventGroupHandle_t g) { delete g; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits) {
    std::lock_guard<std::mutex> l(g->lock); g->bits |= bits; g->changed.notify_all(); return g->bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits) {
    std::lock_guard<std::mutex> l(g->lock);auto old=g->bits;g->bits&=~bits;return old;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t g) {
    std::lock_guard<std::mutex> l(g->lock);return g->bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g,EventBits_t bits,int clear,int all,unsigned wait) {
    std::unique_lock<std::mutex> l(g->lock);
    auto ready=[&]{return all?(g->bits&bits)==bits:(g->bits&bits)!=0;};
    if(wait==0xffffffffu)g->changed.wait(l,ready);
    else g->changed.wait_for(l,std::chrono::milliseconds(150),ready);
    auto old=g->bits;if(clear && ready())g->bits&=~bits;return old;
}
unsigned xTaskGetTickCount() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void vTaskDelay(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* p,unsigned,void* out) {
    if(mode==7)return 0;
    if(out)*static_cast<void**>(out)=reinterpret_cast<void*>(1);
    threads.emplace_back([=]{task_id=reinterpret_cast<void*>(1);fn(p);});return 1;
}
void vTaskDelete(void*) {}
void* xTaskGetCurrentTaskHandle() { return task_id; }
int WebsocketRandom(void* data,size_t size) {
    if(mode==8)return -1;
    for(size_t i=0;i<size;i++)static_cast<unsigned char*>(data)[i]=i+1;
    return 0;
}
int WebsocketSha1(const void* data,size_t size,unsigned char out[20]) {
    if (std::string(static_cast<const char*>(data),size)=="AQIDBAUGBwgJCgsMDQ4PEA==258EAFA5-E914-47DA-95CA-C5AB0DC85B11") {
        const unsigned char digest[20]={11,253,39,152,120,65,206,212,145,25,29,66,192,190,147,127,134,99,194,150};
        std::memcpy(out,digest,20);return 0;
    }
    if (std::string(static_cast<const char*>(data),size)=="dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11") {
        const unsigned char digest[20]={179,122,79,44,192,98,79,22,144,246,70,6,207,56,89,69,178,190,196,234};
        std::memcpy(out,digest,20);return 0;
    }
    return -1;
}
}
struct MockTls { std::mutex lock;std::deque<char> incoming;std::string pending;bool upgraded=false; };
static std::string frame(const std::string& payload,unsigned opcode=0x81) {
    assert(payload.size()<126);return std::string(1,char(opcode))+char(payload.size())+payload;
}
static std::string accept_key(const std::string& key) {
    assert(key=="AQIDBAUGBwgJCgsMDQ4PEA==");return "C/0nmHhBztSRGR1CwL6Tf4ZjwpY=";
}
static void queue(MockTls* t,const std::string& data) { t->incoming.insert(t->incoming.end(),data.begin(),data.end()); }
extern "C" void* TlsSdkConnect(const char* host,int port) {
    assert(std::strcmp(host,"test.invalid")==0 && port==443);
    if(mode==1)return nullptr;
    ++live_tls;return new MockTls;
}
extern "C" int TlsSdkWrite(void* handle,const void* data,size_t size) {
    if(mode==6)return -1;
    auto t=static_cast<MockTls*>(handle);std::lock_guard<std::mutex> l(t->lock);
    const size_t n=std::min(size,size_t(7));t->pending.append(static_cast<const char*>(data),n);
    if(!t->upgraded) {
        const auto end=t->pending.find("\r\n\r\n");
        if(end==std::string::npos)return n;
        const auto& request=t->pending;
        assert(request.find("GET /xiaozhi/v1/?path=a:b HTTP/1.1\r\n")==0);
        assert(request.find("Authorization: Bearer TEST_SECRET\r\n")!=std::string::npos);
        assert(request.find("Device-Id: test-mac\r\n")!=std::string::npos);
        assert(request.find("Client-Id: test-uuid\r\n")!=std::string::npos);
        assert(request.find("Protocol-Version: 3\r\n")!=std::string::npos);
        auto begin=request.find("Sec-WebSocket-Key: ")+19;auto end_key=request.find("\r\n",begin);
        auto key=request.substr(begin,end_key-begin);
        std::string response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: "+accept_key(key)+"\r\n\r\n";
        if(mode==2)response="HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        if(mode==3)response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: invalid\r\n\r\n";
        queue(t,response);t->pending.clear();t->upgraded=true;++requests;return n;
    }
    while(t->pending.size()>=6) {
        size_t len=static_cast<unsigned char>(t->pending[1])&127;
        size_t header=2;
        assert(t->pending[1]&char(0x80));assert(len<127);
        if(len==126) { len=(static_cast<unsigned char>(t->pending[2])<<8)|static_cast<unsigned char>(t->pending[3]);header=4; }
        if(t->pending.size()<header+4+len)break;
        const unsigned opcode=t->pending[0]&15;std::string payload=t->pending.substr(header+4,len);
        for(size_t i=0;i<len;i++)payload[i]^=t->pending[header+i%4];
        t->pending.erase(0,header+4+len);
        if(opcode==10) { assert(payload=="ping");++pong_count;continue; }
        assert(opcode==1);
        auto root=cJSON_Parse(payload.c_str());assert(root);
        assert(std::strcmp(cJSON_GetObjectItem(root,"type")->valuestring,"hello")==0);
        assert(std::strcmp(cJSON_GetObjectItem(root,"transport")->valuestring,"websocket")==0);
        assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"features"),"mcp")));
        auto audio=cJSON_GetObjectItem(root,"audio_params");assert(cJSON_GetObjectItem(audio,"frame_duration")->valueint==60);
        cJSON_Delete(root);++hello_count;
        if(mode==13) { queue(t,frame("",0x88));continue; }
        if(mode==4)continue; // Missing application-level hello.
        std::string hello=R"({"type":"hello","transport":"websocket","session_id":"test-session","audio_params":{"sample_rate":24000,"frame_duration":60}})";
        if(mode==5)hello=R"({"type":"hello","transport":"mqtt"})";
        if(mode==9) { queue(t,std::string("\x81\x7f",2)+std::string(8,char(255)));continue; }
        if(mode==10) { queue(t,std::string("\x81\x80\x01\x02\x03\x04",6));continue; }
        // Fragment the XiaoZhi hello, interleave a ping, split TCP reads into 17 bytes.
        queue(t,frame(hello.substr(0,std::min(size_t(60),hello.size())),1)+frame("ping",0x89)+frame(hello.substr(std::min(size_t(60),hello.size())),0x80));
    }
    return n;
}
extern "C" int TlsSdkRead(void* handle,void* output,size_t capacity) {
    auto t=static_cast<MockTls*>(handle);std::lock_guard<std::mutex> l(t->lock);
    if(mode==12)return 0;
    if(t->incoming.empty())return -2;
    const size_t n=std::min({capacity,size_t(17),t->incoming.size()});
    for(size_t i=0;i<n;i++) { static_cast<char*>(output)[i]=t->incoming.front();t->incoming.pop_front(); }
    return n;
}
extern "C" void TlsSdkClose(void* handle) { if(handle) { --live_tls;delete static_cast<MockTls*>(handle); } }
Settings::Settings(const std::string& ns,bool rw):ns_(ns),read_write_(rw) {}
Settings::~Settings() {}
std::string Settings::GetString(const std::string& key,const std::string&) {
    if(key=="url")return mode==11?"ws://test.invalid/":"wss://test.invalid/xiaozhi/v1/?path=a:b";
    if(key=="token")return "TEST_SECRET";
    return "";
}
int32_t Settings::GetInt(const std::string&,int32_t) { return 3; }
class TestNetwork : public NetworkInterface {
    std::unique_ptr<Http> CreateHttp(int) override {return nullptr;}
    std::unique_ptr<Tcp> CreateTcp(int) override {assert(false);return nullptr;}
    std::unique_ptr<Tcp> CreateSsl(int) override {return std::make_unique<BouffaloSsl>();}
    std::unique_ptr<Udp> CreateUdp(int) override {return nullptr;}
    std::unique_ptr<Mqtt> CreateMqtt(int) override {return nullptr;}
    std::unique_ptr<WebSocket> CreateWebSocket(int id) override {return std::make_unique<WebSocket>(this,id);}
};
Board::Board():uuid_("test-uuid") {}
Led* Board::GetLed(){return nullptr;} Display* Board::GetDisplay(){return nullptr;} Camera* Board::GetCamera(){return nullptr;}
bool Board::GetTemperature(float&){return false;} bool Board::GetBatteryLevel(int&,bool&,bool&){return false;}
std::string Board::GetSystemInfoJson(){return "{}";}
std::string SystemInfo::GetMacAddress(){return "test-mac";}
class TestBoard : public Board {
    TestNetwork network_;
    std::string GetBoardType() override {return "test";}
    AudioCodec* GetAudioCodec() override {return nullptr;}
    NetworkInterface* GetNetwork() override {return &network_;}
    void StartNetwork() override {}
    const char* GetNetworkStateIcon() override {return "";}
    void SetPowerSaveLevel(PowerSaveLevel) override {}
    std::string GetBoardJson() override {return "{}";}
    std::string GetDeviceStatusJson() override {return "{}";}
};
DECLARE_BOARD(TestBoard)
const char* NetworkError::Name()const{return "test-error";}
const char* NetworkError::Message()const{return "test-error";}
std::string NetworkError::ToString()const{return "test-error";}
int main(int argc,char** argv) {
    assert(argc==2);mode=std::atoi(argv[1]);
    std::string host,port,path;
    assert(ParseSecureWebsocketUri("wss://example.com:8443/a:b?q=x",host,port,path) && host=="example.com" && port=="8443" && path=="/a:b?q=x");
    for(auto bad:{"ws://host/","wss://user:pw@host/","wss://host:abc/","wss://host:0/","wss://host:65536/","wss://host/\r\nX: y"})assert(!ParseSecureWebsocketUri(bad,host,port,path));
    assert(VerifyWebsocketUpgrade("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n","dGhlIHNhbXBsZSBub25jZQ=="));
    {
        WebsocketProtocol protocol;
        assert(protocol.Start());
        bool ok=protocol.OpenAudioChannel();assert(ok==(mode==0));
        if(ok) {
            assert(protocol.session_id()=="test-session" && protocol.server_sample_rate()==24000);
            assert(protocol.IsAudioChannelOpened());
            for(int i=0;i<100 && pong_count==0;i++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(pong_count==1);
            protocol.CloseAudioChannel(false);assert(!protocol.IsAudioChannelOpened());
            // A successful old hello must not make the next failed open succeed.
            mode=4;assert(!protocol.OpenAudioChannel());
        }
    }
    for(auto& thread:threads)thread.join();
    assert(live_tls==0);
    puts("WEBSOCKET_TEST_OK");
}
"""

class WebsocketTests(unittest.TestCase):
    def test_original_protocol_and_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'interfaces').mkdir()
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef unsigned TickType_t;\ntypedef void* TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n#define pdPASS 1\n#define portMAX_DELAY 0xffffffffu\n#define portTICK_PERIOD_MS 1\n#define pdMS_TO_TICKS(x) (x)\n')
            (work/'event_groups.h').write_text('#pragma once\nstruct MockEvent; typedef struct MockEvent* EventGroupHandle_t;typedef unsigned EventBits_t;\nEventGroupHandle_t xEventGroupCreate(void);\nvoid vEventGroupDelete(EventGroupHandle_t);\nEventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);\nEventBits_t xEventGroupGetBits(EventGroupHandle_t);\nEventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,unsigned);\n')
            (work/'task.h').write_text('#pragma once\nunsigned xTaskGetTickCount(void);\nvoid* xTaskGetCurrentTaskHandle(void);\nvoid vTaskDelay(unsigned);\nvoid vTaskDelete(void*);\nint xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*);\n')
            (work/'esp_log.h').write_text('#pragma once\n#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf("[%s] " fmt "\\n",tag,##__VA_ARGS__)\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n#define ESP_LOGD(...) ((void)0)\n')
            for header in (ROOT/'third_party/esp-ml307/include').glob('*.h'):
                text=header.read_text().replace('#include <expected>','#include "platform/expected.h"').replace('std::expected<','xiaozhi::Expected<').replace('std::unexpected(','xiaozhi::Unexpected(')
                if header.name in ('tcp.h','web_socket.h'):
                    text=text.replace('#include <string>','#include <string>\n#include <atomic>').replace('bool connected_ = false;','std::atomic<bool> connected_{false};')
                (work/'interfaces'/header.name).write_text(text)
            subprocess.run(['python3',str(PORT/'prepare_websocket.py'),str(ROOT/'third_party/esp-ml307/src/web_socket.cc'),str(work/'web_socket.cc')],check=True)
            (work/'test.cc').write_text(HARNESS)
            subprocess.run(['gcc','-std=c99','-c',str(CJSON/'cJSON.c'),'-o',str(work/'json.o')],check=True)
            includes=[work,work/'interfaces',PORT,PORT/'compat',ROOT/'main',ROOT/'main/boards/common',ROOT/'third_party/tl-expected/include',CJSON]
            sources=[ROOT/'main/protocols/protocol.cc',ROOT/'main/protocols/websocket_protocol.cc',PORT/'tcp_bouffalo.cc',PORT/'websocket_support.cc',work/'web_socket.cc',work/'test.cc',work/'json.o']
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-DXIAOZHI_PLATFORM_BOUFFALO=1','-pthread',*['-I'+str(x) for x in includes],*[str(x) for x in sources],'-o',str(work/'test')],check=True)
            for mode in range(14):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'test'),str(mode)],check=False,text=True,capture_output=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('WEBSOCKET_TEST_OK',result.stdout)
                    self.assertNotIn('TEST_SECRET',result.stdout)


class TlsBridgeTests(unittest.TestCase):
    def test_verified_tls_parameters_and_nonblocking_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            for name in ('lwip','mbedtls'):(work/name).mkdir()
            (work/'lwip/sockets.h').write_text('#include <sys/socket.h>\n#include <sys/ioctl.h>\n#define setsockopt TestSetSockopt\n#define ioctlsocket TestIoctl\nint TestSetSockopt(int,int,int,const void*,socklen_t);\nint TestIoctl(int,long,void*);\n')
            (work/'bflb_sec_trng.h').write_text('#include <stdint.h>\nint bflb_trng_readlen(uint8_t*,uint32_t);\n')
            (work/'mbedtls/version.h').write_text('#define MBEDTLS_VERSION_MAJOR 3\n')
            (work/'mbedtls/sha1.h').write_text('#include <stddef.h>\nint mbedtls_sha1(const unsigned char*,size_t,unsigned char[20]);\n')
            (work/'check.c').write_text(r"""
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include "tls_sdk_bridge.h"
#include "https_wrapper.h"
static int mode, nonblocking=1, timeout_set, sent, destroyed;
https_wrapper_handle_t https_wrapper_connect(const char* host,uint16_t port,http_wrapper_ssl_param_t* p) {
    assert(strcmp(host,"test.invalid")==0 && port==443 && p);
    assert(p->ca_cert && p->ca_cert_len>1000 && strstr(p->ca_cert,"BEGIN CERTIFICATE"));
    assert(strcmp(p->sni,host)==0);return (void*)1;
}
int https_wrapper_socketfd_get(https_wrapper_handle_t h) {assert(h==(void*)1);return 5;}
int https_wrapper_send(https_wrapper_handle_t h,const void* data,uint16_t n,int flags) {
    assert(h==(void*)1 && !nonblocking && timeout_set && data && n && flags==0);++sent;return mode==1?-1:n;
}
int https_wrapper_recv(https_wrapper_handle_t h,uint8_t* data,uint32_t n,int flags) {
    assert(h==(void*)1 && nonblocking && data && n && flags==0);return mode==2?-EAGAIN:0;
}
int https_wrapper_destroy(https_wrapper_handle_t h) {assert(h==(void*)1);++destroyed;return 0;}
int TestSetSockopt(int fd,int level,int opt,const void* value,socklen_t size) {
    assert(fd==5 && level==SOL_SOCKET && opt==SO_SNDTIMEO && value && size);timeout_set=1;return 0;
}
int TestIoctl(int fd,long command,void* arg) { (void)command;assert(fd==5);nonblocking=*(int*)arg;return 0; }
int bflb_trng_readlen(uint8_t* out,uint32_t n) {(void)out;(void)n;return -9;}
int mbedtls_sha1(const unsigned char* in,size_t n,unsigned char out[20]) {(void)in;(void)n;(void)out;return -8;}
int main(void) {
    char data[16];unsigned char hash[20];
    assert(TlsSdkConnect(NULL,443)==NULL && TlsSdkConnect("test.invalid",0)==NULL);
    void* tls=TlsSdkConnect("test.invalid",443);assert(tls);
    assert(TlsSdkWrite(tls,data,sizeof(data))==sizeof(data) && nonblocking && sent==1);
    mode=1;assert(TlsSdkWrite(tls,data,sizeof(data))==-1 && nonblocking && sent==2);
    mode=2;assert(TlsSdkRead(tls,data,sizeof(data))==-2);mode=0;assert(TlsSdkRead(tls,data,sizeof(data))==0);
    assert(WebsocketRandom(data,sizeof(data))==-9 && WebsocketSha1(data,sizeof(data),hash)==-8);
    TlsSdkClose(tls);TlsSdkClose(NULL);assert(destroyed==1);
}
""")
            http=ROOT.parents[2]/'components/net/lib/http'
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),'-I'+str(http),str(PORT/'tls_sdk_bridge.c'),str(work/'check.c'),'-o',str(work/'check')],check=True)
            subprocess.run([str(work/'check')],check=True,timeout=5)

if __name__=='__main__':unittest.main()
