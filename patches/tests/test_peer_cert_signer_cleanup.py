#!/usr/bin/env python3
"""Execute the real temporary-signer function through success and partial failure."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for filename in ('dtls_srtp.c', 'dtls_srtp_v6.c'):
    source = (ROOT / 'third_party/esp_peer/src' / filename).read_text()
    start = source.index('int dtls_srtp_gen_cert(void)')
    end = source.index('\ndtls_srtp_t *dtls_srtp_init', start)
    function = source[start:end]
    v6 = filename.endswith('_v6.c')
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#define DTLS_SIGN_ONCE
#define MBEDTLS_SVC_KEY_ID_INIT 0
static int failure, live, keys, destructors;
typedef struct { int cert, pkey, ctr_drbg, psa_key_id; } dtls_srtp_t;
static void *media_lib_calloc(size_t n,size_t size) { return failure == 4 ? NULL : calloc(n,size); }
static void media_lib_free(void *p) { free(p); }
static void init(int *p) { *p=0; }
#define mbedtls_x509_crt_init init
#define mbedtls_pk_init init
#define mbedtls_ctr_drbg_init init
static void release(int *p) { if(*p) { live--; *p=0; } }
#define mbedtls_x509_crt_free release
#define mbedtls_pk_free release
#define mbedtls_ctr_drbg_free release
#define psa_destroy_key(key) do { assert((key)==1); keys--; } while(0)
static int dtls_srtp_selfsign_cert(dtls_srtp_t *p,bool cache) {
 assert(cache);
 p->ctr_drbg=1;live++;
 if(failure==1)return -1;
 p->pkey=1;live++;
 if(V6) { p->psa_key_id=1;keys++; }
 if(failure==2)return -2;
 p->cert=1;live++;
 return failure==3 ? -3 : 0;
}
/* A temporary signer stays in state NONE; this must never own its cleanup. */
void dtls_srtp_deinit(dtls_srtp_t *p) { (void)p;destructors++; }
'''.replace('V6', str(int(v6))) + function + r'''
int main(void) {
 for(failure=0;failure<=4;failure++) {
  int ret=dtls_srtp_gen_cert();
  assert(ret==(failure==4 ? -1 : -failure));
  assert(live==0 && keys==0 && destructors==0);
 }
 return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        src = Path(directory) / 'signer.c'; binary = Path(directory) / 'signer'
        src.write_text(harness)
        subprocess.run(['cc','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('Temporary DTLS signer: all owned resources released on success/partial failure, v5 and v6: PASS')
