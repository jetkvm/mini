#!/usr/bin/env python3
"""Compile the real parser through the firmware CMake cap with stale settings."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void nested(int depth, int objects, int mixed, int valid) {
    char body[4096];
    size_t n = 0;
    for (int i = 0; i < depth; ++i) {
        const char *open = (objects || (mixed && i % 2)) ? "{\"x\":" : "[";
        size_t length = strlen(open);
        memcpy(body + n, open, length); n += length;
    }
    body[n++] = '0';
    for (int i = depth - 1; i >= 0; --i)
        body[n++] = (objects || (mixed && i % 2)) ? '}' : ']';
    body[n] = 0;
    cJSON *root = cJSON_ParseWithLengthOpts(body, n + 1, NULL, 1);
    assert((root != NULL) == valid);
    cJSON_Delete(root);
}
int main(void) {
    _Static_assert(CJSON_NESTING_LIMIT <= 16, "unsafe public limit");
    for (int kind = 0; kind < 3; ++kind) {
        nested(CJSON_NESTING_LIMIT, kind == 1, kind == 2, 1);
        nested(CJSON_NESTING_LIMIT + 1, kind == 1, kind == 2, 0);
        nested(400, kind == 1, kind == 2, 0);
    }
    cJSON *root = cJSON_Parse("{\"password\":\"[[[{{{\\\"\"}");
    assert(root); cJSON_Delete(root);
    assert(!cJSON_Parse("[[[["));
    puts("cJSON depth boundaries, mixed containers and malformed input: PASS");
}
'''

class CJsonDepthTest(unittest.TestCase):
    def test_depth_cap_applies_whatever_the_old_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix='jetkvm-cjson-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(PROGRAM)
            (path / 'CMakeLists.txt').write_text(f'''
        cmake_minimum_required(VERSION 3.16)
        project(cjson_depth C)
        include("{ROOT}/cmake/limit_cjson_depth.cmake")
        add_library(cjson "{ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c")
        target_include_directories(cjson PUBLIC "{ROOT}/managed_components/espressif__cjson/cJSON")
        target_compile_definitions(cjson PUBLIC CJSON_NESTING_LIMIT=${{OLD_LIMIT}} CJSON_CIRCULAR_LIMIT=10000)
        jetkvm_limit_cjson_depth(cjson)
        add_executable(test_depth test.c)
        target_link_libraries(test_depth cjson m)
        ''')
            for old_limit in (1000, 16, 8):
                build = path / str(old_limit)
                subprocess.run(['cmake', '-S', str(path), '-B', str(build),
                                f'-DOLD_LIMIT={old_limit}',
                                '-DCMAKE_C_FLAGS=-fsanitize=address,undefined'],
                               check=True, stdout=subprocess.DEVNULL)
                subprocess.run(['cmake', '--build', str(build)], check=True,
                               stdout=subprocess.DEVNULL)
                subprocess.run([str(build / 'test_depth')], check=True)


if __name__ == '__main__':
    unittest.main()
