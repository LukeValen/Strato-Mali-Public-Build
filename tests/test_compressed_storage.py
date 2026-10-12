#!/usr/bin/env python3
"""Exercise the production NCA compression reader with the real LZ4 codec."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    executable = str(Path(directory) / "compressed-storage-test")
    command = [os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-pthread",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(root / "app/src/main/cpp/skyline/vfs"),
               str(root / "tests/compressed_storage_test.cpp"), "-o", executable]
    if os.environ.get("LZ4_INCLUDE"):
        command.append("-I" + os.environ["LZ4_INCLUDE"])
    command.append(os.environ.get("LZ4_LIBRARY", "-llz4"))
    subprocess.run(command, check=True)
    subprocess.run([executable], check=True)
