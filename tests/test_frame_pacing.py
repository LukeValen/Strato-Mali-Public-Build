#!/usr/bin/env python3
"""Run the production guest clock with deterministic time and ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="strato-frame-pacing-") as directory:
    executable = str(Path(directory) / "test")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-I", str(root), str(root / "tests/frame_pacing_test.cpp"), "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
