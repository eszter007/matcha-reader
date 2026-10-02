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


MARKER = b"SCREENSHOT_START:"
FOOTER = b"SCREENSHOT_END\n"


def take_commands():
    """Claim the command file atomically: producers appending afterwards start a fresh one."""
    claimed = CMD + ".sending"
    try:
        os.rename(CMD, claimed)
    except FileNotFoundError:
        return []
    with open(claimed) as f:
        lines = [line.strip() for line in f if line.strip()]
    os.remove(claimed)
    return lines


def split_log(pending):
    """Text that is safe to log now, and the rest to keep: a screenshot, or a marker cut in half."""
    start = pending.find(MARKER)
    if start >= 0:
        return pending[:start], pending[start:]
    # Hold back a tail that could be the start of a marker split across reads.
    for keep in range(min(len(MARKER) - 1, len(pending)), 0, -1):
        if MARKER.startswith(pending[-keep:]):
            return pending[:-keep], pending[-keep:]
    return pending, b""


def run():
    log = open(LOG, "ab", buffering=0)
    unsent = []  # survives reconnects; a command is dropped only once it was written
    while True:
        ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
        if not ports:
            time.sleep(0.5)
            continue
        pending = b""
        try:
            port = serial.Serial(ports[0], 115200, timeout=0.2)
            print("open", ports[0], flush=True)
            while True:
                unsent += take_commands()
                while unsent:
                    port.write(("CMD:" + unsent[0] + "\n").encode())
                    port.flush()
                    unsent.pop(0)
                    time.sleep(0.05)
                data = port.read(65536)
                if not data:
                    continue
                pending += data
                text, pending = split_log(pending)
                log.write(text)
                if not pending.startswith(MARKER):
                    continue
                nl = pending.find(b"\n")
                end = pending.find(FOOTER, nl) if nl >= 0 else -1
                if end < 0:
                    continue  # wait for the whole frame
                size = int(pending[len(MARKER):nl])
                save_shot(pending[nl + 1:nl + 1 + size])
                log.write(b"[screenshot saved]\n")
                pending = pending[end + len(FOOTER):]
        except Exception as error:  # unplugged, sleeping, rebooting: reconnect
            # Keep whatever log text arrived, but never carry half a frame into the next connection.
            if pending and not pending.startswith(MARKER):
                log.write(pending)
            print("drop", error, flush=True)
            time.sleep(0.5)


if __name__ == "__main__":
    run()
