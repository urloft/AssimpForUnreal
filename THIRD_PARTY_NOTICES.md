# Third-Party Notices

AssimpForUnreal redistributes the following third-party software in binary form.
The full licence text for each is vendored alongside the binaries.

---

## Open Asset Import Library (Assimp)

- **Version:** 6.0.5
- **Homepage:** https://github.com/assimp/assimp
- **Licence:** BSD 3-Clause
- **Full licence text:** [`Source/ThirdParty/AssimpLibrary/LICENSE.assimp.txt`](Source/ThirdParty/AssimpLibrary/LICENSE.assimp.txt)

Assimp is redistributed as a prebuilt shared library
(`Source/ThirdParty/AssimpLibrary/bin/<Platform>/`) together with its public headers
(`Source/ThirdParty/AssimpLibrary/include/`). It is built from unmodified upstream sources at
tag `v6.0.5` by [`Scripts/BuildAssimp.ps1`](Scripts/BuildAssimp.ps1); that script records the
exact CMake configuration used.

The BSD 3-Clause licence requires that redistributions in binary form reproduce the copyright
notice, the list of conditions, and the disclaimer. **If you ship a game or application built
with this plugin, you are redistributing Assimp in binary form** and must therefore include the
Assimp copyright notice in your product's documentation or attribution screen.

> Copyright (c) 2006-2026, assimp team. All rights reserved.

Assimp's licence also states that neither the name of the assimp team nor the names of its
contributors may be used to endorse or promote derived products without prior written permission.

### Bundled within Assimp

Assimp is built with `ASSIMP_BUILD_ZLIB=ON`, which statically links **zlib**
(https://zlib.net, zlib licence) into the Assimp shared library. zlib's licence is permissive
and requires only that its origin not be misrepresented.

Individual Assimp format importers vendor further permissively licensed third-party code
(for example OpenDDLParser, utf8cpp, rapidjson, stb_image, and Poly2Tri). See
`contrib/` in the upstream Assimp repository for the licence of each.

---

## AssimpForUnreal itself

Licensed under the MIT Licence — see [`LICENSE`](LICENSE).
