#!/usr/bin/env python3
"""Generates SkinnedQuad.gltf and SkinnedQuad.glb, the fixtures for the skeletal-import tests.

Committed alongside its output so the fixture is auditable: a hand-written glTF with a base64
buffer is unreviewable, whereas this script states the intended geometry and skinning in readable
form. Re-run it after changing anything here:

    python Tests/Data/MakeSkinnedQuad.py

The fixture describes a unit quad in the XY plane bound to two joints:

    Root   at the origin              -- influences the two vertices at y = 0
    Bone1  translated +1 on Y         -- influences the two vertices at y = 1

Each vertex is rigidly bound (single influence, weight 1.0), which makes the expected values in the
test exact rather than approximate.

glTF is right-handed Y-up, so under the plugin's basis change (Unreal.X = -Source.Z,
Unreal.Y = Source.X, Unreal.Z = Source.Y) the source translation (0, 1, 0) becomes (0, 0, 1) --
Bone1 ends up one unit above the root on Unreal's up axis. The test asserts exactly that, which is
what makes it a check on the bone-transform conversion rather than merely on parsing.

It also carries one animation clip, "Wave", one second long, driving Bone1 alone:

    translation  (0, 1, 0) -> (2, 1, 0)      a slide along the source X axis
    rotation     identity  -> 90 degrees about the source Y axis

Two keys, not more, so that sampling anywhere between them exercises interpolation rather than
returning a stored key. The translation and the rotation are on different source axes on purpose:
a basis change that only remaps translation, or that loses the sense of a rotation, passes a
translation-only clip and fails this one.

There is also one morph target, "Bulge", displacing every vertex one unit along source +Z -- which
is one unit along Unreal's -X. The same displacement on every vertex is what makes the expected
result trivial to state: the morphed shape is the base shape translated, so a conversion that got
the basis right moves all four vertices identically, and one that did not moves them apart. The
clip animates its weight from 0 to 1 over the same second.

Both outputs are written from the same data. The .glb is not a separate fixture but the same scene
in the binary container, which is what keeps the two from drifting apart.
"""

import base64
import json
import struct
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# Geometry and skinning
# ---------------------------------------------------------------------------------------------

# Quad in the XY plane. The y = 0 edge belongs to Root, the y = 1 edge to Bone1.
POSITIONS = [
    (0.0, 0.0, 0.0),
    (1.0, 0.0, 0.0),
    (1.0, 1.0, 0.0),
    (0.0, 1.0, 0.0),
]

INDICES = [0, 1, 2, 0, 2, 3]

# Joint index per vertex, rigidly bound so weights are exactly 1.0.
JOINTS = [
    (0, 0, 0, 0),
    (0, 0, 0, 0),
    (1, 0, 0, 0),
    (1, 0, 0, 0),
]

WEIGHTS = [(1.0, 0.0, 0.0, 0.0)] * 4

# Inverse bind matrices, column-major as glTF requires.
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
# Inverse of a +1 translation on Y: the translation column becomes -1.
INVERSE_BONE1 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]

INVERSE_BIND_MATRICES = [IDENTITY, INVERSE_BONE1]

# ---------------------------------------------------------------------------------------------
# Morph target
# ---------------------------------------------------------------------------------------------

MORPH_TARGET_NAME = "Bulge"

# glTF morph targets hold DISPLACEMENTS, not absolute positions. Every vertex moves one unit along
# source +Z, which the plugin's basis change (Unreal.X = -Source.Z) turns into one unit along
# Unreal's -X. Displacing all four vertices by the same vector keeps the expected result trivially
# checkable: the morphed mesh is the base mesh translated, so every vertex must move identically.
MORPH_DISPLACEMENTS = [(0.0, 0.0, 1.0)] * 4

# The weight the mesh rests at. Zero, so the base shape is the undeformed one.
MORPH_REST_WEIGHT = 0.0

# The clip drives the weight from fully off to fully on over its one second.
MORPH_WEIGHT_KEYS = [0.0, 1.0]

# ---------------------------------------------------------------------------------------------
# Animation
# ---------------------------------------------------------------------------------------------

ANIMATION_NAME = "Wave"

# Shared by both samplers, so the clip has a single unambiguous length.
ANIMATION_TIMES = [0.0, 1.0]

