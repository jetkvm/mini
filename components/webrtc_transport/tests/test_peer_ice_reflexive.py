#!/usr/bin/env python3
"""Exercise peer-reflexive admission after successful STUN authentication."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
agent_source = (repo / 'third_party/esp_peer/src/core/agent.c').read_text()


def compound(text, opening):
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening + 1:end - 1]


# Check the real source call rather than a particular instruction/relocation
# offset. Only the authenticated Binding Request branch can learn an endpoint.
request = re.search(r'\bvoid\s+agent_process_stun_request\([^;]*?\)\s*\{',
                    agent_source, re.S)
assert request, 'missing STUN request handler'
request_body = compound(agent_source, request.end() - 1)
binding = request_body.split('case STUN_METHOD_BINDING:', 1)[1]
authenticated = re.search(
    r'if\s*\(\s*stun_msg_is_valid\s*\(\s*stun_msg->buf\s*,\s*'
    r'stun_msg->size\s*,\s*agent->local_upwd\s*\)\s*==\s*0\s*\)\s*\{',
    binding)
assert authenticated, 'Binding Request must verify MESSAGE-INTEGRITY with the local ICE password'
authenticated_body = compound(binding, authenticated.end() - 1)
hook = re.compile(
    r'jetkvm_ice_authenticated_pair\s*\(\s*agent\s*,\s*remote\s*,\s*'
    r'stun_msg->buf\s*,\s*\(\s*size_t\s*\)\s*stun_msg->size\s*\)')
assert len(hook.findall(authenticated_body)) == 1, \
    'authenticated candidate admission must receive the wire buffer and explicit length'
assert len(re.findall(r'\bjetkvm_ice_authenticated_pair\s*\(', request_body)) == 1, \
    'candidate admission must remain inside the successful authentication branch'
assert 'jetkvm_ice_authenticated_pair_entry' not in agent_source

with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'esp_peer_types.h').write_text('''
#include <stdint.h>
typedef struct { uint8_t family; uint16_t port; union { uint8_t ipv4[4]; uint8_t ipv6[16]; }; } esp_peer_addr_t;
''')
    (p / 'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n')
    (p / 'test.c').write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include "peer_ice_reflexive.c"
static int additions, pairings;
static bool exists, full;
void *jetkvm_ice_find_pair(void *a, const esp_peer_addr_t *s) {
    (void)a; (void)s; return exists ? &exists : NULL;
}
void jetkvm_ice_add_remote(void *a, char *s, char *end) {
    (void)a; ++additions;
    assert((size_t)(end-s)==strlen(s));
    assert(strcmp(s,"42000 1 UDP 1862270975 192.168.0.207 42000 typ host")==0);
    if (!full) exists=true;
}
void jetkvm_ice_pair_candidates(void *a) {(void)a; ++pairings;}
static void fixture(uint8_t *wire) {
    memset(wire,0,52);
    const uint8_t prefix[]={0,1,0,32,0x21,0x12,0xa4,0x42,
        0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x24,0,4,0x6e,0xff,0xff,0xff,
        0,8,0,20};
    memcpy(wire,prefix,sizeof(prefix));
}
int main(void) {
    uint8_t wire[52]; fixture(wire);
    esp_peer_addr_t source={.family=AF_INET,.port=42000,.ipv4={192,168,0,207}};
    assert(jetkvm_ice_authenticated_pair(NULL,&source,wire,sizeof(wire)));
    assert(additions==1 && pairings==1);
    assert(jetkvm_ice_authenticated_pair(NULL,&source,wire,sizeof(wire)));
    assert(additions==1); /* existing pair reused, no allocation on every STUN */
    exists=false;full=true;
    assert(!jetkvm_ice_authenticated_pair(NULL,&source,wire,sizeof(wire)));
    assert(additions==2); /* capacity remains bounded by the peer allocator */
    full=false;
    for(int which=0;which<7;++which) {
        fixture(wire);
        size_t size=sizeof(wire);
        if(which==0) memset(wire+24,0,4); /* zero PRIORITY */
        if(which==1)wire[23]=8; /* malformed attribute length */
        if(which==2)wire[29]=9; /* missing MESSAGE-INTEGRITY */
        if(which==3)wire[20]=0x80; /* no PRIORITY before integrity */
        if(which==4)wire[3]=40; /* inconsistent STUN size */
        if(which==5)wire[1]=0x11; /* response instead of Binding Request */
        if(which==6)size=1401; /* oversized message is rejected before parsing */
        assert(!jetkvm_ice_authenticated_pair(NULL,&source,wire,size));
        assert(additions==2);
    }
    fixture(wire);source.family=AF_INET6;
    assert(!jetkvm_ice_authenticated_pair(NULL,&source,wire,sizeof(wire)));
    source.family=AF_INET;source.port=0;
    assert(!jetkvm_ice_authenticated_pair(NULL,&source,wire,sizeof(wire)));
    assert(additions==2);
    puts("Authenticated ICE candidate admission, duplicates, malformed input and capacity: PASS");
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(p),'-I',str(component),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
