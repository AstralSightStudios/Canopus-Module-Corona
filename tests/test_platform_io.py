"""Compile the actual shared I/O adapter against native-call stubs.

This checks errno/vararg contracts without claiming firmware execution. The
module suite separately covers the constructor and lifecycle diagnostic I/O.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeOpenContract(unittest.TestCase):
    def test_exact_target_open_contracts(self):
        source = (ROOT / 'src/platform.c').read_text()
        # Keep the real definitions verbatim; substitute only firmware addresses.
        adapter = source[source.index('static int *errno_location('):
                         source.index('int rh_platform_read(')]
        fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
static int native_result, task_errno, expected_flags;
static int native_open(const char *path, int flags, ...) {
    va_list args;
    assert(!strcmp(path, "/fixture"));
    assert(flags == expected_flags);
    va_start(args, flags);
    assert(va_arg(args, int) == 0600);
    va_end(args);
    return native_result;
}
static int *native_errno_location(void) { return &task_errno; }
#define RH_FW_OPEN ((uintptr_t)native_open)
#define RH_FW_OPEN_NEGATIVE_ERRNO TEST_NEGATIVE_ERRNO
#if TEST_NEGATIVE_ERRNO
#define RH_FW_ERRNO_LOCATION ((uintptr_t)native_errno_location)
#else
static int *canopus_fw_errno_location(void) { return native_errno_location(); }
#endif
'''
        checks = r'''
int main(void) {
    expected_flags = 1;
    native_result = 0; task_errno = 13;
    assert(rh_platform_open("/fixture", 1) == 0);
    assert(rh_platform_errno() == 13); /* Successful open need not clear errno. */
#if TEST_NEGATIVE_ERRNO
    native_result = -2; task_errno = 13;
    assert(rh_platform_open("/fixture", 1) == -1);
    assert(rh_platform_errno() == 2); /* nx_open did not update errno itself. */
    native_result = -13; task_errno = 2;
    assert(rh_platform_open("/fixture", 1) == -1);
    assert(rh_platform_errno() == 13);
    native_result = -1; task_errno = 2;
    assert(rh_platform_open("/fixture", 1) == -1);
    assert(rh_platform_errno() == 1);
#else
    native_result = -1; task_errno = 2;
    assert(rh_platform_open("/fixture", 1) == -1);
    assert(rh_platform_errno() == 2);
    task_errno = 13;
    assert(rh_platform_open("/fixture", 1) == -1);
    assert(rh_platform_errno() == 13);
#endif
    native_result = 7; expected_flags = 2;
    assert(rh_platform_open("/fixture", 2) == 7);
    expected_flags = 0x26;
    assert(rh_platform_open("/fixture", 0x26) == 7);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='rh-native-io-') as tmp:
            path = Path(tmp)
            (path / 'test.c').write_text(fixture + adapter + checks)
            for negative in (0, 1):
                with self.subTest(negative_errno=negative):
                    binary = path / f'test-{negative}'
                    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                                    '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=address,undefined',
                                    f'-DTEST_NEGATIVE_ERRNO={negative}',
                                    str(path / 'test.c'), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
