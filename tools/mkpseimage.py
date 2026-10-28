#!/usr/bin/env python3
"""Pack RTL8238B PSE image files into the region format read by rtl8238.c.

    tools/mkpseimage.py IMAGE0 IMAGE1 OUTPUT

IMAGE0 is loaded when register 0xe8 doesn't read 2, IMAGE1 when it does.
Pass '-' for an image that isn't available. Build with:

    make MACHINE=POE_2G040210GSM PSE_IMAGE=OUTPUT

The images are the PoE controller vendor's firmware; keep them and the packed
file out of the repository.
"""
import struct
import sys

PSE_IMAGE_MAX = 0xC000  # rtl8238.h
HDR_LEN = 12


def load(path):
    if path == "-":
        return b""
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    images = [load(p) for p in sys.argv[1:3]]
    if not any(images):
        sys.exit("mkpseimage: no image given")
    out = b"PSE1" + struct.pack("<II", len(images[0]), len(images[1])) + images[0] + images[1]
    if len(out) > PSE_IMAGE_MAX:
        sys.exit("mkpseimage: %d bytes, more than the region's %d" % (len(out), PSE_IMAGE_MAX))
    with open(sys.argv[3], "wb") as f:
        f.write(out)
    print("%s: image 0 %d bytes, image 1 %d bytes, %d bytes in all"
          % (sys.argv[3], len(images[0]), len(images[1]), len(out)))


if __name__ == "__main__":
    main()
