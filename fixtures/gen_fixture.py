#!/usr/bin/env python3
"""Generate the self-authored playback fixture retrovert_selftest.sid.

A PSID v2 tune whose player is 6502 written out by hand below: init sets
the SID up for a sustained sawtooth, and the play routine, called once
per frame, walks a four-note arpeggio. Deterministic output — the
committed fixture and its sha256 in harness.toml must match what this
script emits.
"""

import struct
from pathlib import Path

OUT = Path(__file__).parent / "retrovert_selftest.sid"

LOAD = 0x1000
INIT = 0x1000
PLAY = 0x1020
NOTES = 0x1040  # four 16-bit SID frequencies
FRAME = 0x1048  # frame counter

# SID registers
V1_FREQ_LO, V1_FREQ_HI = 0xD400, 0xD401
V1_CONTROL, V1_AD, V1_SR = 0xD404, 0xD405, 0xD406
VOLUME = 0xD418

# PAL: register value = frequency * 16777216 / 985248.
SCALE = 16777216 / 985248
FIGURE = [261.63, 329.63, 392.00, 523.25]  # C4 E4 G4 C5


def lda_imm(v):
    return [0xA9, v]


def sta_abs(addr):
    return [0x8D, addr & 0xFF, addr >> 8]


def init_routine():
    return (
        lda_imm(0x0F) + sta_abs(VOLUME)  # master volume up
        + lda_imm(0x00) + sta_abs(V1_AD)  # fastest attack, no decay
        + lda_imm(0xF0) + sta_abs(V1_SR)  # full sustain, no release
        + lda_imm(0x00) + sta_abs(FRAME)  # reset the frame counter
        + lda_imm(0x21) + sta_abs(V1_CONTROL)  # sawtooth, gate on
        + [0x60]  # RTS
    )


def play_routine():
    # Bits 5-6 of the frame counter pick the note, so the figure advances
    # every 32 frames -- about 0.64 s on PAL.
    return (
        [0xEE, FRAME & 0xFF, FRAME >> 8]  # INC FRAME
        + [0xAD, FRAME & 0xFF, FRAME >> 8]  # LDA FRAME
        + [0x4A] * 5  # LSR A x5
        + [0x29, 0x03]  # AND #$03
        + [0x0A, 0xAA]  # ASL A; TAX
        + [0xBD, NOTES & 0xFF, NOTES >> 8] + sta_abs(V1_FREQ_LO)
        + [0xBD, (NOTES + 1) & 0xFF, (NOTES + 1) >> 8] + sta_abs(V1_FREQ_HI)
        + [0x60]  # RTS
    )


def note_table():
    out = []
    for hz in FIGURE:
        value = round(hz * SCALE)
        out += [value & 0xFF, value >> 8]
    return out


def c64_image():
    image = bytearray(FRAME + 1 - LOAD)
    for addr, code in ((INIT, init_routine()), (PLAY, play_routine()), (NOTES, note_table())):
        assert len(code) <= 0x20, f"routine at {addr:04x} overruns its slot"
        image[addr - LOAD:addr - LOAD + len(code)] = bytes(code)
    return bytes(image)


def field(text):
    return text.encode("ascii").ljust(32, b"\0")[:32]


def build():
    image = c64_image()
    header = struct.pack(
        ">4sHHHHHHHI",
        b"PSID",
        2,  # version
        0x7C,  # data offset
        LOAD,
        INIT,
        PLAY,
        1,  # number of songs
        1,  # starting song
        0,  # speed: every song vertical blank
    )
    header += field("Retrovert self-test")
    header += field("Retrovert")
    header += field("2026 Retrovert")
    header += struct.pack(">HBBBB", 0x14, 0, 0, 0, 0)  # PAL, 6581; no relocation
    assert len(header) == 0x7C, len(header)
    return header + image


def main():
    data = build()
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
