#!/usr/bin/env python3
"""Decode self-generated red H264/VP8 samples and check the resulting guest pixels."""
import base64
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
video = root / "app/src/main/cpp/skyline/soc/host1x/video"
# Encoded with FFmpeg/libx264 and libvpx from lavfi color=red:size=32x16.
# H264 contains only the IDR NAL: SPS/PPS must be reconstructed by NVDEC.
fixtures = {'h264.bin': 'AAABZYiEP/4kR8Ckh+pgjjzIFQ6P', 'vp8.bin': '8AIAnQEqIAAQAABHCIWFiIWEiAICAnWqA/gD+gIKAh3A/v1u8//jmTcwxP+Obf/xYTwOKMj/8VEA'}
with tempfile.TemporaryDirectory(prefix="strato-video-") as directory:
    tmp = Path(directory)
    for name, data in fixtures.items():
        (tmp / name).write_bytes(base64.b64decode(data))
    sources = [root / "tests/video_pipeline_test.cpp", video / "runtime.cpp", video / "common/stream.cpp"]
    sources += sorted((video / "video_core/host1x").glob("*.cpp"))
    sources += sorted((video / "video_core/host1x/codecs").glob("*.cpp"))
    flags = shlex.split(os.environ.get("VIDEO_FFMPEG_CFLAGS", ""))
    libs = shlex.split(os.environ.get("VIDEO_FFMPEG_LIBS", "-lavcodec -lavutil"))
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(video), *flags,
                    *map(str, sources), *libs, "-o", str(tmp / "test")], check=True)
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1")
    subprocess.run([str(tmp / "test"), str(tmp / "h264.bin"), str(tmp / "vp8.bin")], env=env, check=True)
