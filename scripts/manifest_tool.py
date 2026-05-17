#!/usr/bin/env python3
"""Parse JPEG 2000 files and emit a corpus manifest.

Parses just enough of the JP2 box structure and JPEG 2000 codestream (SIZ,
COD markers) to populate manifest fields without external dependencies.

Usage:
  manifest_tool.py --root corpus/ --out corpus/manifest.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Callable, Iterable

SOC = 0xFF4F   # Start of Codestream
SIZ = 0xFF51   # Image and tile size
COD = 0xFF52   # Coding style default
SOT = 0xFF90   # Start of tile-part

JP2_SIGBOX = b"\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a"


def _find_codestream(data: bytes) -> int:
    """Return offset of SOC marker in `data`. Raises if not found."""
    if data.startswith(JP2_SIGBOX):
        offset = 0
        n = len(data)
        while offset + 8 <= n:
            box_len = struct.unpack(">I", data[offset:offset + 4])[0]
            box_type = data[offset + 4:offset + 8]
            header = 8
            if box_len == 1:
                if offset + 16 > n:
                    break
                box_len = struct.unpack(">Q", data[offset + 8:offset + 16])[0]
                header = 16
            elif box_len == 0:
                box_len = n - offset
            if box_type == b"jp2c":
                start = offset + header
                if start + 2 <= n and data[start] == 0xFF and data[start + 1] == 0x4F:
                    return start
                raise ValueError("jp2c box does not start with SOC")
            offset += box_len
        raise ValueError("no jp2c box found")
    if len(data) >= 2 and data[0] == 0xFF and data[1] == 0x4F:
        return 0
    raise ValueError("not a JP2/J2K file")


_PROG_ORDER = {0: "LRCP", 1: "RLCP", 2: "RPCL", 3: "PCRL", 4: "CPRL"}


def _parse_codestream(data: bytes, cs_offset: int) -> dict:
    """Walk markers from SOC until SOT; extract SIZ + COD info."""
    out: dict = {}
    n = len(data)
    pos = cs_offset
    if data[pos] != 0xFF or data[pos + 1] != 0x4F:
        raise ValueError("expected SOC")
    pos += 2

    while pos + 4 <= n:
        marker = (data[pos] << 8) | data[pos + 1]
        pos += 2
        if marker == SOT:
            break
        seg_len = struct.unpack(">H", data[pos:pos + 2])[0]
        seg_start = pos + 2
        seg_end = pos + seg_len
        if seg_end > n:
            break

        if marker == SIZ:
            xsiz = struct.unpack(">I", data[seg_start + 2:seg_start + 6])[0]
            ysiz = struct.unpack(">I", data[seg_start + 6:seg_start + 10])[0]
            xosiz = struct.unpack(">I", data[seg_start + 10:seg_start + 14])[0]
            yosiz = struct.unpack(">I", data[seg_start + 14:seg_start + 18])[0]
            xtsiz = struct.unpack(">I", data[seg_start + 18:seg_start + 22])[0]
            ytsiz = struct.unpack(">I", data[seg_start + 22:seg_start + 26])[0]
            csiz = struct.unpack(">H", data[seg_start + 34:seg_start + 36])[0]
            ssiz = data[seg_start + 36]
            out["width"] = xsiz - xosiz
            out["height"] = ysiz - yosiz
            out["components"] = csiz
            # Bit depth = (Ssiz & 0x7F) + 1; high bit = signed flag.
            out["bit_depth"] = (ssiz & 0x7F) + 1
            out["tile_w"] = xtsiz
            out["tile_h"] = ytsiz

        elif marker == COD:
            sg_prog = data[seg_start + 1]
            num_layers = struct.unpack(">H", data[seg_start + 2:seg_start + 4])[0]
            mct = data[seg_start + 4]
            num_decomp = data[seg_start + 5]
            # transform: 0 = 9-7 irreversible (lossy), 1 = 5-3 reversible (lossless)
            transform = data[seg_start + 10]
            out["progression"] = _PROG_ORDER.get(sg_prog, f"unknown({sg_prog})")
            out["num_layers"] = num_layers
            out["mct"] = bool(mct)
            out["decomp_levels"] = num_decomp
            out["lossless"] = (transform == 1)

        pos = seg_end

    return out


def parse_file(path: Path) -> dict:
    """Parse a single JP2/J2K file and return its manifest entry (without sha256)."""
    raw = path.read_bytes()
    info: dict = {
        "container": "jp2" if raw.startswith(JP2_SIGBOX) else "j2k",
        "bytes": len(raw),
    }
    cs_off = _find_codestream(raw)
    info.update(_parse_codestream(raw, cs_off))
    return info


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_manifest(files: Iterable[Path], root: Path, out_json: Path,
                   bucket_of: Callable[[Path], str]) -> None:
    """Write a manifest.json describing `files`. Paths recorded relative to root."""
    entries = []
    for p in sorted(files):
        rel = p.resolve().relative_to(root.resolve()).as_posix()
        try:
            info = parse_file(p)
        except Exception as e:
            entries.append({"path": rel, "bucket": bucket_of(p), "error": str(e)})
            continue
        info["path"] = rel
        info["bucket"] = bucket_of(p)
        info["sha256"] = _sha256(p)
        entries.append(info)
    out_json.write_text(json.dumps({"schema_version": 1, "files": entries}, indent=2))


def _bucket_from_path(root: Path):
    def _b(p: Path) -> str:
        rel = p.resolve().relative_to(root.resolve()).parts
        if not rel:
            return "user"
        first = rel[0]
        if first in ("user", "synthetic", "public"):
            return first
        return "user"
    return _b


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True,
                    help="corpus root (paths in manifest are relative to this)")
    ap.add_argument("--out", type=Path, required=True,
                    help="manifest output path")
    args = ap.parse_args()

    root: Path = args.root
    if not root.exists():
        print(f"manifest_tool: {root} does not exist", file=sys.stderr)
        return 2
    files = [p for p in root.rglob("*")
             if p.is_file() and p.suffix.lower() in (".jp2", ".j2k", ".jpc")]
    write_manifest(files, root, args.out, _bucket_from_path(root))
    print(f"manifest_tool: wrote {args.out} ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
