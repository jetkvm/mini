#!/usr/bin/env python3
"""Execute real SDK session processing and close work against slot reuse."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('IDF_PATH', Path.home()/'.espressif/v6.0.2/esp-idf'))
PARSE = 'components/esp_http_server/src/httpd_parse.c'
SESS = 'components/esp_http_server/src/httpd_sess.c'
PATCH = ROOT/'patches/esp-idf-6.0.2-http-server-ws-close-lifetime.patch'

def function(text, signature):
    a=text.index(signature); b=text.index('{',a); end=b+1; depth=1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}');end+=1
    return text[a:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_HTTPD_WS_SUPPORT 1
#define ESP_LOGD(...) ((void)0)
#define HTTP_SERVER_EVENT_DISCONNECTED 1
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
struct httpd_data { int http_server_state; uint64_t lru_counter; };
struct sock_db { int fd; bool ws_close,lru_socket; uint64_t lru_counter; void *handle; };
typedef struct { struct sock_db *sd; } aux_t;
typedef struct { void *handle; } req_t;
static struct sock_db *queued,*active;
static bool deleted,cleanup_done;
static void httpd_sess_delete(struct httpd_data *hd,struct sock_db *sd) { (void)hd;(void)sd;deleted=true; }
int httpd_sess_trigger_close(void *hd,int fd) { (void)hd;assert(fd==9);queued=active;return 0; }
static int httpd_req_new(struct httpd_data *hd,struct sock_db *sd) { (void)hd;active=sd;return ESP_OK; }
'''
request_delete = r'''
static int httpd_req_delete(struct httpd_data *hd) {
 req_t r={.handle=hd};aux_t ra={.sd=active};cleanup(&r,&ra);cleanup_done=true;return ESP_OK;
}
'''
suffix = r'''
int main(void) {
 struct httpd_data hd={.lru_counter=7};
 struct sock_db slot={.fd=9,.ws_close=false,.lru_counter=7,.handle=&hd};
 assert(httpd_sess_process(&hd,&slot)==ESP_OK);
 assert(cleanup_done && !queued && !deleted);
 slot.ws_close=true;cleanup_done=false;
 int ret=httpd_sess_process(&hd,&slot);
 assert(cleanup_done);
 if(!queued)assert(ret==ESP_FAIL); /* no closed WS may remain resident */
 if(ret!=ESP_OK)httpd_sess_delete(&hd,&slot); /* actual HTTPD loop behavior */
 else httpd_sess_delete(&hd,&slot); /* old path: peer EOF wins before queued work */
 assert(deleted);
 slot=(struct sock_db){.fd=9,.lru_counter=9,.handle=&hd};deleted=false;
 if(queued)httpd_sess_close(queued);
 return deleted ? 3 : 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    for rel in (PARSE,SESS):
        # The pristine SDK file, whether or not configure already patched the tree.
        target=p/rel;target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(subprocess.check_output(['git','show',f'HEAD:{rel}'],cwd=SDK))
    legacy={rel:(p/rel).read_text() for rel in (PARSE,SESS)}
    subprocess.run(['git','apply',str(PATCH)],cwd=p,check=True)
    current={rel:(p/rel).read_text() for rel in (PARSE,SESS)}
    subprocess.run(['git','apply','--reverse',str(PATCH)],cwd=p,check=True)
    assert all((p/rel).read_text()==legacy[rel] for rel in (PARSE,SESS))
    for name,source,expected in [('current',current,0),('legacy',legacy,3)]:
        branch=function(source[PARSE],'    if (ra->sd->ws_close)')
        close=function(source[SESS],'static void httpd_sess_close(')
        process=function(source[SESS],'esp_err_t httpd_sess_process(')
        c=p/(name+'.c');binary=p/name
        c.write_text(prefix+close+'\nstatic void cleanup(req_t *r,aux_t *ra) { (void)r;\n'+branch+'\n}\n'+request_delete+process+suffix)
        subprocess.run(['cc','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
        assert subprocess.run([str(binary)]).returncode==expected
print('SDK WS Close: request cleanup precedes immediate retirement; legacy work closes reused HTTP slot: PASS')
