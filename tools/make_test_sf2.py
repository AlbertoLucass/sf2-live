#!/usr/bin/env python3
"""Gera um SoundFont 2 mínimo para os testes locais do motor."""
from __future__ import annotations

import math
import struct
import sys
from pathlib import Path


def chunk(name: bytes, payload: bytes) -> bytes:
    assert len(name) == 4
    padding = b"\0" if len(payload) % 2 else b""
    return name + struct.pack("<I", len(payload)) + payload + padding


def list_chunk(kind: bytes, children: bytes) -> bytes:
    return chunk(b"LIST", kind + children)


def fixed_name(value: str, length: int = 20) -> bytes:
    raw = value.encode("ascii")[:length]
    return raw + b"\0" * (length - len(raw))


def phdr(name: str, preset: int, bank: int, bag_index: int) -> bytes:
    return fixed_name(name) + struct.pack("<HHHIII", preset, bank, bag_index, 0, 0, 0)


def inst(name: str, bag_index: int) -> bytes:
    return fixed_name(name) + struct.pack("<H", bag_index)


def bag(gen_index: int, mod_index: int = 0) -> bytes:
    return struct.pack("<HH", gen_index, mod_index)


def gen(operator: int, amount: int) -> bytes:
    return struct.pack("<HH", operator, amount & 0xFFFF)


def shdr(
    name: str,
    start: int,
    end: int,
    loop_start: int,
    loop_end: int,
    sample_rate: int,
    pitch: int,
    correction: int,
    sample_link: int,
    sample_type: int,
) -> bytes:
    return (
        fixed_name(name)
        + struct.pack("<IIIIIBbHH", start, end, loop_start, loop_end, sample_rate,
                      pitch, correction, sample_link, sample_type)
    )


def build(path: Path) -> None:
    sample_rate = 48_000
    sample_count = 4_800
    samples = [
        int(math.sin(2.0 * math.pi * 440.0 * index / sample_rate) * 16_000)
        for index in range(sample_count)
    ]
    # A especificação recomenda amostras de guarda após o fim da amostra.
    samples.extend([0] * 46)
    smpl = struct.pack(f"<{len(samples)}h", *samples)

    pdta = b"".join(
        [
            chunk(b"phdr", phdr("Test Preset", 0, 0, 0) + phdr("EOP", 0, 0, 1)),
            chunk(b"pbag", bag(0) + bag(1)),
            chunk(b"pmod", b"\0" * 10),
            chunk(b"pgen", gen(41, 0)),  # instrument = 0
            chunk(b"inst", inst("Test Instrument", 0) + inst("EOI", 1)),
            chunk(b"ibag", bag(0) + bag(2)),
            chunk(b"imod", b"\0" * 10),
            chunk(b"igen", gen(54, 1) + gen(53, 0)),  # loop contínuo + sample 0
            chunk(
                b"shdr",
                shdr("Test Sample", 0, sample_count, 128, sample_count - 128,
                     sample_rate, 69, 0, 0, 1)
                + shdr("EOS", sample_count, sample_count, sample_count, sample_count,
                       sample_rate, 0, 0, 0, 1),
            ),
        ]
    )

    info = chunk(b"ifil", struct.pack("<HH", 2, 1)) + chunk(b"INAM", b"SF2 Live Test\0")
    body = b"sfbk" + list_chunk(b"INFO", info) + list_chunk(b"sdta", chunk(b"smpl", smpl)) + list_chunk(b"pdta", pdta)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/sf2live-test.sf2")
    target.parent.mkdir(parents=True, exist_ok=True)
    build(target)
    print(target)
