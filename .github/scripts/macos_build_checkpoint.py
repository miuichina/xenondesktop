"""Stop Ninja and its workers before the workflow saves the build cache."""

import os
import signal
import subprocess
import sys


def build(command, seconds):
    with subprocess.Popen(command, start_new_session=True) as process:
        try:
            return process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            print("Build checkpoint reached; interrupting Ninja.", flush=True)
            try:
                os.killpg(process.pid, signal.SIGINT)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                print("Ninja did not stop within 60 seconds; stopping its process group.", flush=True)
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            return 124


if __name__ == "__main__":
    sys.exit(build(sys.argv[1:], seconds=4 * 60 * 60))
