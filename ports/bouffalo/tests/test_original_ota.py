from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from test_ota_check import CJSON, PORT, ROOT

HARNESS = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/time.h>
#include "ota_bootstrap.h"
#include "http_bouffalo.h"
#include "audio_codec_bouffalo.h"
#include "ota.h"
#include "settings.h"
#include "http_sdk_bridge.h"
#include "lfs_kv.h"
#include "wifi_init.h"
std::unique_ptr<Tcp> BouffaloNetwork::CreateSsl(int) { return nullptr; }
std::unique_ptr<WebSocket> BouffaloNetwork::CreateWebSocket(int) { return nullptr; }
BouffaloAudioCodec::BouffaloAudioCodec() {}
bool BouffaloAudioCodec::FinishOutput(){return true;}
void BouffaloAudioCodec::Start() {}
void BouffaloAudioCodec::EnableOutput(bool) {}
int BouffaloAudioCodec::Write(const int16_t*,int) { return -1; }
static int mode, calls;
static long utc;
extern "C" {
void bflb_mtd_init() {}
lfs_kv_err_t lfs_kv_init() { return mode == 9 ? LFS_KV_ERR_INIT_FAILED : LFS_KV_OK; }
size_t lfs_kv_get_blob(const char* key, void* out, size_t capacity, size_t* saved) {
    std::ifstream f(std::string(std::getenv("STATE_DIR"))+"/"+key,std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)),{});
    if(saved) *saved=text.size();
    size_t n=std::min(capacity,text.size()); if(out && n) std::memcpy(out,text.data(),n); return n;
}
lfs_kv_err_t lfs_kv_set_blob(const char* key,const void* data,size_t size) {
    if(mode==6) return LFS_KV_ERR_WRITE;
    std::ofstream f(std::string(std::getenv("STATE_DIR"))+"/"+key,std::ios::binary);
    f.write(static_cast<const char*>(data),size);return f ? LFS_KV_OK : LFS_KV_ERR_WRITE;
}
int WifiSdkGetStationMac(unsigned char mac[6]) { const unsigned char a[]={0xc8,0xe7,0x13,0x82,0xfc,0xed};std::memcpy(mac,a,6); return 0; }
int mbedtls_sha1(const unsigned char*, size_t, unsigned char hash[20]) { for(unsigned i=0;i<20;i++)hash[i]=i;return 0; }
int NetworkSetServerTime(double ms) { if(ms<1704067200000.0 || ms>2085978495000.0) return -1; utc=ms/1000; return 0; }
int HttpSdkVerifiedPostJson(const http_sdk_json_post_t* req,unsigned timeout,char* body,size_t cap,http_sdk_response_t* out) {
    ++calls;
    assert(std::strcmp(req->url,"https://api.tenclass.net/xiaozhi/ota/")==0 && timeout==10000 && cap==8193);
    assert(req->ca_pem && req->ca_len>0);
    bool device=false, client=false;
    for(const char** h=req->headers;*h;++h) {
        device|=std::strcmp(*h,"Device-Id: c8:e7:13:82:fc:ed\r\n")==0;
        client|=std::strstr(*h,"Client-Id: 00010203-0405-5607-8809-0a0b0c0d0e0f")!=nullptr;
    }
    assert(device && client);
    const char* json=R"({"mqtt":{"endpoint":"host","password":"HIDDEN_PASSWORD"},"websocket":{"url":"wss://host/","token":"HIDDEN_TOKEN","version":3},"server_time":{"timestamp":1792722014000,"timezone_offset":480},"firmware":{"version":"0.0.2","url":"https://host/fw"}})";
    if(mode==0)json=R"({"activation":{"code":"123456"},"websocket":{"url":"wss://host/","token":"HIDDEN_TOKEN","version":3}})";
    if(mode==3)json="{";
    if(mode==5)json=R"({"server_time":{"timestamp":1}})";
    std::memcpy(body,json,std::strlen(json)+1);
    *out={mode==4?403u:200u,mode==7?0:1,std::strlen(json),mode==8?1:0};
    return mode==2 || mode==8 ? -1 : 10;
}
int HttpSdkVerifiedGetBody(const http_sdk_json_post_t*,unsigned,char*,size_t,http_sdk_response_t*) { assert(false);return -1; }
}
bool StartWifiInitialization(const char*,const char*) { return true; }
int main(int argc,char** argv) {
    assert(argc==2);mode=std::atoi(argv[1]);
    bool ok=RunOriginalOtaCheck();
    assert(ok==(mode==1));
    assert(calls==(mode==9?0:1));
    if(mode==1) {
        assert(utc==1792722014);  // UTC, no +8h corruption.
        assert(Settings("mqtt").GetString("endpoint")=="host");
        assert(Settings("websocket").GetInt("version")==3);
        assert(Settings("websocket").GetString("token")=="HIDDEN_TOKEN");
    }
    if(mode==2) assert(Settings("websocket").GetString("token")=="HIDDEN_TOKEN");
    assert(!Ota::Upgrade("https://host/fw",{}));
}
"""

class OriginalOtaTests(unittest.TestCase):
    def test_original_method_and_persistent_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            for d in ['mbedtls','freertos','interfaces']:(work/d).mkdir()
            (work/'FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef unsigned TickType_t;\n')
            (work/'event_groups.h').write_text('#pragma once\ntypedef void* EventGroupHandle_t; typedef unsigned EventBits_t;\n')
            (work/'task.h').write_text('#pragma once\n')
            (work/'mbedtls/version.h').write_text('#define MBEDTLS_VERSION_MAJOR 3\n')
            (work/'mbedtls/sha1.h').write_text('#include <stddef.h>\nint mbedtls_sha1(const unsigned char*,size_t,unsigned char[20]);\n')
            (work/'bflb_mtd.h').write_text('void bflb_mtd_init(void);\n')
            (work/'esp_log.h').write_text('#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf("[%s] " fmt "\\n",tag,##__VA_ARGS__)\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n')
            for header in (ROOT/'third_party/esp-ml307/include').glob('*.h'):
                text=header.read_text().replace('#include <expected>','#include "platform/expected.h"').replace('std::expected<','xiaozhi::Expected<').replace('std::unexpected(','xiaozhi::Unexpected(')
                (work/'interfaces'/header.name).write_text(text)
            (work/'check.cc').write_text(HARNESS)
            subprocess.run(['gcc','-std=c99','-c',str(CJSON/'cJSON.c'),'-o',str(work/'json.o')],check=True)
            sources=[ROOT/'main/audio/audio_codec.cc',ROOT/'main/ota.cc',ROOT/'main/ota_check_response.cc',*[PORT/x for x in ['ota_bootstrap.cc','board_bouffalo.cc','http_bouffalo.cc','settings_bouffalo.cc','device_identity.cc']]]
            includes=[work,work/'interfaces',PORT,PORT/'compat',ROOT/'main',ROOT/'main/boards/common',ROOT/'third_party/tl-expected/include',CJSON,CJSON.parents[1]/'fs/littlefs/lfs_kv']
            # Actual SDK KV header, with the storage device simulated by files.
            includes[-1]=ROOT.parents[2]/'components/fs/littlefs/lfs_kv'
            cmd=['g++','-std=c++17','-Wall','-Wextra','-Werror','-DXIAOZHI_PLATFORM_BOUFFALO=1',*['-I'+str(x) for x in includes],*[str(x) for x in sources],str(work/'check.cc'),str(work/'json.o'),'-o',str(work/'check')]
            subprocess.run(cmd,check=True)
            shared=work/'persistent';shared.mkdir()
            for mode in [0,1,2,3,4,5,6,7,8,9]:
                state=shared if mode in (0,1,2) else work/str(mode)
                state.mkdir(exist_ok=True)
                env=os.environ.copy();env['STATE_DIR']=str(state)
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'check'),str(mode)],env=env,check=True,text=True,capture_output=True,timeout=5)
                    self.assertNotIn('HIDDEN_',result.stdout)
                    self.assertEqual('ORIGINAL_OTA_OK:' in result.stdout,mode in (0,1))
                    if mode==2:self.assertIn('SETTINGS_LOADED: mqtt=1 websocket=1',result.stdout)

if __name__=='__main__':unittest.main()
