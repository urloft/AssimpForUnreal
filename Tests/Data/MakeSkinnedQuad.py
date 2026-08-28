#!/usr/bin/env python3
"""Generates SkinnedQuad.gltf, the fixture for the skeletal-import tests.

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

    return b"".join(chunks), offsets


def build_gltf():
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
                    }
                ],
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
        ],
        "bufferViews": [
            view("positions", len(POSITIONS) * 12),
            view("joints", len(JOINTS) * 4),
            view("weights", len(WEIGHTS) * 16),
            view("ibm", len(INVERSE_BIND_MATRICES) * 64),
            view("indices", len(INDICES) * 2),
        ],
        "buffers": [
            {
                "byteLength": len(buffer_bytes),
                "uri": "data:application/octet-stream;base64,"
                + base64.b64encode(buffer_bytes).decode("ascii"),
            }
        ],
    }

    return gltf


def main():
    output_path = Path(__file__).with_name("SkinnedQuad.gltf")
    gltf = build_gltf()
    output_path.write_text(json.dumps(gltf, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {output_path} ({output_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
