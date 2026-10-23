"""Exercise shared OTA parsing, the port's request, and bounded HTTP collection."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest
import uuid
from test_network_probe import HTTP

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
SDK = ROOT.parents[2]
CJSON = SDK / "components/utils/cjson"

PARSER = r"""
#include <cassert>
#include <cstring>
#include "ota_check_response.h"
int main() {
    OtaCheckResponse response;
    const char* valid = R"({"activation":{"message":"bind device","code":"123456","challenge":"challenge","timeout_ms":1234},"mqtt":{},"websocket":{}})";
    assert(response.Parse(valid, std::strlen(valid)));
    assert(response.activation().code == "123456" && response.activation().has_code);
    assert(response.activation().has_message && response.activation().has_challenge);
    assert(response.activation().has_timeout && response.activation().timeout_ms == 1234);
    assert(response.HasMqttConfig() && response.HasWebsocketConfig());
    const char* missing = "{} \r\n";
    assert(response.Parse(missing, std::strlen(missing)));
    assert(!response.activation().has_code && response.activation().code.empty());
    assert(!response.HasMqttConfig() && !response.HasWebsocketConfig());
    const char* wrong = R"({"activation":{"code":3,"message":null,"timeout_ms":"5"},"mqtt":[],"websocket":null})";
    assert(response.Parse(wrong, std::strlen(wrong)));
    assert(!response.activation().has_code && !response.activation().has_timeout);
    assert(!response.HasMqttConfig() && !response.HasWebsocketConfig());
    for (const char* invalid : {"", "[]", "null", "3", "{", "{}garbage", "{}{}"}) {
        assert(!response.Parse(invalid, std::strlen(invalid)));
        assert(response.root() == nullptr && !response.activation().has_code);
    }
    assert(!response.Parse(nullptr, 5));
    const char embedded[] = {'{','}',0,'{','}'};
    assert(!response.Parse(embedded, sizeof(embedded)));
}
"""

OTA = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ota_check.h"
#include "ota_check_response.h"
#include "http_sdk_bridge.h"
#include "wifi_sdk_bridge.h"
static int mode, requests;
extern "C" {
int WifiSdkGetStationMac(uint8_t mac[6]) {
    const uint8_t known[6] = {0xc8,0xe7,0x13,0x82,0xfc,0xed};
    std::memcpy(mac, known, 6);
    if (mode == 8) std::memset(mac, 0, 6);
    if (mode == 9) mac[0] = 0xc9;
    return mode == 10 ? -1 : 0;
}
int mbedtls_sha1(const unsigned char* input, size_t length, unsigned char out[20]) {
    const unsigned char ns[] = {0x6b,0xa7,0xb8,0x11,0x9d,0xad,0x11,0xd1,0x80,0xb4,0,0xc0,0x4f,0xd4,0x30,0xc8};
    assert(std::memcmp(input, ns, 16) == 0);
    const char name[] = "xiaozhi-bouffalo/bl616cl/c8:e7:13:82:fc:ed";
    assert(length == 16 + std::strlen(name));
    assert(std::memcmp(input+16, name, std::strlen(name)) == 0);
    const unsigned char digest[] = {@DIGEST@};
    std::memcpy(out, digest, 20);
    return mode == 11 ? -1 : 0;
}
int HttpSdkVerifiedPostJson(const http_sdk_json_post_t* request, unsigned timeout,
                            char* body, size_t capacity, http_sdk_response_t* response) {
    ++requests;
    assert(std::strcmp(request->url,"https://api.tenclass.net/xiaozhi/ota/") == 0);
    assert(request->ca_pem && request->ca_len > 0 && timeout == 10000 && capacity == 8193);
    bool device=false, client=false, activation=false;
    for (const char** h=request->headers; *h; ++h) {
        device |= std::strcmp(*h,"Device-Id: c8:e7:13:82:fc:ed\r\n") == 0;
        client |= std::strcmp(*h,"Client-Id: @UUID@\r\n") == 0;
        activation |= std::strcmp(*h,"Activation-Version: 1\r\n") == 0;
    }
    assert(device && client && activation);
    cJSON* data=cJSON_ParseWithLength(request->payload, request->payload_len);
    assert(data);
    assert(std::strcmp(cJSON_GetObjectItem(data,"chip_model_name")->valuestring,"bl616cl") == 0);
    assert(std::strcmp(cJSON_GetObjectItem(data,"uuid")->valuestring,"@UUID@") == 0);
    const cJSON* board=cJSON_GetObjectItem(data,"board");
    assert(std::strcmp(cJSON_GetObjectItem(board,"type")->valuestring,"bouffalo-bl616cl-es8388") == 0);
    cJSON_Delete(data);
    const char* json=R"({"activation":{"code":"123456","challenge":"hidden-challenge"}})";
    if (mode == 1) json=R"({"mqtt":{"endpoint":"host","password":"HIDDEN_MQTT_PASSWORD"}})";
    if (mode == 2) json=R"({"websocket":{"url":"wss://host/","token":"HIDDEN_TOKEN"}})";
    if (mode == 3) json="{";
    if (mode == 12) json=R"({"activation":{"code":5}})";
    if (mode == 13) json="[]";
    std::memcpy(body,json,std::strlen(json)+1);
    *response={mode == 4 ? 403u : 200u,mode == 5 ? 0 : 1,std::strlen(json),mode == 7 ? 1 : 0};
    return mode == 6 || mode == 7 ? -1 : 10;
}
}
int main(int argc,char** argv) {
    assert(argc==2); mode=std::atoi(argv[1]);
    const bool ok=RunOtaCheck();
    assert(ok == (mode==0 || mode==1 || mode==2 || mode==12));
    assert(requests == ((mode>=8 && mode<=11) ? 0 : 1));
    if (mode==0) assert(GetOtaCheckResponse().activation().code == "123456");
}
"""

