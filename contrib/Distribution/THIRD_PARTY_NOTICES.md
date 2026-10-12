# Third-party notices

SkyrimRenderer is MIT-licensed (see `LICENSE`). It includes or is derived from
the following third-party work, used under the licences reproduced below.

## Code derived from other projects

| Project | Used for | Licence |
|---|---|---|
| [HKX2Library](https://github.com/ret2end/HKX2Library) | HKX packfile parsing and spline animation decompression (`HKXParser`, `SplineDecompressor`), translated from C# | MIT, Copyright (c) 2021 kreny |
| [Skyrim-SE-Shader-Tools](https://github.com/aers/Skyrim-SE-Shader-Tools) | Lighting math in the HLSL shaders (`shaders/`) | MIT, Copyright (c) 2020 aers |

## Libraries linked into SkyrimRenderer.dll

| Library | Licence |
|---|---|
| [CommonLibSSE-NG / CommonLibVR](https://github.com/alandtse/CommonLibVR) | MIT, Copyright (c) 2018 Ryan-rsm-McKenzie |
| [DirectXTK](https://github.com/microsoft/DirectXTK) | MIT, Copyright (c) Microsoft Corporation |
| [DirectXMath](https://github.com/microsoft/DirectXMath) | MIT, Copyright (c) Microsoft Corporation |
| [spdlog](https://github.com/gabime/spdlog) | MIT, Copyright (c) 2016 Gabi Melman |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT, Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors |
| [xbyak](https://github.com/herumi/xbyak) | BSD-3-Clause, Copyright (c) 2007 MITSUNARI Shigeo |
| [rapidcsv](https://github.com/d99kris/rapidcsv) | BSD-3-Clause, Copyright (c) 2017, Kristofer Berggren |

## References (no code used)

- NIF reading (`NifReader`) was written from the
  [NifTools nif.xml](https://github.com/niftools/nifxml) format description (BSD-3-Clause).
- Community Shaders: when the user has it installed, SkyrimRenderer loads that
  install's own compiled shaders and live settings at runtime. Nothing from
  Community Shaders is included in SkyrimRenderer.

---

## MIT License

Applies to HKX2Library, Skyrim-SE-Shader-Tools, CommonLibSSE-NG / CommonLibVR,
DirectXTK, DirectXMath, spdlog and {fmt}, each with its copyright line above.

```
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

{fmt} additionally grants: "As an exception, if, as a result of your compiling
your source code, portions of this Software are embedded into a
machine-executable object form of such source code, you may redistribute such
embedded portions in such object form without including the above copyright
and permission notices."

## BSD 3-Clause License

Applies to xbyak and rapidcsv, each with its copyright line above.

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
