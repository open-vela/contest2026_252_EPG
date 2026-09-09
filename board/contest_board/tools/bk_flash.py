#!/usr/bin/env python3
"""Flash a BK7258 without having to catch a 10-second reset window.

The CH340 on this board has its RTS#/CTS# pins unconnected (see the board
schematic), so nothing can pulse the chip's CEN pin: the only way to reset is
the RST button.  ``bk_loader`` waits about ten seconds for that and then gives
up, which turns flashing into a game of pressing RST at the right moment.

This script splits the job.  It hammers CMD_LinkCheck with no timeout until the
chip answers -- so a single RST press at any moment is enough -- and only then
hands the already-open session to ``bk_loader``, which does the actual erase
and write.  The chip stays in download mode once it has answered, so the
handover is safe.

Usage:
    bk_flash.py <image> <start_addr_hex> [port]
    bk_flash.py --read <outfile> <start_hex> <len_hex> [port]
"""
import glob
import os
import subprocess
import sys
import time

import serial

LINK = b"\x01\xe0\xfc\x01\x00"
RESP = b"\x04\x0e"
LINK_BAUDS = [115200, 1500000]
XFER_BAUD = 1500000
PORT_GLOB = "/dev/cu.usbserial-*"


def default_port():
    """Find the CH340, rather than assuming what it was called last time.

    The node name is assigned by the OS at enumeration and is not stable
    across a replug: this board has appeared as both usbserial-310 and
    usbserial-10 in one session.  A hard-coded name turns that into a silent
    twenty-minute hang, because wait_for_chip() below blocks on
    os.path.exists() for a node that will never come back.  Ask the system
    instead, and say plainly when there is nothing to talk to.
    """
    ports = sorted(glob.glob(PORT_GLOB))

    if not ports:
        sys.exit("no %s found -- is the board plugged in?" % PORT_GLOB)

    if len(ports) > 1:
        print("multiple serial ports %s, using %s" % (ports, ports[0]))

    return ports[0]


def wait_for_chip(port, baud=115200, announce_every=2000, reboot=False):
    """Block until the chip answers a link check.

    The l_bootloader download window is only tens of milliseconds wide, so this
    must not leave gaps: the port is opened once and the link check goes out
    back to back until something answers.  An earlier version cycled baud rates
    and reopened the port each round, and lost RST presses in the seams.

    With ``reboot=True`` the console's ``reboot`` command is sent on the SAME
    already-open port immediately before the hammering starts, so the window a
    watchdog reset opens lands in an already-running probe stream.  Sending the
    reboot from a separate process never worked -- the process seam alone eats
    more time than the whole window -- and weeks of "reboot flashing" successes
    were in fact the operator pressing RST.
    """
    probes = 0

    while not os.path.exists(port):
        time.sleep(0.5)

    ser = serial.Serial(port, baud, timeout=0)
    try:
        # Never drive DTR/RTS: on this board they are not wired to CEN, and
        # asserting them only confuses the CH340.
        ser.dtr = False
        ser.rts = False
        ser.reset_input_buffer()

        if reboot:
            ser.write(b"\r\nreboot\r\n")
            ser.flush()
            print("sent 'reboot'; hammering for the download window "
                  "(RST still works as fallback)", flush=True)
        else:
            print(f"waiting for the chip at {baud} baud -- "
                  "press RST once, any time", flush=True)

        while True:
            ser.write(LINK)
            probes += 1
            data = ser.read(64)
            if data and RESP in data:
                print(f"chip is in download mode ({probes} probes)", flush=True)
                return baud
            if probes % announce_every == 0:
                print(f"  still waiting ({probes} probes)...", flush=True)
    finally:
        try:
            ser.close()
        except Exception:
            pass


def run_bk_loader(args):
    """Hand over to bk_loader, which owns the real flash protocol."""
    cmd = ["bk_loader"] + args
    print("-> " + " ".join(cmd), flush=True)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = out.stdout + out.stderr
    for line in text.splitlines():
        if any(k in line for k in ("Get bus", "Download complete",
                                   "Read complete", "fail", "Elapse",
                                   "flash mid")):
            print("   " + line.strip(), flush=True)
    return "all pass" in text


def main():
    argv = list(sys.argv[1:])
    reboot = "--reboot" in argv
    if reboot:
        argv.remove("--reboot")

    if argv and argv[0] == "--read":
        out_file, start, length = argv[1], argv[2], argv[3]
        port = argv[4] if len(argv) > 4 else default_port()
        wait_for_chip(port, reboot=reboot)
        ok = run_bk_loader([
            "read", "-p", port, "-b", str(XFER_BAUD), "--reset_type", "3",
            "-g", "300", "-f", f"{out_file}@{start}-{length}",
        ])
    else:
        image = argv[0]
        start = argv[1]
        port = argv[2] if len(argv) > 2 else default_port()
        print(f"image : {image}  {os.path.getsize(image)} bytes")
        print(f"target: {start}")
        wait_for_chip(port, reboot=reboot)
        ok = run_bk_loader([
            "download", "-p", port, "-b", str(XFER_BAUD), "--reset_type", "3",
            "-g", "300", "-e", "1", "-r", "-i", os.path.abspath(image),
            "-s", start,
        ])

    print("OK" if ok else "FAILED", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