POST = r"""
#include <assert.h>
#include <string.h>
#include "http_sdk_bridge.h"
#include "https_client.h"
static int mode,calls;
int https_client_request(const struct https_client_request* req,uint32_t timeout,void* ctx) {
    ++calls;
    assert(req->method==HTTP_POST && timeout==10000 && req->ca_len==3);
    assert(strcmp(req->content_type_value,"application/json")==0);
    assert(req->payload_len==2 && strcmp(req->payload,"{}")==0);
    struct http_response response={200,0,(unsigned char*)"{\"ok\":",6};
    req->response(&response,HTTP_DATA_MORE,ctx);
    response.body_frag_start=(unsigned char*)"true}"; response.body_frag_len=5;
    if(mode==1) response.body_frag_len=10000;
    if(mode==2) response.body_frag_start=NULL;
    response.message_complete=mode==3 ? 0 : 1;
    req->response(&response,HTTP_DATA_FINAL,ctx);
    assert(req->cancel(ctx)==(mode==1 || mode==2));
    return 11;
}
int main(void) {
    http_sdk_json_post_t input={"https://test/","CA",3,NULL,"{}",2};
    http_sdk_response_t response;
    char output[12];
    for(mode=0;mode<4;++mode) {
        int result=HttpSdkVerifiedPostJson(&input,10000,output,sizeof(output),&response);
        if(mode==0) assert(result>=0 && response.complete && response.body_size==11 && strcmp(output,"{\"ok\":true}")==0);
        if(mode==1 || mode==2) assert(result<0 && response.overflow && !response.complete && response.body_size==6);
        if(mode==3) assert(result>=0 && !response.complete);
    }
    input.ca_pem=NULL;
    assert(HttpSdkVerifiedPostJson(&input,10000,output,sizeof(output),&response)<0);
    input.ca_pem="CA";
    assert(HttpSdkVerifiedPostJson(&input,0,output,sizeof(output),&response)<0);
    assert(HttpSdkVerifiedPostJson(&input,10000,output,1,&response)<0);
    assert(calls==4);
}
"""


class OtaCheckTests(unittest.TestCase):
    def test_shared_parser_and_ota_request(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "mbedtls").mkdir()
            (work / "mbedtls/version.h").write_text('#define MBEDTLS_VERSION_MAJOR 3\n')
            (work / "mbedtls/sha1.h").write_text(
                '#include <stddef.h>\nint mbedtls_sha1(const unsigned char*, size_t, unsigned char[20]);\n')
            (work / "esp_log.h").write_text(
                '#include <stdio.h>\n#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\\n", tag, ##__VA_ARGS__)\n'
                '#define ESP_LOGW ESP_LOGI\n')
            digest = hashlib.sha1(uuid.NAMESPACE_URL.bytes + b'xiaozhi-bouffalo/bl616cl/c8:e7:13:82:fc:ed').digest()
            expected = str(uuid.uuid5(uuid.NAMESPACE_URL,'xiaozhi-bouffalo/bl616cl/c8:e7:13:82:fc:ed'))
            ota = OTA.replace('@DIGEST@',','.join(str(x) for x in digest)).replace('@UUID@',expected)
            (work / "parser.cc").write_text(PARSER)
            (work / "ota.cc").write_text(ota)
            subprocess.run(['gcc','-std=c99','-c',str(CJSON/'cJSON.c'),'-o',str(work/'json.o')],check=True)
            common = ['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),
                      '-I'+str(ROOT/'main'),'-I'+str(CJSON),str(ROOT/'main/ota_check_response.cc'),str(work/'json.o')]
            subprocess.run([*common,str(work/'parser.cc'),'-o',str(work/'parser')],check=True)
            subprocess.run([str(work/'parser')],check=True,timeout=5)
            subprocess.run([*common,str(PORT/'ota_check.cc'),str(work/'ota.cc'),'-o',str(work/'ota')],check=True)
            for mode in range(14):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(work/'ota'),str(mode)],check=True,capture_output=True,text=True,timeout=5)
                    self.assertNotIn('HIDDEN_',result.stdout)
                    self.assertNotIn('hidden-challenge',result.stdout)
                    self.assertEqual('OTA_CHECK_OK:' in result.stdout,mode in (0,1,2,12))

    def test_http_post_fragment_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'https_client.h').write_text(HTTP)
            (work/'post.c').write_text(POST)
            executable=work/'post'
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),
                            str(PORT/'http_sdk_bridge.c'),str(work/'post.c'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=5)


if __name__ == '__main__':
    unittest.main()
