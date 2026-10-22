#!/usr/bin/env python3
"""fileadder: compressed files must not leave their raw text behind.

fileadder reads each file into the image and then compresses it in place.
The raw read used to stay behind past the compressed data, over whatever lay
there: harmless padding in the standard layout, the default config in the
compact one, where it follows the web UI.
"""
import gzip, pathlib, re, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parent.parent
# Only fileadder: httpd_sim needs the version.h a firmware build generates,
# and CI runs these tests first
subprocess.run(["make", "-s", "-C", str(root / "tools"), "create_build_dir", "output/fileadder"],
               check=True, stdout=subprocess.DEVNULL)
tool = str(root / "tools/output/fileadder")
failures = 0


def check(cond, name):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name)
    failures += not cond


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    files = tmp / "files"
    files.mkdir()
    sources = {
        "a.js": b"var answer = 'compressible text';\n" * 2000,	# 68 KB raw
        "b.html": b"<html><body>small</body></html>",
    }
    for name, data in sources.items():
        (files / name).write_bytes(data)

    base = 0x1000
    size = 0x20000
    marker_at = 0x9000	# inside a.js's raw span, past its compressed end
    marker = b"ip 192.168.10.1\ngw 192.168.10.1\n"
    image = bytearray(size)
    image[marker_at:marker_at + len(marker)] = marker
    original = bytes(image)
    path = tmp / "image.bin"
    path.write_bytes(image)

    # As the Makefile does: the file table, then the files into the image
    quiet = dict(cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([tool, "-a", str(base), "-s", str(size), "-z", "-d", str(files), "-p", "fdata"], **quiet)
    subprocess.run([tool, "-a", str(base), "-s", str(size), "-z", "-d", str(files), str(path)], **quiet)
    out = path.read_bytes()
    table = dict(re.findall(r"#define (FDATA_\w+) (0x[0-9a-f]+|\d+)", (tmp / "fdata.h").read_text()))

    check(out[marker_at:marker_at + len(marker)] == marker,
          "data placed earlier past a compressed file survives")
    spans = []
    for name, data in sources.items():
        key = re.sub(r"\W", "_", name)
        start = int(table["FDATA_START_" + key], 0)
        length = int(table["FDATA_SIZE_" + key], 0)
        spans.append((start, start + length))
        check(gzip.decompress(out[start:start + length]) == data,
              f"{name} unpacks from its table address to the original")
    end = max(e for _, e in spans)
    check(int(table["FDATA_END"], 0) == end, "FDATA_END is where the last file ends")
    outside = all(out[i] == original[i] for i in range(size)
                  if not any(s <= i < e for s, e in spans))
    check(outside, "everything outside the files is as it was: no raw tail is left")

sys.exit(1 if failures else 0)
