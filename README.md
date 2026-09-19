# AssimpForUnreal — Assimp mesh importer for Unreal Engine 5

An Unreal Engine 5 plugin that imports 3D models in **71 formats** using the
[Open Asset Import Library (Assimp)](https://github.com/assimp/assimp) — including FBX, glTF/GLB
(with Draco), OBJ, PLY, STL, Collada/DAE, 3DS, Blender, DXF, IFC, LWO, MD5, X and many more — both
as **editor assets** and at **runtime in packaged games**.

Two import paths, one conversion:

- **Editor import** — an Interchange translator that turns files Unreal cannot otherwise read into
  real `UStaticMesh` / `UMaterialInstance` / `UTexture2D` assets, with Nanite, LOD generation,
  collision building and reimport supplied by the engine's own factories.
- **Runtime import** — a Blueprint and C++ API for loading models in a packaged game, with
  asynchronous parsing, progress reporting and cancellation.

Both run the same `aiMesh` → `FMeshDescription` converter, so geometry that imports correctly in the
editor behaves identically at runtime.

---

## Status

Assimp **6.0.5** · Unreal Engine **5.8** · **Win64** · 18 automation tests plus a ~400-file corpus sweep, all passing.

This is a **beta**. What works and what does not:

| Area | Status |
|---|---|
| Static meshes — positions, normals, tangents, up to 8 UV channels, vertex colours | **Working** |
| Coordinate and unit conversion (handedness, winding, V-flip, unit scale) | **Working**, verified against exact expected values |
| Materials → `UMaterialInstance` (base colour, metallic, roughness, **specular**, emissive, opacity, two-sided) | **Working** |
| Textures — external files and textures embedded in the source file | **Working** |
| Scene hierarchy → actor/component hierarchy | **Working** |
| Editor import via Interchange → real saved assets | **Working** |
| Runtime import → `UDynamicMeshComponent`, sync and async, progress + cancel | **Working** |
| Skeletons → `USkeleton` + `USkeletalMesh` — hierarchy, reference pose, skin weights | **Working** |
| **Animation → `UAnimSequence`** — baked bone tracks | **Working** |
| Cameras and lights — described in the scene data | **Working** |
| **Morph targets** | **Not implemented** |
| Cameras/lights spawned as components by the runtime API | **Not implemented** (data is exposed; spawning is not) |
| Linux, macOS, Android, iOS | **Not supported** — layout is per-platform, but only Win64 binaries are built |
| Export | **Not supported** by design (Assimp is built with `ASSIMP_NO_EXPORT`) |

### On animation

An animated, skinned file imports as a `USkeleton`, a `USkeletalMesh` and a `UAnimSequence` per clip.
Bone tracks are **baked**: Assimp hands back three independent key arrays per node -- position,
rotation and scale, each with its own times and no interpolation mode -- while Unreal stores one
transform per bone per frame, so the three are evaluated at common frame times. That is what
Interchange asks a translator for, and it is the honest translation rather than a lossy shortcut.

Two details are worth knowing because they are where this usually goes wrong:

- **Ticks per second is a timebase, not a frame rate.** glTF counts in milliseconds (1000), Collada
  in seconds (1). Reading either as a frame rate would bake a glTF clip at 1000 fps and a Collada
  clip at 1 fps, so the declared value is used only when it is plausibly a frame rate and 30 Hz is
  used otherwise. Formats that do state one, such as BVH, are honoured.
- **The coordinate change of basis is a conjugation.** Converting a key's translation, rotation and
  scale separately gives a different rotation axis and a character whose limbs bend the wrong way.
  Each key is recomposed into a matrix and put through the same conversion the reference pose used.

Still missing: **morph targets**. A clip that also animates morph or mesh channels imports its bone
tracks and says so in the import report rather than dropping them silently.

---

## Requirements

- Unreal Engine 5.8 (the Interchange mesh-payload API changed in 5.6, so 5.6+ is the realistic floor;
  only 5.8 is tested)
- Windows, 64-bit
- Git LFS, to fetch the prebuilt Assimp binaries

## Installation

```bash
git lfs install
git clone https://github.com/urloft/AssimpForUnreal.git YourProject/Plugins/AssimpForUnreal
git -C YourProject/Plugins/AssimpForUnreal lfs pull
```

Then regenerate project files and build. There is **no setup step and no CMake run** — the Assimp
binaries are prebuilt and committed, so the plugin compiles on clone.

If the build stops with a message about a missing vendored binary, the LFS objects were not fetched
and the file on disk is still a text pointer. `git lfs pull` fixes it; failing that, rebuild Assimp
from source (below).

---

## Usage

### Editor import

Drag a supported file into the Content Browser. It imports through Interchange like any other asset,
including Reimport.

**Which extensions are claimed** is configurable under *Project Settings → Plugins → Assimp For
Unreal*. The default list covers 50-odd formats that no engine translator handles — `.ply`, `.stl`,
`.dae`, `.3ds`, `.blend`, `.lwo`, `.ms3d`, `.x`, `.ifc`, `.md5mesh` and so on.

`.fbx`, `.obj`, `.gltf` and `.glb` are **deliberately not claimed by default.** Interchange resolves a
translator by registration order and the engine's translators register first, so claiming them would
have no effect while appearing to. The `bClaim*` switches exist for projects that disable the
corresponding engine translator; the editor logs a warning if you enable one without doing that.

Assimp's parsing options (coordinate conversion, normal generation, mesh optimisation) also live in
project settings rather than the per-import dialog. This is a structural consequence of Interchange
calling `Translate()` before pipeline options are applied: a per-import choice could only take effect
by re-parsing the file after the dialog, doubling the cost of every import.

### Runtime import

```cpp
FAssimpImportSettings Settings;
Settings.bImportSkeletalMesh = false;

FAssimpRuntimeImportResult Result;
UAssimpSceneObject* Scene = UAssimpBlueprintLibrary::ImportSceneFromFile(
    this, TEXT("D:/Models/scan.ply"), Settings, Result);

if (Result.bSucceeded)
{
    UAssimpBlueprintLibrary::SpawnSceneAsDynamicMeshComponents(this, Scene, /*bMergeByNode*/ true);
}
```

From Blueprint, use **Import Assimp Scene From File (Async)** for anything user-facing — it parses on
a worker thread and exposes `Progress` and `Completed` pins plus a `Cancel` call. The blocking variant
exists for load screens and tests.

Unlike the editor path, the runtime path calls Assimp directly and never consults the plugin's
extension settings, so it reads **all 71** formats the linked Assimp reports — including `.fbx`,
`.obj`, `.gltf` and `.glb`, which the Interchange path cannot claim.

#### Materials and textures

Every spawned component gets a material slot per source mesh, instanced from
`/AssimpForUnreal/M_AssimpDefault` unless a `BaseMaterial` is passed. This is not merely cosmetic: a
primitive with **zero** material slots produces an empty `FMaterialRelevance`, so the renderer gives
it no pass flags and draws nothing — geometry with no material is invisible, not grey.

Textures load on demand and are cached per source image, so a model sharing a few images across many
materials decodes each one once. Both external files (resolved against the model's directory, with
fallbacks for the absolute paths source files habitually record) and textures embedded in the source
file are handled.

To use your own material, expose any of these parameter names — the values are applied to a dynamic
instance per slot, and any parameter you omit is simply skipped:

| Parameter | Type |
|---|---|
| `BaseColorTexture` `NormalTexture` `RoughnessTexture` `MetallicTexture` `OcclusionTexture` `EmissiveTexture` | Texture |
| `BaseColor` `EmissiveColor` | Vector |
| `Metallic` `Roughness` `Opacity` | Scalar |
| `UseBaseColorTexture` `UseRoughnessTexture` `UseMetallicTexture` `UseEmissiveTexture` `UseOcclusionTexture` `UseOpacityMask` | Scalar (0 or 1) |

The `Use…Texture` scalars matter. An unbound `TextureSampleParameter2D` still samples its *default*
texture, so a channel wired straight from a sampler picks up that default even when the model
supplies no such map — a white emissive default washes the whole model out to a milky pale. Each
scalar drives a `Lerp` between the flat parameter value and the sampled value, and the importer sets
it to 1 for exactly the maps it bound.

They are scalars rather than static switches because a `UMaterialInstanceDynamic` cannot change
static switches at runtime; those need a compiled permutation.

A sampler's declared type must also match its default texture's colour space, or the material fails
to **compile** (`Sampler type is Normal, should be Color`) and the engine substitutes the default
material for the entire thing. `M_AssimpDefault` uses `BaseFlattenNormalMap` for the normal sampler
and `BaseFlattenLinearColor` for the data samplers, both `sRGB = false`.

```cpp
UAssimpBlueprintLibrary::SpawnSceneAsDynamicMeshComponents(
    this, Scene, /*bMergeByNode*/ true, MyBaseMaterial);
```

`M_AssimpDefault` is generated by [`Scripts/GenerateDefaultMaterial.py`](Scripts/GenerateDefaultMaterial.py)
rather than hand-authored, so the parameter contract lives in source control as readable code instead
of an opaque binary. Regenerate it with:

```
UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript ^
    -script="<plugin>/Scripts/GenerateDefaultMaterial.py"
```

#### A note on scale

The plugin reproduces the source file's coordinates faithfully; it does not attempt to guess a
"correct" real-world size. glTF is metres by specification, so those files are scaled by 100 to reach
Unreal centimetres. If a model arrives smaller or larger than expected, the file itself is usually
not authored at real-world scale — many asset-store models are not. Use `UniformScale`, or set
`bApplyFileUnitScale = false` to see raw file units.

---

## Rebuilding Assimp

Only needed to bump the Assimp version, change its build configuration, or add a platform.

```bash
pwsh ./Scripts/BuildAssimp.ps1
```

Requires CMake 3.22+ and MSVC. The script clones Assimp at a pinned tag, builds it, and installs the
headers and binaries into `Source/ThirdParty/AssimpLibrary/`, recording exactly what it did in
`AssimpVersion.json`.

Two choices in that script are worth knowing about:

- **`-DBUILD_SHARED_LIBS=ON`** — Assimp ships as a DLL, not a static library. Assimp bundles its own
  zlib, and linking that statically into Unreal collides with the engine's zlib symbols. A DLL keeps
  Assimp's copy private to itself.
- **`-DLIBRARY_SUFFIX=`** — Assimp otherwise emits a toolset-tagged name like
  `assimp-vc143-mt.dll`, which would force the build rules to guess which MSVC toolset produced the
  binaries. Because `LIBRARY_SUFFIX` is a CMake cache entry declared without `FORCE`, passing it
  empty on the command line makes the target build as a plain `assimp`. This is done at *configure*
  time on purpose: an MSVC import library records the DLL filename to load, so renaming a built DLL
  afterwards produces a library that links fine and then fails to load at run time.

---

## Architecture

```
AssimpLibrary        External   prebuilt Assimp, include paths, delay-load
      |
AssimpCore           Runtime    RAII scene, IO/log/progress bridges, -> FMeshDescription
      |                         no UObjects, Assimp strictly private
      +--------------------+
AssimpRuntime        Runtime    AssimpInterchange   Runtime   UInterchangeAssimpTranslator
  Blueprint API,                        |
  DynamicMesh, async          AssimpEditor         Editor     settings validation
```

The rule the design turns on: **Assimp symbols exist only inside `AssimpCore/Private`.** Nothing in
any public header names an Assimp type, so no consumer inherits Assimp's include paths, its
exception/RTTI requirements, or its `<windows.h>` macro pollution. All Assimp includes funnel through
one guarded header, `AssimpCore/Private/AssimpIncludes.h`.

That constraint is checkable:

```bash
grep -r '#include "assimp/' Source --include=*.h    # must match only AssimpIncludes.h
```

Two other decisions worth stating:

- **The parsed scene is never a `UObject` field.** `FAssimpScene` is a plain RAII type held by
  `TSharedPtr`; `UAssimpSceneObject` is only a handle to it. A `UObject`'s destruction is scheduled by
  the garbage collector, which is the wrong lifetime model for a large non-`UObject` allocation owned
  by a third-party library. There is no way to obtain the raw `aiScene*`.
- **The coordinate change of basis happens exactly once,** in `FAssimpAxisConverter`, as one explicit
  matrix. Assimp's `aiProcess_MakeLeftHanded` / `FlipWindingOrder` / `FlipUVs` are never enabled,
  because composing them with our own conversion is how a model ends up mirrored.

---

## Testing

### Corpus sweep

Beyond the unit tests, the plugin is validated against **Assimp's own test corpus** — the broadest
real-world sample of the formats it claims. Clone `assimp/assimp` and point the sweep at
`test/models`:

```bash
set ASSIMP_CORPUS_DIR=C:/path/to/assimp/test/models
```

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" YourProject.uproject -ExecCmds="Automation RunTests AssimpForUnreal.Corpus.Sweep" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -NoSound -NoPause
```

Run it **separately** from the unit tests — it loads ~400 files and is long enough that bundling it
into one run truncates the others.

`-TestExit` rather than a `; Quit` appended to `-ExecCmds`: the Quit runs immediately, so the session
ends before a single test does and the run reports nothing at all rather than failing.

This is worth doing after any change to the conversion path. It found three crashes the
hand-written fixtures did not:

| Found | Cause |
|---|---|
| Access violation on `Collada/box_nested_animation.dae` | Assimp UV channels can be **sparse** — channel 0 and 2 populated with 1 null. "Below the highest populated channel" does not imply "populated". |
| Access violation inside `Importer::ReadFile` on malformed glTF | Assimp is not memory-safe on hostile input, and a Windows access violation is a *structured* exception that `catch (...)` cannot intercept under `/EHsc`. |
| Silent process death on `glTF2/RecursiveNodes` | Unbounded recursion in our own node walk. Stack overflow kills the process with no catchable exception and no crash log. |

Failures in the sweep are expected and not all defects: the corpus deliberately includes malformed
fixtures, formats requiring absent external files, and encodings Assimp cannot read. Judge it on the
*trend*, not on reaching 100%.


```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" YourProject.uproject -ExecCmds="Automation RunTests AssimpForUnreal" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -NoSound
```

18 tests across four groups. The fixtures in `Tests/Data` are hand-authored ASCII (or generated by a
committed script) so their expected values can be derived by reading them.

The tests deliberately assert geometry, not just success. A handedness or winding mistake still
produces a mesh with the right triangle count, so counting triangles proves nothing; instead:

- `Core.AxisConversion` — three vertices on the source basis axes must land on exact Unreal
  coordinates, pinning down every column of the basis matrix.
- `Core.CubeWindingAndNormals` — every face of a closed cube must be wound so its geometric normal
  points outward. One flip too few or too many fails this.
- `Core.UVsAndMaterials` — the quad's explicit flat normal `(0,0,1)` must convert to `(-1,0,0)` *and*
  agree with the winding-implied normal.
- `Core.SkinnedMesh` — a joint translated `(0,1,0)` in the source must land at `(0,0,1)` in Unreal.
- `Core.Animation` — a baked key applied to a converted point must give the same answer as converting
  the point the source transform produces. That identity holds only if the change of basis was
  applied as a conjugation; remapping the translation alone passes any translation-only clip and
  fails this one.
- `Core.MaterialSpecular` — a white Phong `Ks`, which is what an exporter writes when nobody chose
  anything, must land on Unreal's neutral `Specular` of 0.5 and leave the model looking untouched;
  the specular exponent must order two materials the way the file does.
- `Interchange.ImportStaticMesh` — a `.ply` becomes a real `UStaticMesh` with its topology intact.
- `Interchange.ImportSkeletalAnimation` — a `.dae` becomes a `USkeleton`, a `USkeletalMesh` and a
  `UAnimSequence` whose bone track moves the right way. Collada rather than glTF on purpose:
  Interchange picks a translator by iterating a set, so where the engine claims the same extension
  the winner is unspecified and the test would prove nothing about this plugin.

### Packaging is also a test

```bash
RunUAT.bat BuildPlugin -Plugin=<path>\AssimpForUnreal.uplugin -Package=<short-output-path> -TargetPlatforms=Win64
```

Worth running before any release, and CI runs it on every push. `BuildPlugin` compiles **without
unity builds**, which is the only thing that catches a missing `#include`: in a unity build the
translation units are concatenated, so a header one file forgot is usually supplied by a neighbour,
and the omission stays invisible until a consumer builds the plugin in a different configuration.

Use a short output path — `BuildPlugin` nests a host project inside the output directory, and the
resulting intermediate paths hit Windows' 260-character limit surprisingly easily.

Run the header-hygiene check too; CI enforces it:

```bash
grep -r '#include "assimp/' Source --include=*.h    # must match only AssimpIncludes.h
```

---

## Comparison with `irajsb/UE4_Assimp`

[`UE4_Assimp`](https://github.com/irajsb/UE4_Assimp) is the established Assimp plugin for Unreal
(178★). It targets UE 5.4.3 and was last updated in April 2025.

| | UE4_Assimp | AssimpForUnreal |
|---|---|---|
| Editor import to saved assets | No — runtime only | Yes, via Interchange |
| Nanite / LOD / collision / reimport | Not applicable | Inherited from engine factories |
| Assimp binaries | Git submodule; you run CMake | Prebuilt and committed; compiles on clone |
| Assimp in public headers | Yes (`AIScene.h` includes `assimp/scene.h`) | Never outside `AssimpCore/Private` |
| Assimp's private `code/` on the public include path | Yes | No |
| Scene lifetime | Raw `aiScene*` in a `UObject`, manual `BeginDestroy` | RAII, `TSharedPtr`, pointer never exposed |
| Custom file IO | No | Yes, over Unreal's `IFileManager` |
| Progress / cancellation | No | Yes, both |
| Automated tests | No | 18 |
| Runtime Blueprint import | Yes | Yes |
| Animation import | Partial | Yes — baked bone tracks to `UAnimSequence` |
| Platforms | Win64, Mac, Linux, Android | Win64 only |

Two concrete defects in that plugin this one avoids by construction:

- Its open issue *"Parent C++ project compile error (UpdateResourceW)"* is Assimp's headers dragging
  Win32 macros into unrelated consumer code, a direct consequence of exposing Assimp in public
  headers. Confining Assimp to private translation units behind Unreal's `AllowWindowsPlatformTypes`
  guard removes the possibility.
- Its delay-load declaration passes a *full path* to `PublicDelayLoadDLLs`. Delay-load entries must be
  the bare filename as it appears in the import table, so that hook never fires and DLL resolution
  silently falls back to the default search order — which is the classic reason a plugin works
  in-editor and then cannot find its DLL in a packaged build.

Where `UE4_Assimp` is still ahead: platform coverage.

---

## Licence

MIT — see [`LICENSE`](LICENSE).

Assimp is redistributed in binary form under the BSD 3-Clause licence. **If you ship a game built
with this plugin you are redistributing Assimp and must include its copyright notice.** See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
