#!/usr/bin/env python3
"""Execute the real request_fits() against the P4 reconnect failure snapshot."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'jetkvm_memory_policy.c').read_text()
header = (root / 'include/jetkvm_memory_policy.h').read_text()

def function(name):
    match = re.search(r'static bool ' + name + r'\([^;]*?\)\s*\{', source, re.S)
    assert match
    depth = 1
    pos = match.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]

logic = '\n'.join(function(name) for name in (
    'has_projected_floor', 'psram_fits', 'request_fits'))
macros = []
for name in sorted(set(re.findall(r'JETKVM_MEMORY_\w+', logic))):
    match = re.search(r'^#define ' + name + r'\s+((?:[^\n]*\\\n)*[^\n]*)', header, re.M)
    assert match, name
    macros.append(match.group(0))
types = [s for s in re.findall(r'typedef struct \{.*?\} \w+;', header, re.S)
         if s.endswith((' jetkvm_memory_policy_snapshot_t;', ' jetkvm_memory_policy_request_t;'))]
assert len(types) == 2
program = '\n'.join(['#include <assert.h>', '#include <stdbool.h>', '#include <stdint.h>',
                     '#include <stddef.h>', *macros, *types, logic]) + r'''
static bool admit(jetkvm_memory_policy_snapshot_t *m,
                  jetkvm_memory_policy_request_t *r) {
    return request_fits(m, r, 0, 0, 0, 0, 0, 0, 0);
}
int main(void) {
    /* Measured after 1080/720/4:3/16:10 switching: enough free PSRAM and
     * a 3.44 MiB block. The video pool needs no further block for the next
     * rebuild, so the peer is admitted by its own 1 MiB block. */
    jetkvm_memory_policy_snapshot_t m = {
        .internal_free=137083, .dma_free=106199, .psram_free=10952824,
        .internal_largest=65536, .dma_largest=65536, .psram_largest=3604480,
    };
    jetkvm_memory_policy_request_t r = {
        .psram_bytes=1835008, .psram_largest_block=1048576,
        .storage_ingress_bytes=262144,
    };
    assert(admit(&m, &r));
    m.psram_largest = r.psram_largest_block - 1;
    assert(!admit(&m, &r));
    m.psram_largest = r.psram_largest_block;
    assert(admit(&m, &r));
    m.psram_free = r.psram_bytes + JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES - 1;
    assert(!admit(&m, &r));
    m.psram_free++;
    assert(admit(&m, &r));
    m.internal_free = JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES - 1;
    assert(!admit(&m, &r));
    m.internal_free++;
    m.dma_free = JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES - 1;
    assert(!admit(&m, &r));
    m.dma_free++;
    r.storage_ingress_bytes = JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES + 1;
    assert(!admit(&m, &r));
    r.storage_ingress_bytes = 262144;
    assert(admit(&m, &r));
}
'''
with tempfile.TemporaryDirectory(prefix='p4-admission-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Peer admission by its own largest block, retained memory floors: PASS')
