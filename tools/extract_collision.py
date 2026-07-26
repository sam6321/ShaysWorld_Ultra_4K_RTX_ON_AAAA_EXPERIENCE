#!/usr/bin/env python3
"""Extract CreateBoundingBoxes / CreatePlains from OG main.cpp → collision.json (WORLD_SCALE)."""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "Shays Code" / "Source" / "main.cpp"
OUT = Path(__file__).resolve().parents[1] / "assets" / "collision.json"
WORLD_SCALE = 0.01

TYPE_MAP = {"FLAT_PLAIN": 0, "XY_PLAIN": 1, "ZY_PLAIN": 2}


def main() -> None:
    text = MAIN.read_text(encoding="utf-8", errors="ignore")

    aabbs: list[dict] = []
    # Collect SetAABB* into a dict keyed by index
    boxes: dict[int, dict[str, float]] = {}
    for m in re.finditer(
        r"cam\.SetAABB(Max|Min)([XZ])\s*\(\s*(\d+)\s*,\s*([-\d.]+)\s*\)", text
    ):
        which, axis, idx_s, val_s = m.groups()
        idx = int(idx_s)
        boxes.setdefault(idx, {})
        key = ("max" if which == "Max" else "min") + axis.lower()
        boxes[idx][key] = float(val_s) * WORLD_SCALE
    for idx in sorted(boxes):
        b = boxes[idx]
        aabbs.append(
            {
                "minX": b["minx"],
                "maxX": b["maxx"],
                "minZ": b["minz"],
                "maxZ": b["maxz"],
            }
        )

    plains: list[dict] = []
    # Expand stair loop by simulating it (same as CreatePlains).
    plain_re = re.compile(
        r"cam\.SetPlains\s*\(\s*(FLAT_PLAIN|XY_PLAIN|ZY_PLAIN)\s*,"
        r"\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,"
        r"\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)"
    )

    # Prefer the definition body, not the forward declaration.
    start = text.find("void CreatePlains()\n{")
    if start < 0:
        start = text.find("void CreatePlains()\r\n{")
    if start < 0:
        start = text.find("// Set up co-ordinates of different plains")
    end = text.find("void DeleteImageFromMemory", start if start >= 0 else 0)
    body = text[start:end] if start >= 0 else text
    print(f"CreatePlains body chars={len(body)}")

    for m in plain_re.finditer(body):
        typ, xs, xe, ys, ye, zs, ze = m.groups()
        ys_f, ye_f = float(ys), float(ye)
        # Obvious OG typo on higher hill Yend
        if ye_f > 50000:
            ye_f = ys_f
        plains.append(
            {
                "type": TYPE_MAP[typ],
                "minX": float(xs) * WORLD_SCALE,
                "maxX": float(xe) * WORLD_SCALE,
                "minY": ys_f * WORLD_SCALE,
                "maxY": ye_f * WORLD_SCALE,
                "minZ": float(zs) * WORLD_SCALE,
                "maxZ": float(ze) * WORLD_SCALE,
            }
        )

    # Stair loop is in CreatePlains but uses variables — simulate if not already
    # captured (regex won't catch `step` identifiers). Detect by counting flats near stairs X.
    has_stairs = any(
        abs(p["minX"] - 31582.0 * WORLD_SCALE) < 0.01 and p["type"] == 0 for p in plains
    )
    if not has_stairs:
        step = 10450.0
        step_length = 9808.0
        for i in range(18):
            plains.append(
                {
                    "type": 0,
                    "minX": 31582.0 * WORLD_SCALE,
                    "maxX": 33835.0 * WORLD_SCALE,
                    "minY": step * WORLD_SCALE,
                    "maxY": step * WORLD_SCALE,
                    "minZ": step_length * WORLD_SCALE,
                    "maxZ": (step_length + 42.0) * WORLD_SCALE,
                }
            )
            step -= 48.0
            step_length -= 142.0
            if (i + 3) % 5 == 0:
                step_length -= 500.0
                step -= 48.0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    data = {
        "worldScale": WORLD_SCALE,
        "worldSizeX": 36000.0 * WORLD_SCALE,
        "worldSizeZ": 43200.0 * WORLD_SCALE,
        "aabbs": aabbs,
        "plains": plains,
    }
    OUT.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(f"Wrote {OUT} ({len(aabbs)} aabbs, {len(plains)} plains)")


if __name__ == "__main__":
    main()
