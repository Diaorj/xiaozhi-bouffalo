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
    def test_shared_response_parser(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "parser.cc").write_text(PARSER)
            subprocess.run(['gcc','-std=c99','-c',str(CJSON/'cJSON.c'),'-o',str(work/'json.o')],check=True)
            common = ['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),
                      '-I'+str(ROOT/'main'),'-I'+str(CJSON),str(ROOT/'main/ota_check_response.cc'),str(work/'json.o')]
            subprocess.run([*common,str(work/'parser.cc'),'-o',str(work/'parser')],check=True)
            subprocess.run([str(work/'parser')],check=True,timeout=5)

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
