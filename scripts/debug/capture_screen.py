#!/usr/bin/env python3
import sys
import subprocess
import os
import signal
import time
from PIL import Image

def convert_raw_to_png(raw_path, out_png="screen.png", width=1024, height=768):
    if not os.path.exists(raw_path) or os.path.getsize(raw_path) < width * height * 4:
        return False
    with open(raw_path, "rb") as f:
        data = f.read(width * height * 4)
    img = Image.frombytes("RGB", (width, height), data, "raw", "XRGB")
    img.save(out_png)
    print(f"Saved {out_png} ({os.path.getsize(out_png)} bytes)")
    return True

def capture(pid=None, out_png="screen.png"):
    raw_path = "fb_boot.bin"
    if pid is not None:
        try:
            mtime_before = os.path.getmtime(raw_path) if os.path.exists(raw_path) else 0
            os.kill(pid, signal.SIGUSR1)
            for _ in range(20):
                time.sleep(0.05)
                if os.path.exists(raw_path) and os.path.getmtime(raw_path) > mtime_before:
                    break
        except Exception as e:
            print(f"Signal failed: {e}")

    if convert_raw_to_png(raw_path, out_png):
        return True

    # Fallback to /tmp/pearpc_fb.raw if present
    if convert_raw_to_png("/tmp/pearpc_fb.raw", out_png):
        return True

    print(f"Failed to find valid framebuffer dump at {raw_path}")
    return False

if __name__ == "__main__":
    pid = None
    out = "screen.png"
    if len(sys.argv) >= 2:
        if sys.argv[1].isdigit():
            pid = int(sys.argv[1])
            if len(sys.argv) >= 3:
                out = sys.argv[2]
        else:
            out = sys.argv[1]
    capture(pid, out)