# Bone1 slides two units along source X, which is Unreal's +Y.
ANIMATION_TRANSLATIONS = [
    (0.0, 1.0, 0.0),
    (2.0, 1.0, 0.0),
]

# Identity, then a quarter turn about source Y (Unreal's up axis). glTF quaternions are (x, y, z, w).
HALF_SQRT2 = 2 ** 0.5 / 2
ANIMATION_ROTATIONS = [
    (0.0, 0.0, 0.0, 1.0),
    (0.0, HALF_SQRT2, 0.0, HALF_SQRT2),
]

# ---------------------------------------------------------------------------------------------
# Buffer assembly
# ---------------------------------------------------------------------------------------------

def build_buffer():
    """Packs every accessor's data into one buffer, returning the bytes and each view's offset."""
    chunks = []
    offsets = {}
    cursor = 0

    def append(name, data):
        nonlocal cursor
        # glTF requires each accessor's byte offset to be a multiple of its component size; padding
        # to 4 satisfies every type used here.
        pad = (-cursor) % 4
        if pad:
            chunks.append(b"\x00" * pad)
            cursor += pad
        offsets[name] = cursor
        chunks.append(data)
        cursor += len(data)

    append("positions", b"".join(struct.pack("<3f", *p) for p in POSITIONS))
    append("joints", b"".join(struct.pack("<4B", *j) for j in JOINTS))
    append("weights", b"".join(struct.pack("<4f", *w) for w in WEIGHTS))
    append("ibm", b"".join(struct.pack("<16f", *m) for m in INVERSE_BIND_MATRICES))
    append("indices", b"".join(struct.pack("<H", i) for i in INDICES))
    append("times", b"".join(struct.pack("<f", t) for t in ANIMATION_TIMES))
    append("anim_translations", b"".join(struct.pack("<3f", *t) for t in ANIMATION_TRANSLATIONS))
    append("anim_rotations", b"".join(struct.pack("<4f", *r) for r in ANIMATION_ROTATIONS))
    append("morph_displacements", b"".join(struct.pack("<3f", *d) for d in MORPH_DISPLACEMENTS))
    append("morph_weights", b"".join(struct.pack("<f", w) for w in MORPH_WEIGHT_KEYS))

    return b"".join(chunks), offsets


