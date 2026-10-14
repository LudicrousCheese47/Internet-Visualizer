# Third-party notices

Internet Visualizer is MIT licensed (see `LICENSE`). The following third-party
components are used and are distributed under their own licenses.

## raylib (zlib License)

The windowing/rendering library is pulled in at build time via CMake
`FetchContent` from https://github.com/raysan5/raylib (see `CMakeLists.txt`),
so its sources are not part of this repository. Builds and installers
redistribute the compiled library inside `Internet_Visualizer.exe`.

Raylib is provided under the zlib License:

```
Copyright (c) 2013-2024 Ramon Santamaria (@raysan5)

This software is provided "as-is", without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
```

## stb_image.h (Public Domain / MIT)

`Include/stb_image.h` (v2.30) is a single-header image loader by Sean Barrett
and contributors (http://nothings.org/stb). It is dual-licensed Public Domain
(https://unlicense.org) and MIT; you may use it under either license. The
standard public-domain notice applies:

```
This software is available under 2 licenses -- choose whichever you prefer.
...
Unlicense: This is free and unencumbered software released into the public
domain...
```

## JetBrains Mono (SIL Open Font License 1.1)

`assets/JetBrainsMono-Regular.ttf` and `assets/JetBrainsMono-Bold.ttf` are
Copyright (c) 2020 The JetBrains Mono Project Authors, licensed under the SIL
Open Font License, Version 1.1. The full license text ships alongside the
fonts as `assets/OFL.txt`.

## WiX Toolset (tooling only)

The Windows Installer (.msi) is produced with the WiX Toolset v7
(https://wixtoolset.org/), licensed under the Microsoft Reciprocal License.
WiX is a build-time tool and is **not** redistributed with or required to run
Internet Visualizer. WiX v7 binary releases are covered by the Open Source
Maintenance Fee (OSMF) EULA; see https://docs.firegiant.com/wix/osmf/ for the
terms that apply to your (re)use of the tool itself.