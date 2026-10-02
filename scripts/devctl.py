#!/usr/bin/env python3
"""Serial logger + remote control for a CrossPoint device running a debug build (LOG_LEVEL >= 2).

    python3 scripts/devctl.py [outdir]          # default outdir: ./.devctl

Everything the device prints is appended to <outdir>/device.log; it reconnects across sleep and
reboots. To drive the device, append lines to <outdir>/cmd.txt; each is sent as `CMD:<line>`:

    PRESS:<BACK|CONFIRM|LEFT|RIGHT|UP|DOWN|POWER>   hardware button press + release
    OPEN:/path/to/book.epub                         open a book in the reader
    HOME                                            go to Home
    RMDIR:/.crosspoint/epub_<hash>                  drop one book cache
    SCREENSHOT                                      saved to <outdir>/shot.png (portrait)

Keep this process the only user of the serial port; stop it while flashing.
"""
import glob
import os
import sys
import time

import serial
from PIL import Image

OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".devctl")
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, "device.log")
CMD = os.path.join(OUT, "cmd.txt")
SHOT = os.path.join(OUT, "shot.png")
W, H = 800, 480  # panel-native framebuffer


def save_shot(buf):
    Image.frombytes("1", (W, H), bytes(buf)).rotate(-90, expand=True).save(SHOT)


def run():
    log = open(LOG, "ab", buffering=0)
    pending = b""
    while True:
        ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
        if not ports:
            time.sleep(0.5)
            continue
        try:
            port = serial.Serial(ports[0], 115200, timeout=0.2)
            print("open", ports[0], flush=True)
            while True:
                if os.path.exists(CMD):
                    with open(CMD) as f:
                        lines = [line.strip() for line in f if line.strip()]
                    os.remove(CMD)
                    for line in lines:
                        port.write(("CMD:" + line + "\n").encode())
                        time.sleep(0.05)
                data = port.read(65536)
                if not data:
                    continue
                pending += data
                start = pending.find(b"SCREENSHOT_START:")
                if start >= 0:
                    nl = pending.find(b"\n", start)
                    end = pending.find(b"SCREENSHOT_END\n", nl if nl >= 0 else start)
                    if nl < 0 or end < 0:
                        continue  # wait for the whole frame
                    size = int(pending[start + 17:nl])
                    save_shot(pending[nl + 1:nl + 1 + size])
                    log.write(pending[:start] + b"[screenshot saved]\n")
                    pending = pending[end + len(b"SCREENSHOT_END\n"):]
                    continue
                log.write(pending)
                pending = b""
        except Exception as error:  # unplugged, sleeping, rebooting: reconnect
            print("drop", error, flush=True)
            time.sleep(0.5)


if __name__ == "__main__":
    run()