def build_gltf(embed_buffer=True):
    """Assembles the glTF JSON, returning it with the buffer it refers to.

    With embed_buffer the buffer carries a base64 data URI, which is what makes the .gltf a single
    self-contained text file. Without it the buffer declares no URI, which is how a .glb says "my
    bytes are in the BIN chunk".
    """
    buffer_bytes, offsets = build_buffer()

    def view(name, length):
        return {"buffer": 0, "byteOffset": offsets[name], "byteLength": length}

    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "AssimpForUnreal Tests/Data/MakeSkinnedQuad.py",
        },
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "Root", "children": [1]},
            {"name": "Bone1", "translation": [0.0, 1.0, 0.0]},
            {"name": "SkinnedMesh", "mesh": 0, "skin": 0},
        ],
        "skins": [
            {
                "name": "Skin",
                "joints": [0, 1],
                "skeleton": 0,
                "inverseBindMatrices": 3,
            }
        ],
        "meshes": [
            {
                "name": "SkinnedQuad",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2},
                        "indices": 4,
                        "mode": 4,  # triangles
                        "targets": [{"POSITION": 8}],
                    }
                ],
                # The rest weight, and the name. glTF has no first-class place for a morph target's
                # name, so every exporter agrees on this extras convention instead -- and without a
                # name Unreal has nothing to key the morph target by.
                "weights": [MORPH_REST_WEIGHT],
                "extras": {"targetNames": [MORPH_TARGET_NAME]},
            }
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,  # float
                "count": len(POSITIONS),
                "type": "VEC3",
                "min": [0.0, 0.0, 0.0],
                "max": [1.0, 1.0, 0.0],
            },
            {
                "bufferView": 1,
                "componentType": 5121,  # unsigned byte
                "count": len(JOINTS),
                "type": "VEC4",
            },
            {
                "bufferView": 2,
                "componentType": 5126,  # float
                "count": len(WEIGHTS),
                "type": "VEC4",
            },
            {
                "bufferView": 3,
                "componentType": 5126,  # float
                "count": len(INVERSE_BIND_MATRICES),
                "type": "MAT4",
            },
            {
                "bufferView": 4,
                "componentType": 5123,  # unsigned short
                "count": len(INDICES),
                "type": "SCALAR",
            },
            {
                # An animation sampler's input accessor must declare min and max: they are what a
                # reader uses to determine the clip's length without walking every key.
                "bufferView": 5,
                "componentType": 5126,  # float
                "count": len(ANIMATION_TIMES),
                "type": "SCALAR",
                "min": [min(ANIMATION_TIMES)],
                "max": [max(ANIMATION_TIMES)],
            },
            {
                "bufferView": 6,
                "componentType": 5126,  # float
                "count": len(ANIMATION_TRANSLATIONS),
                "type": "VEC3",
            },
            {
                "bufferView": 7,
                "componentType": 5126,  # float
                "count": len(ANIMATION_ROTATIONS),
                "type": "VEC4",
            },
            {
                # Morph displacements. min/max are required on a morph target's POSITION accessor.
                "bufferView": 8,
                "componentType": 5126,  # float
                "count": len(MORPH_DISPLACEMENTS),
                "type": "VEC3",
                "min": [0.0, 0.0, 1.0],
                "max": [0.0, 0.0, 1.0],
            },
            {
                # One weight per target per keyframe; with a single target that is one per key.
                "bufferView": 9,
                "componentType": 5126,  # float
                "count": len(MORPH_WEIGHT_KEYS),
                "type": "SCALAR",
            },
        ],
        "animations": [
            {
                "name": ANIMATION_NAME,
                "samplers": [
                    {"input": 5, "output": 6, "interpolation": "LINEAR"},
                    {"input": 5, "output": 7, "interpolation": "LINEAR"},
                    {"input": 5, "output": 9, "interpolation": "LINEAR"},
                ],
                # Node 1 is Bone1. The root is deliberately left unanimated, so a clip that moved
                # everything would not pass for one that moves the right thing.
                "channels": [
                    {"sampler": 0, "target": {"node": 1, "path": "translation"}},
                    {"sampler": 1, "target": {"node": 1, "path": "rotation"}},
                    # Node 2 is the mesh: a weights channel targets the node that draws it, not the
                    # skeleton.
                    {"sampler": 2, "target": {"node": 2, "path": "weights"}},
                ],
            }
        ],
        "bufferViews": [
            view("positions", len(POSITIONS) * 12),
            view("joints", len(JOINTS) * 4),
            view("weights", len(WEIGHTS) * 16),
            view("ibm", len(INVERSE_BIND_MATRICES) * 64),
            view("indices", len(INDICES) * 2),
            view("times", len(ANIMATION_TIMES) * 4),
            view("anim_translations", len(ANIMATION_TRANSLATIONS) * 12),
            view("anim_rotations", len(ANIMATION_ROTATIONS) * 16),
            view("morph_displacements", len(MORPH_DISPLACEMENTS) * 12),
            view("morph_weights", len(MORPH_WEIGHT_KEYS) * 4),
        ],
        "buffers": [{"byteLength": len(buffer_bytes)}],
    }

    if embed_buffer:
        gltf["buffers"][0]["uri"] = (
            "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii")
        )

    return gltf, buffer_bytes


def build_glb(gltf, buffer_bytes):
    """Packs the JSON and the buffer into the binary container.

    Both chunks are padded to four bytes -- JSON with spaces, BIN with zeros -- because the format
    requires every chunk to start on a four-byte boundary and readers are entitled to trust it.
    """
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((-len(json_bytes)) % 4)

    bin_bytes = buffer_bytes + b"\x00" * ((-len(buffer_bytes)) % 4)

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)

    out = bytearray()
    out += b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I", len(json_bytes)) + b"JSON" + json_bytes
    out += struct.pack("<I", len(bin_bytes)) + b"BIN\x00" + bin_bytes

    return bytes(out)


def main():
    gltf_path = Path(__file__).with_name("SkinnedQuad.gltf")
    gltf, _ = build_gltf(embed_buffer=True)
    gltf_path.write_text(json.dumps(gltf, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {gltf_path} ({gltf_path.stat().st_size} bytes)")

    glb_path = Path(__file__).with_name("SkinnedQuad.glb")
    glb_gltf, buffer_bytes = build_gltf(embed_buffer=False)
    glb_path.write_bytes(build_glb(glb_gltf, buffer_bytes))
    print(f"wrote {glb_path} ({glb_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
