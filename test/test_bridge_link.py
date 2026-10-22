#!/usr/bin/env python3
"""Bridge link: the RP2040's Python framing against the 8051's C framing.

Runs the test_bridge_link binary (modchip/bridge/link.c compiled unmodified)
to encode the shared vectors and to decode what Python encodes, so the two
ends of the link are checked against each other, not each against itself.
"""
import pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "modchip/pico"))
import bridge_link as bl  # noqa: E402

binary = str(pathlib.Path(__file__).resolve().parent / "build/test_bridge_link")
failures = 0


def check(cond, name):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name)
    failures += not cond


def vector(k):
    """As vector() in test_bridge_link.c"""
    lens = (0, 1, 1, 253, 254, 255, 508, 509, 300, 1557)  # 1557: the largest payload, LINK_MSG_MAX - 4
    x = 12345 + k
    out = bytearray()
    for _ in range(lens[k]):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        if k in (1, 8):
            out.append(0)
        elif 3 <= k <= 7:
            out.append(1 + (x >> 16) % 255)
        else:
            out.append((x >> 16) & 0xFF)
    return bytes(out)


check(bl.crc16(b"123456789") == 0x4B37, "CRC-16/MODBUS check value")

c_frames = subprocess.run([binary, "--emit"], check=True, capture_output=True,
                          text=True).stdout.split()
for k, line in enumerate(c_frames):
    frame = bytes.fromhex(line)
    py = bl.encode(0x10 + k, k, vector(k))
    check(frame == py, "vector %d: C and Python encode identically" % k)
    msgs = bl.Deframer().feed(frame)
    check(msgs == [(0x10 + k, k, vector(k))], "vector %d: Python decodes the C frame" % k)

# Python encodes, C decodes; then a corrupted copy of each
lines, expect = [], []
for k in range(10):
    frame = bl.encode(0x20 + k, 100 + k, vector(k))
    lines.append(frame.hex())
    expect.append("MSG " + (bytes((0x20 + k, 100 + k)) + vector(k)).hex())
    bad = bytearray(frame)
    bad[1] ^= 0x01 if bad[1] != 0x01 else 0x03
    lines.append(bytes(bad).hex())
    expect.append("ERROR")
result = subprocess.run([binary, "--decode"], input="\n".join(lines) + "\n", check=True,
                        capture_output=True, text=True).stdout.splitlines()
for k in range(10):
    check(result[2 * k] == expect[2 * k], "vector %d: C decodes the Python frame" % k)
    check(result[2 * k + 1] == "ERROR", "vector %d: C refuses a corrupted Python frame" % k)

# The hello payload layout as put_hello() in bridge.c writes it
hello = (bytes((1,)) + bl.p32(0x83720000) + bytes.fromhex("060f00dd3b3e") + bl.p32(0x80000)
         + bl.p32(0x3F000) + bl.p32(0x3F000) + bytes((9, 3, 8, 2, 4))
         + b"SWGT024 V2.0 Unmanaged".ljust(30, b"\x00") + b"v0.1.0-abc1234")
h = bl.parse_hello(hello)
check(h["chip_id"] == 0x83720000 and h["mac"] == "06:0f:00:dd:3b:3e" and h["cpu_port"] == 9
      and h["max_port"] == 8 and h["machine"] == "SWGT024 V2.0 Unmanaged"
      and h["version"] == "v0.1.0-abc1234", "hello payload parses")

sys.exit(1 if failures else 0)
