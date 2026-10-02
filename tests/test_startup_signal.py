"""Interrupt graphics probing in a private PTY; never use a live terminal."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "kilix-land-desktop"


@unittest.skipUnless(BINARY.is_file(), "native binary has not been built")
class StartupSignalTests(unittest.TestCase):
    def test_sigterm_during_probe_restores_terminal(self):
        with tempfile.TemporaryDirectory(prefix="land-startup-test-") as directory:
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(("KILIX", "KITTY", "GPU_TERMINAL", "XDG_"))}
            environment.update(HOME=directory, XDG_CONFIG_HOME=directory,
                               KILIX_LAND_DESKTOP_CONFIG_DIR=directory,
                               KILIX_LAND_DESKTOP_AUDIO="0",
                               KILIX_LAND_DESKTOP_EXTERNAL_APPS="0")
            master, slave = pty.openpty()
            proc = None
            try:
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480))
                original = termios.tcgetattr(slave)
                flags = fcntl.fcntl(slave, fcntl.F_GETFL)
                proc = subprocess.Popen([str(BINARY)], cwd=ROOT, env=environment,
                                        stdin=slave, stdout=slave, stderr=slave,
                                        start_new_session=True)
                deadline = time.monotonic() + 10
                output = bytearray()
                while b"\x1b_Gi=31,a=q" not in output:
                    self.assertIsNone(proc.poll(), output.decode("utf-8", "replace"))
                    self.assertLess(time.monotonic(), deadline, "graphics probe did not start")
                    if select.select([master], [], [], 0.1)[0]:
                        output.extend(os.read(master, 65536))
                self.assertNotEqual(termios.tcgetattr(slave), original)
                os.kill(proc.pid, signal.SIGTERM)
                while proc.poll() is None:
                    self.assertLess(time.monotonic(), deadline, "startup termination did not finish")
                    if select.select([master], [], [], 0.1)[0]:
                        output.extend(os.read(master, 65536))
                proc.wait(timeout=1)
                self.assertEqual(termios.tcgetattr(slave), original)
                self.assertEqual(fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK,
                                 flags & os.O_NONBLOCK)
            finally:
                if proc is not None and proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                os.close(master)
                os.close(slave)


if __name__ == "__main__":
    unittest.main()
