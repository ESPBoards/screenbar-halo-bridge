#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stand in for the C3: talk the bridge protocol to an S3 running the USB
build (s3/sdkconfig.usb), over its USB port.

  wire.py PORT hello
  wire.py PORT config 1A2B3C4D [--no-listen]
  wire.py PORT set on|off [--front/--no-front] [--back/--no-back] [--raw 0x4C-0xFE] [--temp 0-100]
  wire.py PORT pair 30
  wire.py PORT watch 60

Every command waits a little and prints what comes back.
"""
import argparse
import struct
import sys
import time

FRAMING = 1   # HALO_WIRE_FRAMING: the frame layout, not the message version
NAMES = {0x01: "HELLO_REQ", 0x02: "CONFIG", 0x03: "SET", 0x04: "PAIR",
         0x81: "HELLO", 0x82: "ACK", 0x83: "STATE", 0x84: "HEARD", 0x85: "HEARTBEAT"}
EVENTS = ["SYNC", "DONE", "FAILED", "REMOTE", "CANCELLED"]
STATUS = ["OK", "UNKNOWN_TYPE", "BAD_LENGTH", "BUSY"]


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs(data):
    out, block = bytearray(), bytearray()
    for b in data:
        if b:
            block.append(b)
        else:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
    out += bytes([len(block) + 1]) + block
    return bytes(out) + b"\0"


def uncobs(data):
    out, k = bytearray(), 0
    while k < len(data):
        code = data[k]
        if not code or k + code > len(data):
            return None
        out += data[k + 1:k + code]
        k += code
        if code < 0xFF and k < len(data):
            out.append(0)
    return bytes(out)


def frame(mtype, seq, payload=b""):
    body = bytes([FRAMING, mtype, seq, len(payload)]) + payload
    return cobs(body + struct.pack("<H", crc16(body)))


def parse(raw):
    f = uncobs(raw)
    if not f or len(f) < 6 or f[0] != FRAMING or f[3] != len(f) - 6:
        return None
    if struct.unpack("<H", f[-2:])[0] != crc16(f[:-2]):
        return None
    return f[1], f[2], f[4:-2]


def lights(bits):
    on = "on" if bits & 0x80 else "off"
    return f"{on} front={int(bool(bits & 0x40))} back={int(bool(bits & 0x01))}"


# Payload sizes per message type, as in lib/wire's halo_wire_payload_len().
LENGTHS = {0x81: 8, 0x82: 1, 0x83: 4, 0x84: 9, 0x85: 22}


def describe(mtype, seq, p):
    name = NAMES.get(mtype, f"0x{mtype:02X}")
    if mtype in LENGTHS and len(p) != LENGTHS[mtype]:
        # a peer on another protocol version: show the bytes instead of crashing
        return f"{name} seq {seq} unexpected length {len(p)}: {p.hex()}"
    if mtype == 0x81:
        version, fw, reset, addr = struct.unpack("<BHBI", p)
        return f"{name} version {version} firmware {fw} reset_reason {reset} address {addr:08X}"
    if mtype == 0x82:
        return f"{name} seq {seq} {STATUS[p[0]] if p[0] < len(STATUS) else p[0]}"
    if mtype == 0x83:
        ev, bits, bri, temp = p
        known = "".join(k for k, m in (("L", 8), ("B", 4), ("T", 2)) if bits & m) or "-"
        event = EVENTS[ev] if ev < len(EVENTS) else ev
        return f"{name} {event} {lights(bits)} brightness 0x{bri:02X} temp {temp} known {known}"
    if mtype == 0x84:
        addr, pid, a, b, off = struct.unpack("<IBBBh", p)
        return f"{name} {addr:08X} pid {pid} payload {a:02X}{b:02X} offset {off} kHz"
    if mtype == 0x85:
        up, sent, acked, failed, heard, dropped, restarts, bad = struct.unpack("<IIIHHHHH", p)
        return (f"{name} up {up}s sent {sent} acked {acked} failed {failed} heard {heard / 10:.1f}% "
                f"dropped {dropped} restarts {restarts} wire_bad {bad}")
    return f"{name} seq {seq} {p.hex()}"


class Link:
    def __init__(self, port):
        import serial  # here, so -h works without pyserial installed

        # dsrdtr: leave DTR/RTS alone, or opening the port resets the S3.
        self.s = serial.Serial(port, 115200, timeout=0.05, dsrdtr=True)
        self.buf = bytearray()
        self.seq = 0

    def send(self, mtype, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.s.write(frame(mtype, self.seq, payload))

    def read(self, seconds, quiet_beats=False):
        end = time.time() + seconds
        while time.time() < end:
            self.buf += self.s.read(256)
            while b"\0" in self.buf:
                raw, _, rest = self.buf.partition(b"\0")
                self.buf = bytearray(rest)
                msg = parse(bytes(raw)) if raw else None
                if msg and not (quiet_beats and msg[0] == 0x85):
                    print(f"{time.strftime('%H:%M:%S')} {describe(*msg)}", flush=True)


def ranged(lo, hi, shown):
    def check(v):
        try:
            n = int(v, 0)
        except ValueError:
            raise argparse.ArgumentTypeError(f"{v} is not a number") from None
        if not lo <= n <= hi:
            raise argparse.ArgumentTypeError(f"{v} is outside {shown}")
        return n
    return check


def duration(v):
    try:
        s = float(v)
    except ValueError:
        s = 0
    if not 0 < s < float("inf"):
        raise argparse.ArgumentTypeError(f"{v} is not a positive number of seconds")
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("cmd", choices=["hello", "config", "set", "pair", "watch"])
    ap.add_argument("arg", nargs="?", help="config: lamp address in hex; set: on or off; pair, watch: seconds")
    ap.add_argument("--no-listen", action="store_true", help="config: don't listen for the remote when idle")
    ap.add_argument("--front", action=argparse.BooleanOptionalAction, default=True)
    ap.add_argument("--back", action=argparse.BooleanOptionalAction, default=True)
    ap.add_argument("--raw", type=ranged(0x4C, 0xFE, "0x4C-0xFE"), default=0xA5, help="brightness, 0x4C-0xFE")
    ap.add_argument("--temp", type=ranged(0, 100, "0-100"), default=50, help="0 cold to 100 warm")
    ap.add_argument("--wait", type=duration, default=2.0, help="seconds to print replies for (not pair or watch)")
    ap.add_argument("--beats", action="store_true", help="print heartbeats too")
    a = ap.parse_args()

    if a.cmd == "config":
        try:
            address = int(a.arg or "", 16)
            if not 0 <= address <= 0xFFFFFFFF:
                raise ValueError
        except ValueError:
            ap.error("config needs the lamp address in hex, e.g. 1A2B3C4D")
    if a.cmd == "set" and a.arg not in ("on", "off"):
        ap.error("set needs on or off")
    if a.cmd == "pair":
        seconds = 30 if a.arg is None else int(a.arg) if a.arg.isdigit() else 0
        if not 1 <= seconds <= 255:
            ap.error("pair takes 1-255 seconds")
    if a.cmd == "watch":
        try:
            watch = 30.0 if a.arg is None else duration(a.arg)
        except argparse.ArgumentTypeError:
            ap.error("watch takes a number of seconds")
    if a.cmd == "hello" and a.arg is not None:
        ap.error("hello takes no argument")

    link = Link(a.port)
    if a.cmd == "hello":
        link.send(0x01)
    elif a.cmd == "config":
        # gain 4, retry_gain 6, max_attempts 5: the C3's defaults
        link.send(0x02, struct.pack("<IBBBB", address, 4, 6, 5, 0 if a.no_listen else 1))
    elif a.cmd == "set":
        bits = (0x80 if a.arg == "on" else 0) | (0x40 if a.front else 0) | (0x01 if a.back else 0)
        link.send(0x03, bytes([bits, a.raw, a.temp]))
    elif a.cmd == "pair":
        link.send(0x04, bytes([seconds]))
    # watch and pair run as long as asked, so pairing's reports aren't missed
    wait = watch if a.cmd == "watch" else seconds if a.cmd == "pair" else a.wait
    link.read(wait, quiet_beats=not a.beats and a.cmd != "watch")


if __name__ == "__main__":
    sys.exit(main())
