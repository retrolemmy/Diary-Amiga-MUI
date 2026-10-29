#!/usr/bin/env python3
"""Writes Diary.info, the Workbench icon for Diary.

The icon is a closed diary (the same drawing as the Mac version's icon)
in the four standard Workbench colours: grey, black, white and blue.
It's a classic planar icon, so it works on every Workbench.
Usage: make_icon.py Diary.info
"""
import struct
import sys

SIZE = 32

# Drawing key -> Workbench pen (0 grey/transparent, 1 black, 2 white, 3 blue)
PENS = {
    '.': 0,     # background
    'K': 1,     # outline
    'R': 3,     # cover
    'H': 3,     # cover highlight (no extra colour available)
    'D': 1,     # spine and strap
    'W': 2,     # pages and label
    'G': 0,     # page edge lines
    'Y': 2,     # clasp and spine bands
    'O': 1,     # label frame
    'B': 3,     # ribbon
}


def new_grid(n):
    return [['.'] * n for _ in range(n)]


def rect(g, top, left, bottom, right, c):
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            g[y][x] = c


def frame(g, top, left, bottom, right, c):
    for x in range(left, right + 1):
        g[top][x] = g[bottom][x] = c
    for y in range(top, bottom + 1):
        g[y][left] = g[y][right] = c


def diary():
    g = new_grid(SIZE)
    # Page block peeking out at the right and bottom
    rect(g, 3, 5, 29, 27, 'W')
    frame(g, 3, 5, 29, 27, 'K')
    for y in range(5, 28, 2):
        g[y][26] = 'G'
    for x in range(7, 26, 2):
        g[28][x] = 'G'
    # Cover
    rect(g, 1, 3, 27, 25, 'R')
    frame(g, 1, 3, 27, 25, 'K')
    for x in range(9, 25):
        g[2][x] = 'H'
    for y in range(2, 27):
        g[y][9] = 'H'
    # Spine
    rect(g, 2, 4, 26, 7, 'D')
    for x in range(4, 8):
        g[5][x] = g[23][x] = 'Y'
    for y in range(2, 27):
        g[y][8] = 'K'
    # Label with two text lines
    rect(g, 6, 12, 12, 22, 'W')
    frame(g, 6, 12, 12, 22, 'O')
    for x in range(14, 21):
        g[8][x] = 'K'
    for x in range(14, 19):
        g[10][x] = 'K'
    # Strap across the right edge with a clasp
    rect(g, 15, 19, 19, 27, 'D')
    frame(g, 15, 19, 19, 27, 'K')
    rect(g, 14, 20, 20, 23, 'Y')
    frame(g, 14, 20, 20, 23, 'K')
    g[17][21] = g[17][22] = 'O'
    # Ribbon bookmark with a notched end
    rect(g, 28, 11, 31, 13, 'B')
    g[31][12] = '.'
    for y in (30, 31):
        g[y][10] = g[y][14] = 'K'
    return g


def planes(g, depth):
    """Planar image data: each bit plane in turn, rows padded to 16 bits."""
    words = (SIZE + 15) // 16
    data = bytearray()
    for plane in range(depth):
        for row in g:
            bits = 0
            for x in range(words * 16):
                pen = PENS[row[x]] if x < SIZE else 0
                bits = (bits << 1) | ((pen >> plane) & 1)
            data += bits.to_bytes(words * 2, 'big')
    return bytes(data)


def info_file(g):
    depth = 2
    # struct DiskObject with its embedded struct Gadget
    gadget = struct.pack('>IhhhhHHHIIIIIHI',
                         0,                 # NextGadget
                         0, 0, SIZE, SIZE,      # Left, Top, Width, Height
                         0x0004,            # GFLG_GADGIMAGE (select = complement)
                         0x0003,            # GACT_RELVERIFY | GACT_IMMEDIATE
                         0x0001,            # GTYP_BOOLGADGET
                         1,                 # GadgetRender: an image follows
                         0,                 # SelectRender
                         0, 0, 0,           # GadgetText, MutualExclude, SpecialInfo
                         0,                 # GadgetID
                         1)                 # UserData: WB_DISKREVISION
    disk_object = (struct.pack('>HH', 0xE310, 1) + gadget
                   + struct.pack('>BBIIiiIIi',
                                 3, 0,              # WBTOOL, pad
                                 0, 0,              # DefaultTool, ToolTypes
                                 -0x80000000, -0x80000000,  # NO_ICON_POSITION
                                 0, 0,              # DrawerData, ToolWindow
                                 16384))            # StackSize
    assert len(disk_object) == 78
    image = struct.pack('>hhhhhIBBI', 0, 0, SIZE, SIZE, depth,
                        1,                  # ImageData follows
                        (1 << depth) - 1, 0,  # PlanePick, PlaneOnOff
                        0)                  # NextImage
    return disk_object + image + planes(g, depth)


def main():
    with open(sys.argv[1], 'wb') as f:
        f.write(info_file(diary()))


if __name__ == '__main__':
    main()
