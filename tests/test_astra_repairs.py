"""Small C harnesses: process operations are mocks, never real signals/spawns.

Run with KILIX_TEST_SDK pointing at an existing pinned SDK for launcher coverage
and syntax checks. Temporary build products stay inside this checkout.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = os.environ.get("KILIX_TEST_SDK")


def includes():
    result = []
    for root, dirs, _ in os.walk(SDK, followlinks=True):
        dirs[:] = [d for d in dirs if d not in (".git", "build")]
        if Path(root).name == "include":
            result.append("-I" + root)
    return result


class RepairTests(unittest.TestCase):
    def compile_run(self, source, sdk=False):
        with tempfile.TemporaryDirectory(prefix=".astra-test-", dir=ROOT) as tmp:
            path = Path(tmp)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=c11", "-D_DEFAULT_SOURCE",
                            "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                            "-Werror", "-ffunction-sections", "-fdata-sections",
                            "-I" + str(ROOT), *(includes() if sdk else []),
                            str(path / "test.c"), "-Wl,--gc-sections",
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test"), tmp], check=True, timeout=10)

    def test_registry_validation(self):
        self.compile_run(r'''
#include <assert.h>
#define kill mock_kill
#include "src/laptop.c"
static int calls;
int mock_kill(pid_t pid, int sig) {
    assert(pid > 1); assert(sig == 0); ++calls;
    errno = ESRCH; return -1;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(setenv("KILIX_LAPTOP_PROFILES", argv[1], 1) == 0);
    long value = 0;
    assert(parse_run_pid("123\n", 4, &value) && value == 123);
    assert(parse_run_pid("123", 3, &value) && value == 123);
    char limit[64];
    snprintf(limit, sizeof limit, "%d\n", INT_MAX);
    assert(parse_run_pid(limit, strlen(limit), &value));
    const char *invalid[] = {"", "0", "1", "-2", "+2", " 2", "2\nextra",
                            "2\n\n", "2 ", "999999999999999999999999999999"};
    char path[PATH_MAX];
    assert(ensure_directory(argv[1]));
    char directory[PATH_MAX];
    assert(desk_laptop_run_directory(directory, sizeof directory));
    assert(ensure_directory(directory));
    assert(run_pid_path("test", path, sizeof path));
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        assert(!parse_run_pid(invalid[i], strlen(invalid[i]), &value));
        assert(write_private_file(path, invalid[i], strlen(invalid[i])));
        assert(!desk_laptop_run_terminate("test"));
        assert(calls == 0);
    }
    const char embedded[] = {'2', '\0', '3'};
    assert(!parse_run_pid(embedded, sizeof embedded, &value));
    assert(write_private_file(path, embedded, sizeof embedded));
    assert(desk_laptop_run_status("test", NULL) == -1 && calls == 0);
    assert(!desk_laptop_run_record("test", 1));
#if LONG_MAX > INT_MAX
    assert(!desk_laptop_run_record("test", (long)INT_MAX + 1));
    assert(!run_pid_alive((long)INT_MAX + 1) && calls == 0);
#endif
    assert(write_private_file(path, "123\n", 4));
    assert(desk_laptop_run_status("test", NULL) == 0 && calls == 1);
    return 0;
}
''')

    @unittest.skipUnless(SDK, "set KILIX_TEST_SDK to existing pinned SDK")
    def test_child_capacity(self):
        self.compile_run(r'''
#include <assert.h>
#define waitpid mock_waitpid
#define posix_spawnp mock_spawn
#define nanosleep mock_sleep
#define access mock_access
#define kill mock_kill
#include "src/launcher.c"
static int wait_mode, spawned, signals;
int mock_kill(pid_t pid, int sig) {
    assert(pid == 123 && (sig == SIGTERM || sig == SIGKILL));
    ++signals; return 0;
}
pid_t mock_waitpid(pid_t pid, int *status, int options) {
    (void)status;
    assert(pid > 1 && options == WNOHANG);
    if (wait_mode == 1) { errno = EINTR; return -1; }
    if (wait_mode == 2) { errno = ECHILD; return -1; }
    return 0;
}
int mock_spawn(pid_t *pid, const char *file,
               const posix_spawn_file_actions_t *actions,
               const posix_spawnattr_t *attr, char *const argv[],
               char *const envp[]) {
    (void)file; (void)actions; (void)attr; (void)argv; (void)envp;
    ++spawned; *pid = 123; return 0;
}
int mock_sleep(const struct timespec *req, struct timespec *rem) {
    (void)req; (void)rem; return 0;
}
int mock_access(const char *path, int mode) { (void)path; (void)mode; return 0; }
int main(void) {
    const char *command[] = {"mock"}; pid_t pid;
    setenv("KILIX_RC_PASSWORD_FILE", "/unused-mock", 1);
    setenv("PATH", "/mock", 1);
    for (int i = 0; i < LAUNCH_PENDING_MAX; ++i) pending_children[i] = 100 + i;
    assert(!spawn_detached(command, 1));
    assert(!spawn_tracked(command, 1, &pid));
    assert(!spawn_tab("test", NULL, command, 1));
    assert(!desk_launcher_password_is_default());
    assert(spawned == 0);
    wait_mode = 1;
    assert(available_child_slot() == -1); /* EINTR keeps ownership */
    wait_mode = 2;
    assert(available_child_slot() == 0); /* ECHILD releases ownership */
    wait_mode = 0;
    assert(spawn_tracked(command, 1, &pid));
    assert(pid == 123 && pending_children[0] == 123 && spawned == 1);
    wait_mode = 1;
    assert(spawn_detached(command, 1));
    assert(pending_children[1] == 123 && spawned == 2);
    wait_mode = 0; cancel_pending_child(123); assert(signals == 2);
    wait_mode = 2; cancel_pending_child(123); assert(signals == 2);
    return 0;
}
''', sdk=True)

    def test_terminal_lifecycle(self):
        source = (ROOT / "src/main.c").read_text()
        run = source[source.index("static int run_interactive(void)"):]
        initial = run[:run.index("last_movement_key = KITTYKB_KEY_NONE;")]
        self.assertLess(initial.index("set_emergency_terminal(&terminal_session)"),
                        initial.index("kittyts_start("))
        for cleanup in (initial[initial.index("kittyts_stop("):],
                        run[run.index("done:"):]):
            self.assertLess(cleanup.index("kittyts_stop("),
                            cleanup.index("set_emergency_terminal(NULL)"))
            self.assertLess(cleanup.index("set_emergency_terminal(NULL)"),
                            cleanup.index("kilix_game_signals_restore("))
        block = run[run.index("        if (kilix_game_signals_suspend_requested"):]
        block = block[:block.index("        if (!poll_input")]
        self.compile_run(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#define KITTYKB_KEY_NONE 0
static int events[16], count, fail_start, terminated;
static void event(int n) { events[count++] = n; }
static bool kilix_game_signals_suspend_requested(void *p) { (void)p; return true; }
static bool kilix_game_signals_requested(void *p) { (void)p; return terminated; }
static void kittyts_suspend(void *p) { (void)p; event(1); }
static void kilix_game_signals_set_emergency_terminal(void *p) { event(p ? 4 : 2); }
#define raise mock_raise
static int mock_raise(int sig) { assert(sig == SIGSTOP); event(3); return 0; }
static int kittyts_start(void *p, int in, int out, void *opts) {
    (void)p; (void)in; (void)out; (void)opts; event(5); return fail_start;
}
static int kittyts_width(void *p) { (void)p; return 900; }
static int kittyts_height(void *p) { (void)p; return 600; }
static bool ki_td_soft_renderer_resize(void *p, int w, int h) {
    (void)p; assert(w == 900 && h == 600); event(6); return true;
}
static int64_t kilix_game_monotonic_ns(void) { return 12345; }
static void kilix_game_clock_reset(void *p, int64_t now) {
    (void)p; assert(now == 12345); event(7);
}
static void check(void) {
    struct { int suspend_requested; } signals = {1};
    struct { int select_slot, move_x; } pending_input = {3, 1};
    int terminal_session, terminal_options, renderer, clock, width, height;
    int last_movement_key = 42; int64_t now;
    uint64_t last_presented_step = 12;
    bool failed = false, first_frame = false;
    for (int once = 0; once < 1; ++once) {
''' + block + r'''
    }
    assert(signals.suspend_requested == 0);
    if (terminated) { assert(count == 2 && !failed); return; }
    if (fail_start) { assert(count == 5 && failed); return; }
    assert(!failed && count == 7);
    for (int i = 0; i < 7; ++i) assert(events[i] == i + 1);
    assert(first_frame && last_presented_step == UINT64_MAX);
    assert(last_movement_key == 0 && pending_input.move_x == 0);
    assert(pending_input.select_slot == -1);
}
int main(void) {
    check(); count = 0; fail_start = 1; check();
    count = 0; terminated = 1; check(); return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
