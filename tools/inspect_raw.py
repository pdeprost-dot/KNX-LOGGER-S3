#!/usr/bin/env python3
"""Validate and summarize KXLR/1 files using only Python's standard library."""
import argparse, binascii, json, struct
from pathlib import Path

HEADER = struct.Struct("<8sHHHH24s40sIIIIIIqQ16s16s356sI")
RECORD = struct.Struct("<IHHIIQQII")
TYPES = {int.from_bytes(x, "little"): x.decode() for x in (b"DATA", b"GAP ", b"TIME", b"STAT", b"END ")}

def cstr(value): return value.split(b"\0", 1)[0].decode("ascii", "replace")

def inspect(path: Path):
    with path.open("rb") as f:
        raw = f.read(HEADER.size)
        if len(raw) != HEADER.size: raise ValueError("header truncated")
        h = HEADER.unpack(raw)
        if h[0] != b"KNXRAW1\0": raise ValueError("bad magic")
        if binascii.crc32(raw[:-4]) & 0xffffffff != h[-1]: raise ValueError("bad header CRC")
        result = {"file": str(path), "recorder_id": cstr(h[5]), "session_id": cstr(h[6]),
                  "requested_hz": h[7], "start_epoch_ms": h[13], "time_source": cstr(h[15]),
                  "records": {}, "data_samples": 0, "crc_errors": 0}
        while True:
            rh = f.read(RECORD.size)
            if not rh: break
            if len(rh) != RECORD.size: raise ValueError("record header truncated")
            kind, version, hbytes, pbytes, seq, first, mono, crc, reserved = RECORD.unpack(rh)
            if hbytes != RECORD.size: raise ValueError(f"unsupported record header at {seq}")
            payload = f.read(pbytes)
            if len(payload) != pbytes: raise ValueError(f"payload truncated at {seq}")
            name = TYPES.get(kind, f"0x{kind:08x}")
            result["records"][name] = result["records"].get(name, 0) + 1
            if pbytes and binascii.crc32(payload) & 0xffffffff != crc: result["crc_errors"] += 1
            if name == "DATA": result["data_samples"] += pbytes // 2
        result["bytes"] = path.stat().st_size
        return result

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("file", type=Path)
    print(json.dumps(inspect(p.parse_args().file), indent=2))

