# Third-party notices

## src/third_party

- `cgltf.h`: cgltf 1.15, MIT License, Copyright (c) 2018-2021 Johannes Kuhlmann. Its license is at
  the end of the file.
- `stb_image.h` (v2.30) and `stb_dxt.h` (v1.12): by Sean Barrett and contributors (stb_dxt originally
  by Fabian "ryg" Giesen), public domain or MIT, at your choice. The licenses are at the end of
  each file.

## The bone-name table in tools/unity2hypr3d.py

`BONE_NAMES` in `tools/unity2hypr3d.py` is the bone-name table of Modular Avatar's
`HeuristicBoneMapper` (https://github.com/bdunderscore/modular-avatar, version 1.18.7). Modular
Avatar's code is under the MIT License below. Its source credits the table's origins, which are MIT
too:

- HhotateA's AvatarModifyTools (https://github.com/HhotateA/AvatarModifyTools), Copyright (c) 2021
  @HhotateA_xR, MIT License
- Azukimochi's BoneRenamer (https://github.com/Azukimochi/BoneRenamer), Copyright (c) 2023
  Azukimochi, MIT License

```
MIT License

Copyright (c) 2022 bd_

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The rest of the converter's Modular Avatar support reimplements MA's behaviour in Python. It was
written from reading MA's source, and the notice above covers it too.
