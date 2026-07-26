#!/usr/bin/env python3
"""Extract Shay's World geometry from main.cpp into a bake-ready scene.

Parses CreateDisplayList / CreateAngledPolygon / window lists, maps list IDs to
textures via Display* glBindTexture + glCallList, applies WORLD_SCALE, and
writes scene.bin + materials.json + converted RGB textures.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

WORLD_SCALE = 0.01
AXIS = {"XY": 0, "XZ": 1, "YZ": 2, "YZ_FLIP": 3, "XY_FLIP": 4}


@dataclass
class Vertex:
    px: float
    py: float
    pz: float
    nx: float
    ny: float
    nz: float
    u: float
    v: float


@dataclass
class Mesh:
    vertices: list[Vertex] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)
    material_name: str = "default"


def push_quad(mesh: Mesh, pts_uv_n) -> None:
    base = len(mesh.vertices)
    for p in pts_uv_n:
        mesh.vertices.append(Vertex(*p))
    mesh.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def create_xtoz(mesh, x_img, z_img, x0, y0, z0, xt, zt):
    x1, z1 = x0, z0
    x2 = x0 + x_img * xt
    z2 = z0 + z_img * zt
    push_quad(
        mesh,
        [
            (x1, y0, z1, 0, 1, 0, 0, 0),
            (x1, y0, z2, 0, 1, 0, 0, zt),
            (x2, y0, z2, 0, 1, 0, xt, zt),
            (x2, y0, z1, 0, 1, 0, xt, 0),
        ],
    )


def create_xtoy(mesh, x_img, y_img, x0, y0, z0, xt, yt, flip=False):
    flip_x, temp_x = (xt, 0.0) if flip else (0.0, xt)
    x2 = x0 + x_img * xt
    y2 = y0 + y_img * yt
    push_quad(
        mesh,
        [
            (x0, y0, z0, 0, 0, 1, flip_x, 0),
            (x0, y2, z0, 0, 0, 1, flip_x, yt),
            (x2, y2, z0, 0, 0, 1, temp_x, yt),
            (x2, y0, z0, 0, 0, 1, temp_x, 0),
        ],
    )


def create_ytoz(mesh, y_img, z_img, x0, y0, z0, yt, zt, flip=False):
    flip_z, temp_z = (zt, 0.0) if flip else (0.0, zt)
    y2 = y0 + y_img * yt
    z2 = z0 + z_img * zt
    push_quad(
        mesh,
        [
            (x0, y0, z0, 1, 0, 0, 0, flip_z),
            (x0, y0, z2, 1, 0, 0, 0, temp_z),
            (x0, y2, z2, 1, 0, 0, yt, temp_z),
            (x0, y2, z0, 1, 0, 0, yt, flip_z),
        ],
    )


def create_display_list(mode: int, x_img, z_img, x0, y0, z0, xt, zt) -> Mesh:
    mesh = Mesh()
    if mode == 0:
        create_xtoy(mesh, x_img, z_img, x0, y0, z0, xt, zt, False)
    elif mode == 1:
        create_xtoz(mesh, x_img, z_img, x0, y0, z0, xt, zt)
    elif mode == 2:
        create_ytoz(mesh, x_img, z_img, x0, y0, z0, xt, zt, False)
    elif mode == 3:
        create_ytoz(mesh, x_img, z_img, x0, y0, z0, xt, zt, True)
    elif mode == 4:
        create_xtoy(mesh, x_img, z_img, x0, y0, z0, xt, zt, True)
    return mesh


def create_yz_window(x0, y0, y_size, z0, z_size, y_img, z_img) -> Mesh:
    mesh = Mesh()
    push_quad(
        mesh,
        [
            (x0, y0, z0, 1, 0, 0, 0, z_img),
            (x0, y0, z0 + z_size, 1, 0, 0, 0, 0),
            (x0, y0 + y_size, z0 + z_size, 1, 0, 0, y_img, 0),
            (x0, y0 + y_size, z0, 1, 0, 0, y_img, z_img),
        ],
    )
    return mesh


def create_xy_window(z0, x0, x_size, y0, y_size, x_img, y_img) -> Mesh:
    mesh = Mesh()
    push_quad(
        mesh,
        [
            (x0, y0, z0, 0, 0, 1, 0, 0),
            (x0 + x_size, y0, z0, 0, 0, 1, x_img, 0),
            (x0 + x_size, y0 + y_size, z0, 0, 0, 1, x_img, y_img),
            (x0, y0 + y_size, z0, 0, 0, 1, 0, y_img),
        ],
    )
    return mesh


def texture_scale(a, b, c, d, image_size):
    b = (b - a) / image_size
    c = (c - a) / image_size
    d = (d - a) / image_size
    a = 0.0
    return a, b, c, d


def create_angled(image_w, image_h, xs, ys, zs, smallest_x, smallest_z) -> Mesh:
    x1, x2, x3, x4 = xs
    y1, y2, y3, y4 = ys
    z1, z2, z3, z4 = zs
    xi = [x1, x2, x3, x4]
    zi = [z1, z2, z3, z4]

    if 1 <= smallest_x <= 4:
        order = [smallest_x - 1] + [i for i in range(4) if i != smallest_x - 1]
        vals = [xi[i] for i in order]
        scaled = texture_scale(vals[0], vals[1], vals[2], vals[3], image_w)
        # Map back in CreateTextureScale order: first arg is the "1" slot being zeroed
        # Shay mutates by reference in call order — replicate CreateTextureScale on the
        # four values passed in the order of the if-branch.
        if smallest_x == 1:
            x_img = list(texture_scale(x1, x2, x3, x4, image_w))
        elif smallest_x == 2:
            t = list(texture_scale(x2, x1, x3, x4, image_w))
            x_img = [t[1], t[0], t[2], t[3]]
        elif smallest_x == 3:
            t = list(texture_scale(x3, x1, x2, x4, image_w))
            x_img = [t[1], t[2], t[0], t[3]]
        else:
            t = list(texture_scale(x4, x1, x2, x3, image_w))
            x_img = [t[1], t[2], t[3], t[0]]
    else:
        yi = [y1, y2, y3, y4]
        if smallest_x == 5:
            x_img = list(texture_scale(yi[0], yi[1], yi[2], yi[3], image_w))
        elif smallest_x == 6:
            t = list(texture_scale(yi[1], yi[0], yi[2], yi[3], image_w))
            x_img = [t[1], t[0], t[2], t[3]]
        elif smallest_x == 7:
            t = list(texture_scale(yi[2], yi[0], yi[1], yi[3], image_w))
            x_img = [t[1], t[2], t[0], t[3]]
        else:
            t = list(texture_scale(yi[3], yi[0], yi[1], yi[2], image_w))
            x_img = [t[1], t[2], t[3], t[0]]

    if 1 <= smallest_z <= 4:
        if smallest_z == 1:
            z_img = list(texture_scale(z1, z2, z3, z4, image_h))
        elif smallest_z == 2:
            t = list(texture_scale(z2, z1, z3, z4, image_h))
            z_img = [t[1], t[0], t[2], t[3]]
        elif smallest_z == 3:
            t = list(texture_scale(z3, z1, z2, z4, image_h))
            z_img = [t[1], t[2], t[0], t[3]]
        else:
            t = list(texture_scale(z4, z1, z2, z3, image_h))
            z_img = [t[1], t[2], t[3], t[0]]
    else:
        yi = [y1, y2, y3, y4]
        if smallest_z == 5:
            z_img = list(texture_scale(yi[0], yi[1], yi[2], yi[3], image_h))
        elif smallest_z == 6:
            t = list(texture_scale(yi[1], yi[0], yi[2], yi[3], image_h))
            z_img = [t[1], t[0], t[2], t[3]]
        elif smallest_z == 7:
            t = list(texture_scale(yi[2], yi[0], yi[1], yi[3], image_h))
            z_img = [t[1], t[2], t[0], t[3]]
        else:
            t = list(texture_scale(yi[3], yi[0], yi[1], yi[2], image_h))
            z_img = [t[1], t[2], t[3], t[0]]

    pts = [(x1, y1, z1), (x2, y2, z2), (x3, y3, z3), (x4, y4, z4)]
    nx = ny = nz = 0.0
    # Shay often repeats a corner to fake a triangle; skip degenerate edges.
    for i in range(4):
        for j in range(i + 1, 4):
            for k in range(j + 1, 4):
                ax = pts[j][0] - pts[i][0]
                ay = pts[j][1] - pts[i][1]
                az = pts[j][2] - pts[i][2]
                bx = pts[k][0] - pts[i][0]
                by = pts[k][1] - pts[i][1]
                bz = pts[k][2] - pts[i][2]
                cx = ay * bz - az * by
                cy = az * bx - ax * bz
                cz = ax * by - ay * bx
                length = math.sqrt(cx * cx + cy * cy + cz * cz)
                if length > 1e-6:
                    nx, ny, nz = cx / length, cy / length, cz / length
                    break
            else:
                continue
            break
        else:
            continue
        break
    if nx == 0.0 and ny == 0.0 and nz == 0.0:
        nx, ny, nz = 0.0, 1.0, 0.0
    # Grass hills / ramps should face upward under Lambert (OG had no lit normals).
    if abs(ny) >= abs(nx) and abs(ny) >= abs(nz) and ny < 0.0:
        nx, ny, nz = -nx, -ny, -nz

    mesh = Mesh()
    push_quad(
        mesh,
        [
            (x1, y1, z1, nx, ny, nz, x_img[0], z_img[0]),
            (x2, y2, z2, nx, ny, nz, x_img[1], z_img[1]),
            (x3, y3, z3, nx, ny, nz, x_img[2], z_img[2]),
            (x4, y4, z4, nx, ny, nz, x_img[3], z_img[3]),
        ],
    )
    return mesh


def parse_defines(text: str) -> dict[str, int]:
    defs = {}
    for m in re.finditer(r"#define\s+([A-Z0-9_]+)\s+(\d+)", text):
        defs[m.group(1)] = int(m.group(2))
    return defs


def parse_textures(text: str) -> dict[int, dict]:
    """Map texture define ID -> {file, w, h}."""
    # image = tp.LoadTexture("data/foo.raw", W, H);
    # tp.CreateTexture(NAME, image, W, H);
    loads = list(
        re.finditer(
            r'image\s*=\s*tp\.LoadTexture\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\s*;'
            r"\s*tp\.CreateTexture\(\s*([A-Z0-9_]+)\s*,",
            text,
            re.MULTILINE,
        )
    )
    # Fallback: separate lines
    if not loads:
        chunks = re.findall(
            r'LoadTexture\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\);\s*'
            r"\s*tp\.CreateTexture\(\s*([A-Z0-9_]+)",
            text,
        )
        result = {}
        defines = parse_defines(text)
        for path, w, h, name in chunks:
            tid = defines.get(name)
            if tid is not None:
                result[tid] = {"name": name, "path": path, "w": int(w), "h": int(h)}
        return result

    defines = parse_defines(text)
    result = {}
    for m in loads:
        path, w, h, name = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        tid = defines.get(name)
        if tid is not None:
            result[tid] = {"name": name, "path": path, "w": w, "h": h}
    return result


def eval_expr(expr: str, env: dict[str, float] | None = None) -> float:
    """Evaluate simple numeric expressions used in Shay source (a + b, a * b, parens, vars)."""
    expr = expr.strip()
    env = dict(env or {})

    def _eval_numeric(s: str) -> float:
        if not re.fullmatch(r"[\d\s.+\-*/()eE]+", s):
            raise ValueError(s)
        return float(eval(s, {"__builtins__": {}}, {}))  # noqa: S307 — constrained charset

    try:
        return _eval_numeric(expr)
    except Exception:
        pass

    def repl(m: re.Match[str]) -> str:
        name = m.group(0)
        if name not in env:
            raise KeyError(name)
        return f"({env[name]})"

    try:
        substituted = re.sub(r"[A-Za-z_]\w*", repl, expr)
        return _eval_numeric(substituted)
    except Exception:
        m = re.search(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", expr)
        if not m:
            raise ValueError(expr) from None
        return float(m.group(0))


def flatten_numbers(blob: str, env: dict[str, float] | None = None) -> list[float]:
    """Split a comma-separated argument list and eval each argument expression."""
    blob = blob.replace("\n", " ")
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in blob:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur).strip())
    out: list[float] = []
    for p in parts:
        if not p:
            continue
        try:
            out.append(eval_expr(p, env))
        except Exception:
            out.extend(float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", p))
    return out


def mesh_from_corners(corners: list[tuple[float, float, float, float, float]]) -> Mesh:
    """Build a textured quad/tri mesh from (x,y,z,u,v) corners.

    Shay pavement joins sometimes repeat a vertex to fake a triangle inside GL_QUADS.
    Degenerate corners are collapsed. Horizontal ground faces are forced to +Y normals
    (hand lists often wind clockwise from above, which lit as black under Lambert).
    """
    if len(corners) < 3:
        raise ValueError("need >= 3 corners")

    # Drop consecutive duplicates (and wrap-around duplicate of first).
    cleaned: list[tuple[float, float, float, float, float]] = []
    for c in corners:
        if cleaned:
            p = cleaned[-1]
            if abs(p[0] - c[0]) < 1e-6 and abs(p[1] - c[1]) < 1e-6 and abs(p[2] - c[2]) < 1e-6:
                continue
        cleaned.append(c)
    if len(cleaned) >= 2:
        a, b = cleaned[0], cleaned[-1]
        if abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6 and abs(a[2] - b[2]) < 1e-6:
            cleaned.pop()
    corners = cleaned
    if len(corners) < 3:
        raise ValueError("need >= 3 unique corners")

    # Prefer a non-degenerate triangle for the normal (skip zero-area edges).
    nx = ny = nz = 0.0
    for i in range(1, len(corners) - 1):
        x1, y1, z1, _, _ = corners[0]
        x2, y2, z2, _, _ = corners[i]
        x3, y3, z3, _, _ = corners[i + 1]
        ax, ay, az = x2 - x1, y2 - y1, z2 - z1
        bx, by, bz = x3 - x1, y3 - y1, z3 - z1
        cx = ay * bz - az * by
        cy = az * bx - ax * bz
        cz = ax * by - ay * bx
        length = math.sqrt(cx * cx + cy * cy + cz * cz)
        if length > 1e-8:
            nx, ny, nz = cx / length, cy / length, cz / length
            break

    # Ground planes: always face upward so Lambert matches the main pavement.
    ys = [c[1] for c in corners]
    if max(ys) - min(ys) < 1e-4:
        nx, ny, nz = 0.0, 1.0, 0.0
        # Tiny lift so join triangles win cleanly over coplanar base pavement.
        corners = [(c[0], c[1] + 0.5, c[2], c[3], c[4]) for c in corners]

    mesh = Mesh()
    if len(corners) == 4:
        push_quad(
            mesh,
            [
                (c[0], c[1], c[2], nx, ny, nz, c[3], c[4])
                for c in corners
            ],
        )
    else:
        base = len(mesh.vertices)
        for c in corners:
            mesh.vertices.append(Vertex(c[0], c[1], c[2], nx, ny, nz, c[3], c[4]))
        for i in range(1, len(corners) - 1):
            mesh.indices.extend([base, base + i, base + i + 1])
    return mesh


def parse_hand_quad_lists(text: str) -> dict[int, Mesh]:
    """Extract hand-authored glNewList(ID) + glBegin(GL_QUADS|POLYGON) blocks."""
    meshes: dict[int, Mesh] = {}
    for m in re.finditer(
        r"glNewList\s*\(\s*(\d+)\s*,\s*GL_COMPILE\s*\)\s*;"
        r"(?:[^\n]*\n)*?"  # allow trailing comments / blank lines
        r"\s*glBegin\s*\(\s*(?:GL_QUADS|GL_POLYGON)\s*\)\s*;"
        r"(.*?)"
        r"glEnd\s*\(\s*\)\s*;"
        r"\s*glEndList\s*\(\s*\)\s*;",
        text,
        re.S,
    ):
        list_id = int(m.group(1))
        body = m.group(2)
        # Pair texcoord + vertex with nested-paren-safe argument scrape
        corners: list[tuple[float, float, float, float, float]] = []
        pos = 0
        while True:
            tm = re.search(r"glTexCoord2f\s*\(", body[pos:])
            if not tm:
                break
            t_start = pos + tm.end()
            t_args, t_end = _read_call_args(body, t_start)
            vm = re.search(r"glVertex3f\s*\(", body[t_end:])
            if not vm:
                break
            v_start = t_end + vm.end()
            v_args, v_end = _read_call_args(body, v_start)
            pos = v_end
            if len(t_args) < 2 or len(v_args) < 3:
                continue
            try:
                corners.append(
                    (
                        eval_expr(v_args[0]),
                        eval_expr(v_args[1]),
                        eval_expr(v_args[2]),
                        eval_expr(t_args[0]),
                        eval_expr(t_args[1]),
                    )
                )
            except Exception as exc:
                print(f"warn: hand list {list_id} corner: {exc}")
                corners = []
                break
        if len(corners) >= 3:
            try:
                meshes[list_id] = mesh_from_corners(corners)
            except Exception as exc:
                print(f"warn: hand list {list_id}: {exc}")
    return meshes


def _read_call_args(text: str, start: int) -> tuple[list[str], int]:
    """Parse comma-separated args starting after '(', return (args, index_after_closing_paren)."""
    depth = 1
    i = start
    args: list[str] = []
    cur: list[str] = []
    while i < len(text) and depth > 0:
        ch = text[i]
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return [a for a in args if a != "" or len(args) == 1], i + 1
            cur.append(ch)
        elif ch == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    args.append("".join(cur).strip())
    return [a for a in args if a], i


def expand_angled_roof_beams(text: str) -> dict[int, Mesh]:
    """Inline DrawAngledRoofBeam / DrawAngledRoofBeam2 call sites into meshes."""
    meshes: dict[int, Mesh] = {}

    def beam1(list_no: int, x: float, y: float, z: float, beam_size: float) -> None:
        meshes[list_no] = mesh_from_corners(
            [
                (x, y, z + 32.0, 0.0, 0.0),
                (x, y, z, 0.0, 1.0),
                (33848.0, 12012.72, z, beam_size, 1.0),
                (33848.0, 12012.72, z + 32.0, beam_size, 0.0),
            ]
        )
        meshes[list_no + 5] = mesh_from_corners(
            [
                (x, y, z, 0.0, 0.0),
                (x, y + 82.0, z, 0.0, 1.0),
                (33848.0, 12012.72 + 82.0, z, beam_size, 1.0),
                (33848.0, 12012.72, z, beam_size, 0.0),
            ]
        )

    def beam2(list_no: int, x: float, y: float, z: float, beam_size: float) -> None:
        meshes[list_no] = mesh_from_corners(
            [
                (x, y, z, 0.0, 0.0),
                (x + 32.0, y, z, 1.0, 0.0),
                (x + 32.0, 11998.0, 43056.0, 1.0, beam_size),
                (x, 11998.0, 43056.0, 0.0, beam_size),
            ]
        )
        meshes[list_no + 5] = mesh_from_corners(
            [
                (x, y, z, 0.0, 0.0),
                (x, y + 82.0, z, 1.0, 0.0),
                (x, 11998.0 + 82.0, 43056.0, 1.0, beam_size),
                (x, 11998.0, 43056.0, 0.0, beam_size),
            ]
        )

    for m in re.finditer(
        r"DrawAngledRoofBeam\s*\(\s*(\d+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^)]+)\)\s*;",
        text,
    ):
        beam1(
            int(m.group(1)),
            eval_expr(m.group(2)),
            eval_expr(m.group(3)),
            eval_expr(m.group(4)),
            eval_expr(m.group(5)),
        )
    for m in re.finditer(
        r"DrawAngledRoofBeam2\s*\(\s*(\d+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^)]+)\)\s*;",
        text,
    ):
        beam2(
            int(m.group(1)),
            eval_expr(m.group(2)),
            eval_expr(m.group(3)),
            eval_expr(m.group(4)),
            eval_expr(m.group(5)),
        )
    return meshes


def expand_entrance_steps() -> dict[int, Mesh]:
    """Unroll DrawEntranceSteps variable CreateDisplayList loops."""
    meshes: dict[int, Mesh] = {}
    step = 10000.0
    step_length = 9808.0
    for i in range(258, 274):
        meshes[i] = create_display_list(AXIS["XZ"], 1024.0, 512.0, 31582.0, step, step_length, 2.2, 0.277)
        meshes[i + 16] = create_display_list(
            AXIS["XY"], 64.0, 64.0, 31582.0, step - 64.0, step_length, 35.0, 1.0
        )
        step -= 48.0
        step_length -= 142.0
        if (i + 3) % 4 == 0:
            step_length -= 500.0
            step -= 48.0

    step = 9808.0
    step_length = 8882.0
    for i in range(290, 293):
        meshes[i] = create_display_list(AXIS["XZ"], 1024.0, 512.0, 31582.0, step, step_length, 2.2, 1.0)
        meshes[i + 3] = create_display_list(
            AXIS["XY"], 64.0, 64.0, 31582.0, step - 64.0, step_length, 35.0, 1.0
        )
        step -= 239.0
        step_length -= 1068.0
    return meshes


@dataclass
class DrawInstance:
    list_id: int
    material: str
    tx: float = 0.0
    ty: float = 0.0
    tz: float = 0.0
    # OpenGL post-multiply rotations in call order (applied to local verts first-to-last).
    rotations: tuple[tuple[float, float, float, float], ...] = ()


def _rotate_point(
    x: float, y: float, z: float, angle_deg: float, ax: float, ay: float, az: float
) -> tuple[float, float, float]:
    ang = math.radians(angle_deg)
    c, s = math.cos(ang), math.sin(ang)
    length = math.sqrt(ax * ax + ay * ay + az * az) or 1.0
    ax, ay, az = ax / length, ay / length, az / length
    # Rodrigues
    dot = ax * x + ay * y + az * z
    cx, cy, cz = ay * z - az * y, az * x - ax * z, ax * y - ay * x
    return (
        x * c + cx * s + ax * dot * (1 - c),
        y * c + cy * s + ay * dot * (1 - c),
        z * c + cz * s + az * dot * (1 - c),
    )


def transform_mesh(
    mesh: Mesh,
    tx: float,
    ty: float,
    tz: float,
    rotations: tuple[tuple[float, float, float, float], ...] = (),
) -> Mesh:
    out = Mesh(material_name=mesh.material_name)
    # OpenGL: glTranslate; glRotate A; glRotate B ⇒ M = T * A * B ⇒ apply B then A then T
    ordered = tuple(reversed(rotations))
    for v in mesh.vertices:
        px, py, pz = v.px, v.py, v.pz
        nx, ny, nz = v.nx, v.ny, v.nz
        for angle, ax, ay, az in ordered:
            px, py, pz = _rotate_point(px, py, pz, angle, ax, ay, az)
            nx, ny, nz = _rotate_point(nx, ny, nz, angle, ax, ay, az)
        nlen = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        out.vertices.append(
            Vertex(px + tx, py + ty, pz + tz, nx / nlen, ny / nlen, nz / nlen, v.u, v.v)
        )
    out.indices = list(mesh.indices)
    return out


def make_cylinder(r_base: float, r_top: float, height: float, slices: int = 16) -> Mesh:
    """GLU-style cylinder along +Z from 0..height (side wall only)."""
    mesh = Mesh()
    for i in range(slices):
        a0 = 2 * math.pi * i / slices
        a1 = 2 * math.pi * (i + 1) / slices
        c0, s0 = math.cos(a0), math.sin(a0)
        c1, s1 = math.cos(a1), math.sin(a1)
        u0, u1 = i / slices, (i + 1) / slices
        # outward approx normal in XY
        nx0, ny0 = c0, s0
        nx1, ny1 = c1, s1
        push_quad(
            mesh,
            [
                (r_base * c0, r_base * s0, 0.0, nx0, ny0, 0.0, u0, 0.0),
                (r_base * c1, r_base * s1, 0.0, nx1, ny1, 0.0, u1, 0.0),
                (r_top * c1, r_top * s1, height, nx1, ny1, 0.0, u1, 1.0),
                (r_top * c0, r_top * s0, height, nx0, ny0, 0.0, u0, 1.0),
            ],
        )
    return mesh


def _matching_brace_block(lines: list[str], open_idx: int) -> tuple[list[str], int]:
    """Return body lines inside `{...}` (braces preserved) and index after the closing `}`."""
    depth = 0
    started = False
    body_lines: dict[int, list[str]] = {}
    i = open_idx
    while i < len(lines):
        for ch in lines[i]:
            if ch == "{":
                depth += 1
                started = True
                body_lines.setdefault(i, []).append(ch)
                continue
            if ch == "}":
                depth -= 1
                body_lines.setdefault(i, []).append(ch)
                if started and depth == 0:
                    ordered = ["".join(body_lines[k]) for k in sorted(body_lines)]
                    return ordered, i + 1
                continue
            if started and depth >= 1:
                body_lines.setdefault(i, []).append(ch)
        i += 1
    return [], open_idx + 1


def _extract_for_body(lines: list[str], for_idx: int) -> tuple[list[str], int]:
    """Given a for-line index, return body lines (with braces) and index after the loop."""
    line = lines[for_idx]
    if "{" in line:
        return _matching_brace_block(lines, for_idx)
    rest = line.split(")", 1)[-1].strip()
    if rest and not rest.startswith("{"):
        return [rest], for_idx + 1
    j = for_idx + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j < len(lines) and "{" in lines[j]:
        return _matching_brace_block(lines, j)
    if j < len(lines):
        return [lines[j]], j + 1
    return [], for_idx + 1


def _split_top_level(expr: str, op: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    i = 0
    n = len(op)
    cur: list[str] = []
    while i < len(expr):
        ch = expr[i]
        if ch == "(":
            depth += 1
            cur.append(ch)
            i += 1
            continue
        if ch == ")":
            depth = max(0, depth - 1)
            cur.append(ch)
            i += 1
            continue
        if depth == 0 and expr.startswith(op, i):
            parts.append("".join(cur).strip())
            cur = []
            i += n
            continue
        cur.append(ch)
        i += 1
    parts.append("".join(cur).strip())
    return [p for p in parts if p]


def _eval_condition(expr: str, env: dict[str, float]) -> bool:
    """Evaluate Shay if-conditions: == != > < >= <=, &&, ||, !, parenthesized forms."""
    expr = expr.strip()
    while True:
        if len(expr) >= 2 and expr[0] == "(" and expr[-1] == ")":
            depth = 0
            wrapped = True
            for i, ch in enumerate(expr):
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0 and i != len(expr) - 1:
                        wrapped = False
                        break
            if wrapped:
                expr = expr[1:-1].strip()
                continue
        break

    if expr.startswith("!"):
        return not _eval_condition(expr[1:].strip(), env)

    parts = _split_top_level(expr, "&&")
    if len(parts) > 1:
        return all(_eval_condition(p, env) for p in parts)
    parts = _split_top_level(expr, "||")
    if len(parts) > 1:
        return any(_eval_condition(p, env) for p in parts)

    m = re.fullmatch(r"(\w+)\s*(==|!=|>=|<=|>|<)\s*(-?\d+(?:\.\d+)?)", expr)
    if m:
        left = env.get(m.group(1), -1.0)
        right = float(m.group(3))
        op = m.group(2)
        if op == "==":
            return abs(left - right) < 1e-9
        if op == "!=":
            return abs(left - right) >= 1e-9
        if op == ">":
            return left > right
        if op == "<":
            return left < right
        if op == ">=":
            return left >= right
        if op == "<=":
            return left <= right
    return False


def parse_draw_instances(text: str) -> list[DrawInstance]:
    """Collect glCallList draws, unrolling for-loops and step/step2 translates."""
    lines = text.splitlines()
    instances: list[DrawInstance] = []
    # DisplayExtras rebinds STA/GS signs via int sign = STA_TRAVEL; sign = GS_SIGN;
    tex_vars: dict[str, str] = {}

    def run(body_lines: list[str], mat: str, stack: list[tuple[float, float, float]], env: dict[str, float]) -> str:
        nonlocal instances
        i = 0
        while i < len(body_lines):
            line = body_lines[i]

            # Strip // comments so trailing notes don't break if/for matching
            code = line.split("//")[0]

            # DisplayBanner is expanded explicitly (doge billboard + poles) — skip here
            # so we don't double-count / mis-bind textures on the side poles.
            if re.match(r"\s*void\s+DisplayBanner\b", line):
                depth = 0
                started = False
                while i < len(body_lines):
                    for ch in body_lines[i]:
                        if ch == "{":
                            depth += 1
                            started = True
                        elif ch == "}":
                            depth -= 1
                            if started and depth == 0:
                                i += 1
                                depth = -1
                                break
                    if depth < 0:
                        break
                    i += 1
                continue

            # sign = GS_SIGN; / int sign = STA_TRAVEL;
            svm = re.match(
                r"\s*(?:int\s+)?(sign|signBack|signEdge)\s*=\s*([A-Z][A-Z0-9_]*)\s*;",
                code,
            )
            if svm:
                tex_vars[svm.group(1)] = svm.group(2)
                i += 1
                continue

            bm = re.search(r"tp\.GetTexture\(\s*([A-Za-z0-9_]+)\s*\)", line)
            if bm:
                name = bm.group(1)
                if re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                    mat = name
                elif name in tex_vars:
                    mat = tex_vars[name]
            # numeric GetTexture(220) → DOGE (CreateTexture overwrites EXIT_WEST slot)
            if re.search(r"tp\.GetTexture\(\s*220\s*\)", line):
                mat = "DOGE"

            am = re.match(
                r"\s*(?:GLdouble\s+)?(step2?|stepLength|vertStep|beamstep)\s*=\s*([^;]+);",
                code,
            )
            if am:
                try:
                    env[am.group(1)] = eval_expr(am.group(2), env)
                except Exception:
                    pass
                i += 1
                continue

            am = re.match(
                r"\s*(step2?|stepLength|vertStep|beamstep)\s*\+=\s*([^;]+);",
                line,
            )
            if am:
                try:
                    env[am.group(1)] = env.get(am.group(1), 0.0) + eval_expr(am.group(2), env)
                except Exception:
                    pass
                i += 1
                continue

            am = re.match(
                r"\s*(step2?|stepLength|vertStep|beamstep)\s*-=\s*([^;]+);",
                line,
            )
            if am:
                try:
                    env[am.group(1)] = env.get(am.group(1), 0.0) - eval_expr(am.group(2), env)
                except Exception:
                    pass
                i += 1
                continue

            # if (i == N) step += X;  / if (j == N) step2 += X;
            im = re.search(
                r"if\s*\(\s*(\w+)\s*==\s*(\d+)\s*\)\s*(step2?|stepLength|vertStep)\s*\+=\s*([^;]+);",
                line,
            )
            if im:
                try:
                    if abs(env.get(im.group(1), -1) - float(im.group(2))) < 1e-9:
                        env[im.group(3)] = env.get(im.group(3), 0.0) + eval_expr(im.group(4), env)
                except Exception:
                    pass
                i += 1
                continue

            # if (i == 4) step = 13440.0;  (phys sci upstairs gap jump)
            im_set = re.search(
                r"if\s*\(\s*(\w+)\s*==\s*(\d+)\s*\)\s*(step2?|stepLength|vertStep)\s*=\s*([^;]+);",
                code,
            )
            if im_set:
                try:
                    if abs(env.get(im_set.group(1), -1) - float(im_set.group(2))) < 1e-9:
                        env[im_set.group(3)] = eval_expr(im_set.group(4), env)
                except Exception:
                    pass
                i += 1
                continue

            # if ((i == 0) || (i == 8)) step += 960.0;
            im2 = re.search(
                r"if\s*\(\s*\(\s*(\w+)\s*==\s*(\d+)\s*\)\s*\|\|\s*\(\s*\1\s*==\s*(\d+)\s*\)\s*\)\s*"
                r"(step2?|stepLength|vertStep)\s*\+=\s*([^;]+);",
                line,
            )
            if im2:
                try:
                    iv = env.get(im2.group(1), -1)
                    if abs(iv - float(im2.group(2))) < 1e-9 or abs(iv - float(im2.group(3))) < 1e-9:
                        env[im2.group(4)] = env.get(im2.group(4), 0.0) + eval_expr(im2.group(5), env)
                except Exception:
                    pass
                i += 1
                continue

            # if (j == 6) { ... }  / if ((i == 7) && (j == 0)) { ... }
            # Match against comment-stripped code so trailing // notes don't break detection.
            im3 = re.match(r"\s*if\s*\(\s*(.+?)\s*\)\s*$", code) or re.match(
                r"\s*if\s*\(\s*(.+?)\s*\)\s*\{", code
            )
            if im3 and "glCallList" not in code and "GetTexture" not in code:
                cond_expr = im3.group(1).strip()
                cond = _eval_condition(cond_expr, env)
                if "{" in line:
                    block, next_i = _matching_brace_block(body_lines, i)
                else:
                    j = i + 1
                    while j < len(body_lines) and not body_lines[j].strip():
                        j += 1
                    if j < len(body_lines) and "{" in body_lines[j]:
                        block, next_i = _matching_brace_block(body_lines, j)
                    else:
                        # single-statement if already handled above; skip unknown
                        block, next_i = [], i + 1
                if cond and block:
                    mat = run(block, mat, stack, env)
                i = next_i
                continue

            # Reset transform stack at function boundaries so unbalanced Draw* bodies
            # don't leak into later Display* draws.
            if re.match(r"\s*void\s+(?:Display|Draw)\w*", line):
                stack[:] = [(0.0, 0.0, 0.0)]

            if "glPushMatrix" in line:
                stack.append(stack[-1])
            if "glPopMatrix" in line and len(stack) > 1:
                stack.pop()

            tm = re.search(r"glTranslatef\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^)]+)\)", line)
            if tm and len(stack) > 1:
                try:
                    dx = eval_expr(tm.group(1), env)
                    dy = eval_expr(tm.group(2), env)
                    dz = eval_expr(tm.group(3), env)
                    x, y, z = stack[-1]
                    stack[-1] = (x + dx, y + dy, z + dz)
                except Exception:
                    pass

            fm = re.search(
                r"for\s*\(\s*(?:int\s+)?(\w+)\s*=\s*(\d+)\s*;\s*\1\s*<\s*(\d+)\s*;",
                code,
            )
            if fm:
                var, a, b = fm.group(1), int(fm.group(2)), int(fm.group(3))
                # One-liner range call
                fm_call = re.search(
                    r"for\s*\(\s*(?:int\s+)?(\w+)\s*=\s*(\d+)\s*;\s*\1\s*<\s*(\d+)\s*;[^)]*\)\s*"
                    r"glCallList\s*\(\s*\1\s*\)",
                    code,
                )
                if fm_call:
                    tx, ty, tz = stack[-1]
                    for lid in range(int(fm_call.group(2)), int(fm_call.group(3))):
                        instances.append(DrawInstance(lid, mat, tx, ty, tz))
                    i += 1
                    continue
                fm_call2 = re.search(
                    r"for\s*\([^;]*?=\s*(\d+)\s*;[^;]*?<\s*(\d+)\s*;[^)]*\)\s*glCallList\s*\(\s*\w+\s*\)",
                    code,
                )
                if fm_call2 and "glCallList" in code and "{" not in code:
                    tx, ty, tz = stack[-1]
                    for lid in range(int(fm_call2.group(1)), int(fm_call2.group(2))):
                        instances.append(DrawInstance(lid, mat, tx, ty, tz))
                    i += 1
                    continue

                loop_body, next_i = _extract_for_body(body_lines, i)
                for val in range(a, b):
                    env[var] = float(val)
                    # Fresh copy of stack depth for each iteration? Shay resets via push/pop in body.
                    # Keep same stack object; body should push/pop balanced.
                    mat = run(loop_body, mat, stack, env)
                i = next_i
                continue

            cm = re.search(r"glCallList\s*\(\s*(\d+)\s*\)", code)
            if cm:
                tx, ty, tz = stack[-1]
                instances.append(DrawInstance(int(cm.group(1)), mat, tx, ty, tz))
            else:
                cm2 = re.search(r"glCallList\s*\(\s*(\w+)\s*\)", code)
                if cm2 and cm2.group(1) in env:
                    tx, ty, tz = stack[-1]
                    instances.append(DrawInstance(int(env[cm2.group(1)]), mat, tx, ty, tz))

            i += 1
        return mat

    env0 = {
        "step": 0.0,
        "step2": 0.0,
        "stepLength": 0.0,
        "vertStep": 0.0,
        "beamstep": 0.0,
        "i": 0.0,
        "j": 0.0,
        "k": 0.0,
    }
    run(lines, "MISSING", [(0.0, 0.0, 0.0)], env0)
    # Light fittings use glRotate after translate; bake the known DisplayLights rotations.
    light_rot = ((90.0, 1.0, 0.0, 0.0), (-90.0, 0.0, 0.0, 1.0))
    instances = [
        DrawInstance(i.list_id, i.material, i.tx, i.ty, i.tz, light_rot)
        if i.list_id == 376
        else i
        for i in instances
    ]
    return instances


def parse_geometry(text: str) -> dict[int, Mesh]:
    meshes: dict[int, Mesh] = {}

    # CreateDisplayList (MODE, id, ...) — Shay sometimes writes list ids as 497.0
    for m in re.finditer(
        r"tp\.CreateDisplayList\s*\(\s*(XY_FLIP|YZ_FLIP|XY|XZ|YZ)\s*,\s*(\d+)(?:\.0)?\s*,([^;]+)\);",
        text,
    ):
        mode = AXIS[m.group(1)]
        list_id = int(m.group(2))
        nums = flatten_numbers(m.group(3))
        if len(nums) < 7:
            print(f"warn: CreateDisplayList {list_id} expected 7 nums got {nums}")
            continue
        meshes[list_id] = create_display_list(mode, *nums[:7])

    for m in re.finditer(r"tp\.CreateYtoZWindowList\s*\(\s*(\d+)(?:\.0)?\s*,([^;]+)\);", text):
        list_id = int(m.group(1))
        nums = flatten_numbers(m.group(2))
        if len(nums) >= 7:
            meshes[list_id] = create_yz_window(*nums[:7])

    for m in re.finditer(r"tp\.CreateXtoYWindowList\s*\(\s*(\d+)(?:\.0)?\s*,([^;]+)\);", text):
        list_id = int(m.group(1))
        nums = flatten_numbers(m.group(2))
        if len(nums) >= 7:
            meshes[list_id] = create_xy_window(*nums[:7])

    for m in re.finditer(r"tp\.CreateAngledPolygon\s*\(\s*(\d+)(?:\.0)?\s*,([^;]+)\);", text):
        list_id = int(m.group(1))
        nums = flatten_numbers(m.group(2))
        if len(nums) >= 16:
            img_w, img_h = nums[0], nums[1]
            xs = nums[2:6]
            ys = nums[6:10]
            zs = nums[10:14]
            sx, sz = int(nums[14]), int(nums[15])
            meshes[list_id] = create_angled(img_w, img_h, xs, ys, zs, sx, sz)

    meshes.update(parse_hand_quad_lists(text))
    meshes.update(expand_angled_roof_beams(text))
    meshes.update(expand_entrance_steps())
    # Light fitting cylinder (gluCylinder 75,75,60) — GLU_LINE in original, solid here.
    meshes[376] = make_cylinder(75.0, 75.0, 60.0, 16)
    # DisplayBanner centre billboard (doge.raw on texture slot 220)
    meshes[10001] = mesh_from_corners(
        [
            (-960.0, -540.0, 0.0, 0.0, 1.0),
            (960.0, -540.0, 0.0, 1.0, 1.0),
            (960.0, 540.0, 0.0, 1.0, 0.0),
            (-960.0, 540.0, 0.0, 0.0, 0.0),
        ]
    )
    return meshes


def doge_banner_instances() -> list[DrawInstance]:
    """Side poles + centre doge quad from DisplayBanner."""
    return [
        DrawInstance(10000, "MAIN_POST", 14990.0, 11090.0, 25000.0),
        DrawInstance(10000, "MAIN_POST", 17010.0, 11090.0, 25000.0),
        DrawInstance(10001, "DOGE", 16000.0, 11300.0, 25000.0),
    ]


def parse_list_materials(text: str, defines: dict[str, int]) -> dict[int, str]:
    """Associate display-list IDs with texture define names from Display* bodies."""
    list_to_mat: dict[int, str] = {}
    current = None

    # Process line-by-line for bind / call / simple translate instances
    translate = (0.0, 0.0, 0.0)
    in_push = 0

    for line in text.splitlines():
        bm = re.search(r"tp\.GetTexture\(\s*([A-Z0-9_]+)\s*\)", line)
        if bm:
            current = bm.group(1)

        tm = re.search(
            r"glTranslatef\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^)]+)\)", line
        )
        if tm and in_push > 0:
            try:
                translate = (float(tm.group(1)), float(tm.group(2)), float(tm.group(3)))
            except ValueError:
                pass

        if "glPushMatrix" in line:
            in_push += 1
        if "glPopMatrix" in line:
            in_push = max(0, in_push - 1)
            translate = (0.0, 0.0, 0.0)

        # for (int i = A; i < B; i++) glCallList(i);
        # for (i = A; i < B; ++i) glCallList(i);
        fm = re.search(
            r"for\s*\(\s*(?:int\s+)?(\w+)\s*=\s*(\d+)\s*;\s*\1\s*<\s*(\d+)\s*;[^)]*\)\s*glCallList\s*\(\s*\1\s*\)",
            line,
        )
        if fm and current:
            for lid in range(int(fm.group(2)), int(fm.group(3))):
                list_to_mat[lid] = current

        # also: for (...) { glCallList(i); } on following lines handled loosely via current bind
        fm2 = re.search(
            r"for\s*\(\s*(?:int\s+)?(\w+)\s*=\s*(\d+)\s*;\s*\1\s*<\s*(\d+)\s*;",
            line,
        )
        if fm2 and current and "glCallList" in line:
            for lid in range(int(fm2.group(2)), int(fm2.group(3))):
                list_to_mat[lid] = current

        cm = re.search(r"glCallList\s*\(\s*(\d+)\s*\)", line)
        if cm and current:
            list_to_mat[int(cm.group(1))] = current

        # Range call without matching var name in glCallList(i) already covered; handle
        # for (i = A; i < B; i++) glCallList(i) where type omitted:
        fm3 = re.search(
            r"for\s*\([^;]*?=\s*(\d+)\s*;[^;]*?<\s*(\d+)\s*;[^)]*\)\s*glCallList\s*\(\s*\w+\s*\)",
            line,
        )
        if fm3 and current:
            for lid in range(int(fm3.group(1)), int(fm3.group(2))):
                list_to_mat[lid] = current

    return list_to_mat


def material_category(name: str) -> tuple[float, float, str]:
    n = name.lower()
    if "grass" in n:
        return 0.0, 0.85, "grass"
    if "pave" in n or "step" in n:
        return 0.0, 0.7, "pavement"
    if "brick" in n or "wall" in n:
        return 0.0, 0.75, "brick"
    if "window" in n or "glass" in n:
        return 0.0, 0.15, "window"
    if "roof" in n:
        return 0.0, 0.8, "roof"
    if "light" in n:
        return 0.0, 0.35, "light"
    if "post" in n or "metal" in n or "machine" in n:
        return 0.6, 0.45, "metal"
    return 0.0, 0.6, "default"


def write_scene(
    out_dir: Path,
    meshes_by_list: dict[int, Mesh],
    instances: list[DrawInstance],
    textures: dict[int, dict],
    defines: dict[str, int],
    data_root: Path,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    tex_dir = out_dir / "textures"
    tex_dir.mkdir(exist_ok=True)

    grouped: dict[str, Mesh] = defaultdict(Mesh)
    missing_lists = 0
    for inst in instances:
        base = meshes_by_list.get(inst.list_id)
        if base is None:
            missing_lists += 1
            continue
        mesh = transform_mesh(base, inst.tx, inst.ty, inst.tz, inst.rotations)
        g = grouped[inst.material]
        base_i = len(g.vertices)
        g.vertices.extend(mesh.vertices)
        g.indices.extend(i + base_i for i in mesh.indices)
        g.material_name = inst.material

    if missing_lists:
        print(f"warn: {missing_lists} draw calls referenced unknown list ids")

    for mesh in grouped.values():
        for v in mesh.vertices:
            v.px *= WORLD_SCALE
            v.py *= WORLD_SCALE
            v.pz *= WORLD_SCALE

    materials = []
    mat_index = {}
    for name in sorted(grouped.keys()):
        tid = defines.get(name)
        if name == "DOGE":
            tid = 220
        tex = textures.get(tid, {}) if tid is not None else {}
        if name == "DOGE" and not tex:
            tex = {"path": "data/doge.raw", "w": 1920, "h": 1080}
        src = tex.get("path", "")
        w, h = tex.get("w", 1), tex.get("h", 1)
        out_name = f"{name.lower()}.rgb"
        if src:
            rel = src[5:] if src.replace("\\", "/").startswith("data/") else Path(src).name
            candidate = data_root / rel
            if not candidate.exists():
                candidate = data_root / "windows" / Path(src).name
            if candidate.exists():
                raw = candidate.read_bytes()
                expected = w * h * 3
                if len(raw) < expected:
                    raw = raw + bytes(expected - len(raw))
                if len(raw) >= expected:
                    (tex_dir / out_name).write_bytes(raw[:expected])
                else:
                    print(f"warn: size mismatch {candidate} got {len(raw)} want {expected}")
            else:
                print(f"warn: missing texture {src} -> {candidate}")
        metal, rough, cat = material_category(name)
        mat_index[name] = len(materials)
        materials.append(
            {
                "name": name,
                "texture": f"textures/{out_name}" if src else "",
                "width": w,
                "height": h,
                "metallic": metal,
                "roughness": rough,
                "category": cat,
            }
        )

    vertices: list[float] = []
    indices: list[int] = []
    submeshes = []
    for name, mesh in grouped.items():
        if not mesh.indices:
            continue
        first = len(indices)
        base = len(vertices) // 8
        for v in mesh.vertices:
            vertices.extend([v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.u, v.v])
        indices.extend(base + i for i in mesh.indices)
        submeshes.append(
            {
                "material": mat_index[name],
                "firstIndex": first,
                "indexCount": len(mesh.indices),
            }
        )

    bin_path = out_dir / "scene.bin"
    with bin_path.open("wb") as f:
        f.write(b"SHAY")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<f", WORLD_SCALE))
        f.write(struct.pack("<I", len(vertices) // 8))
        f.write(struct.pack("<I", len(indices)))
        f.write(struct.pack("<I", len(submeshes)))
        f.write(struct.pack("<I", len(materials)))
        f.write(struct.pack(f"<{len(vertices)}f", *vertices))
        f.write(struct.pack(f"<{len(indices)}I", *indices))
        for sm in submeshes:
            f.write(struct.pack("<III", sm["material"], sm["firstIndex"], sm["indexCount"]))
        for mat in materials:
            name_b = mat["name"].encode("utf-8")[:63].ljust(64, b"\0")
            tex_b = mat["texture"].encode("utf-8")[:127].ljust(128, b"\0")
            f.write(name_b)
            f.write(tex_b)
            f.write(struct.pack("<IIff", mat["width"], mat["height"], mat["metallic"], mat["roughness"]))

    meta = {
        "worldScale": WORLD_SCALE,
        "lists": len(meshes_by_list),
        "drawCalls": len(instances),
        "materials": len(materials),
        "vertices": len(vertices) // 8,
        "indices": len(indices),
        "submeshes": len(submeshes),
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2))
    (out_dir / "materials.json").write_text(json.dumps(materials, indent=2))
    print(json.dumps(meta, indent=2))
    print(f"wrote {bin_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--main",
        type=Path,
        default=Path(r"H:\ShaysWorld\Shays Code\Source\main.cpp"),
    )
    ap.add_argument(
        "--data",
        type=Path,
        default=Path(r"H:\ShaysWorld\Shays Code\bin\data"),
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path(r"H:\ShaysWorld\modern\assets"),
    )
    args = ap.parse_args()

    text = args.main.read_text(encoding="utf-8", errors="ignore")
    # Normalize LoadTexture+CreateTexture onto fewer lines for regex
    text_compact = re.sub(r"\r\n", "\n", text)

    defines = parse_defines(text_compact)
    textures = parse_textures(text_compact)
    if not textures:
        # line-by-line fallback
        defines = parse_defines(text_compact)
        textures = {}
        lines = text_compact.splitlines()
        for i, line in enumerate(lines):
            lm = re.search(r'LoadTexture\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)', line)
            if not lm:
                continue
            for j in range(i + 1, min(i + 4, len(lines))):
                cm = re.search(r"CreateTexture\(\s*([A-Z0-9_]+)", lines[j])
                if cm:
                    name = cm.group(1)
                    tid = defines.get(name)
                    if tid is not None:
                        textures[tid] = {
                            "name": name,
                            "path": lm.group(1),
                            "w": int(lm.group(2)),
                            "h": int(lm.group(3)),
                        }
                    break

    meshes = parse_geometry(text_compact)
    instances = parse_draw_instances(text_compact)
    instances.extend(doge_banner_instances())
    # doge.raw is loaded onto texture id 220 (overwrites EXIT_WEST in the original)
    textures[220] = {"name": "DOGE", "path": "data/doge.raw", "w": 1920, "h": 1080}
    defines["DOGE"] = 220
    # Also keep a material map fallback for debugging
    list_mats = parse_list_materials(text_compact, defines)
    print(
        f"defines={len(defines)} textures={len(textures)} meshes={len(meshes)} "
        f"draws={len(instances)} mappedLists={len(list_mats)}"
    )
    # Verify expression fix for list 413
    if 413 in meshes:
        ys = sorted({round(v.py, 3) for v in meshes[413].vertices})
        xs = sorted({round(v.px, 3) for v in meshes[413].vertices})
        print(f"list 413 pre-scale Y={ys} X={xs}")
    write_scene(args.out, meshes, instances, textures, defines, args.data)


if __name__ == "__main__":
    main()
