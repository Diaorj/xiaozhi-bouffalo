"""Ensure TLS diagnostics preserve the SDK return value and errno."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import PORT

class TlsReadDiagnosticsTests(unittest.TestCase):
    def test_original_error_and_errno_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);(work/'mbedtls').mkdir()
            (work/'mbedtls/ssl.h').write_text('#include <stddef.h>\ntypedef struct {int unused;} mbedtls_ssl_context;\n#define MBEDTLS_ERR_SSL_WANT_READ (-0x6900)\n#define MBEDTLS_ERR_SSL_WANT_WRITE (-0x6880)\n#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (-0x7880)\n')
            (work/'test.c').write_text(r"""
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "mbedtls/ssl.h"
static int next_result, diagnostics;
int __wrap_mbedtls_ssl_read(mbedtls_ssl_context*,unsigned char*,size_t);
int mbedtls_ssl_read(mbedtls_ssl_context* ssl,unsigned char* data,size_t size) {
    assert(ssl && data && size==16);errno=EDOM;return next_result;
}
void MemoryDiagnosticsStage(const char* stage) {
    assert(strcmp(stage,"tls_read_error")==0);++diagnostics;errno=EAGAIN;
}
int main(void) {
    mbedtls_ssl_context ssl;unsigned char data[16];
    const int cases[]={16,MBEDTLS_ERR_SSL_WANT_READ,0,MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY,
                       -0x7f00,MBEDTLS_ERR_SSL_WANT_WRITE};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        next_result=cases[i];assert(__wrap_mbedtls_ssl_read(&ssl,data,sizeof(data))==cases[i]);
        assert(errno==EDOM);
    }
    assert(diagnostics==2);
}
""")
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(PORT),str(PORT/'tls_read_diagnostics.c'),str(work/'test.c'),'-Wl,--wrap=mbedtls_ssl_read','-o',str(work/'test')],check=True)
            result=subprocess.run([str(work/'test')],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('TLS_READ_ERROR: ret=-32512 hex=0x7f00',result.stdout)
            self.assertEqual(result.stdout.count('TLS_READ_CLOSED:'),2)

if __name__=='__main__':unittest.main()
