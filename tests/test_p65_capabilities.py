"""P65 default feature contract and calendar configuration transactions."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "resource_hook_platform.h"
#include "resource_hook_target.h"
#include "resource_hook_calendar_runtime.h"
#include "resource_hook_calendar_native.h"

_Static_assert(RH_HAS_FONT_RELOAD == 1, "P65 font reload must be enabled by default");
_Static_assert(RH_HAS_NATIVE_CALENDAR == 1, "P65 calendar must be enabled by default");
_Static_assert(RH_HAS_QUICKAPP_ICONS == 1, "P65 QuickApp icons must be enabled by default");
static const char *config;
static unsigned position, renders, publishes, resets, retires;
static unsigned fail_retire_at;
static int render_result, reset_result;
void *rh_platform_alloc(uint32_t n) { return malloc(n); }
void rh_platform_free(void *p) { free(p); }
uint32_t rh_platform_lock(void) { return 0; }
void rh_platform_unlock(uint32_t irq) { (void)irq; }
int rh_platform_open(const char *p, int mode) {
    (void)p; (void)mode; position = 0; return config ? 1 : -RH_ENOENT;
}
int rh_platform_read(int fd, void *out, uint32_t cap) {
    uint32_t n = (uint32_t)strlen(config) - position;
    (void)fd; if (n > cap) n = cap;
    memcpy(out, config + position, n); position += n; return (int)n;
}
void rh_platform_close(int fd) { (void)fd; }
int rh_platform_redraw_ready(void) { return 1; }
int rh_platform_calendar_retire(const struct rh_calendar_config *c) {
    assert(c); retires++; return retires == fail_retire_at ? -1 : 0;
}
void rh_platform_calendar_publish(void) { publishes++; }
int rh_calendar_native_date(struct rh_calendar_date *d) {
    *d = (struct rh_calendar_date){2026, 10, 28, 3, 0}; return 0;
}
int rh_calendar_native_render(const struct rh_calendar_config *c,
                             const struct rh_calendar_date *d, const char *p) {
    assert(c && c->custom && d->year == 2026);
    assert(p && (!strcmp(p, RH_CALENDAR_BANK0) || !strcmp(p, RH_CALENDAR_BANK1)));
    renders++;
    return render_result;
}
int rh_p65_calendar_reset(void) { resets++; return reset_result; }
void rh_p65_calendar_publish(void) { publishes++; }
int main(void) {
    struct rh_calendar_config *candidate = 0;
    assert(!rh_calendar_stage_from("calendar.cfg", &candidate) && candidate);
    assert(candidate->items[1].y == 28 && candidate->items[2].y == 12);
    assert(!rh_calendar_commit(candidate));
    assert(!rh_calendar_pending());
    assert(!rh_calendar_refresh() && !renders && !resets && !publishes);
    config = "version=1\nday\ttext\ttop_mid\t0\t34\t48\t000000\t0\n";
    assert(!rh_calendar_stage_from("calendar.cfg", &candidate) && candidate && candidate->custom);
    assert(!rh_calendar_commit(candidate) && rh_calendar_pending());
    fail_retire_at = retires + 1;
    assert(rh_calendar_refresh() < 0 && !renders && !publishes);
    fail_retire_at = 0; render_result = -1;
    assert(rh_calendar_refresh() < 0 && renders == 1 && !publishes);
    render_result = 0; fail_retire_at = retires + 2;
    assert(rh_calendar_refresh() < 0 && renders == 2 && !publishes);
    fail_retire_at = 0;
    assert(!rh_calendar_refresh() && renders == 3 && publishes == 1);
    assert(!rh_calendar_pending());
    assert(!rh_calendar_poll() && renders == 3 && publishes >= 1);
    assert(rh_calendar_redirect("data/quickapp/app/perpetual_calendar/calendar_icon.bin"));
    assert(!rh_calendar_redirect("data/app/perpetual_calendar/calendar_icon.bin"));
    config = "version=1\n";
    assert(!rh_calendar_stage_from("calendar.cfg", &candidate) && candidate && !candidate->custom);
    assert(!rh_calendar_commit(candidate) && rh_calendar_pending());
    reset_result = -1;
    assert(rh_calendar_refresh() < 0 && renders == 3 && resets == 1);
    assert(rh_calendar_redirect("data/quickapp/app/perpetual_calendar/calendar_icon.bin"));
    reset_result = 0; fail_retire_at = retires + 1;
    assert(rh_calendar_refresh() < 0 && renders == 3 && resets == 2);
    assert(rh_calendar_redirect("data/quickapp/app/perpetual_calendar/calendar_icon.bin"));
    fail_retire_at = 0;
    assert(!rh_calendar_refresh() && renders == 3 && resets == 3);
    assert(!rh_calendar_pending());
    assert(!rh_calendar_redirect("data/quickapp/app/perpetual_calendar/calendar_icon.bin"));
    assert(!rh_calendar_poll() && resets == 3);
    return 0;
}
'''


class P65Capabilities(unittest.TestCase):
    def test_native_manager_uses_the_same_p65_delete_event(self):
        sdk = Path(os.environ.get("CANOPUS_ROOT", ROOT.parent / "Canopus-Private"))
        header = sdk / "targets/redmi-watch-6-3.100.043/generated/canopus_band11_addresses.h"
        if not header.is_file():
            self.skipTest("P65 companion Canopus pack is not installed")
        self.assertIn("#define FW_UI_FONT_EVENT_DELETE 35u", header.read_text())

    def test_exact_target_rejects_mixed_firmware_selection(self):
        for other in ("155", "177", "179", "1043"):
            with self.subTest(other=other):
                result = subprocess.run([os.environ.get("CLANG", "clang"), "-E",
                    "-DRH_TARGET_P65=1", "-DRH_TARGET_" + other + "=1", "-x", "c",
                    str(ROOT / "include/resource_hook_target.h")],
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Select exactly one Resource Hook target", result.stderr)

    def test_default_features_and_calendar_failure_retention(self):
        with tempfile.TemporaryDirectory(prefix="rh-p65-capabilities-") as temporary:
            harness = Path(temporary) / "capabilities.c"
            executable = Path(temporary) / "capabilities"
            harness.write_text(HARNESS)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-DRH_TARGET_P65=1", "-I" + str(ROOT / "include"), str(harness),
                str(ROOT / "src/calendar_config.c"), str(ROOT / "src/calendar_icon.c"),
                "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
