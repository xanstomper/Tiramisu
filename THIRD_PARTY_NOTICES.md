# Third-party notices

## DesktopMode_uwp / Win32Bridge — Pedro1234-code (Pdawg11239)

Source: https://github.com/Pedro1234-code/DesktopMode_uwp (`Win32Bridge/`)
License: MIT — Copyright (c) 2023 Pdawg11239

Win32Bridge is an experimental compatibility layer that runs native Windows
x64 applications inside a UWP shell on Windows and Xbox Developer Mode. Its
design was studied and the following Tiramisu components are clean-room
rewrites **mapped from** it (new code, Tiramisu conventions: u16 strings, VFS,
resource bridge, Win64-ABI thunks — no source is copied verbatim):

| Tiramisu | Derived from (Win32Bridge) | What was taken |
| --- | --- | --- |
| `src/trt/bridge_actctx.{h,cpp}` | `Bridge/ActivationContext.{h,cpp}` | Activation-context model: manifest identity/dependency/run-level/supportedOS/WindowsSettings parsing, per-thread activation stack with cookies, process-default context, `QueryActCtxW`/`QueryActCtxSettingsW` information classes |
| `src/trt/shell_link.{h,cpp}` | `Bridge/ShellLinkReader.h` | MS-SHLLINK reader hardening rules: size cap, CLSID check, bounded reads, LinkInfo local-path assembly, StringData order |

Full MIT text follows (required by the license):

```
MIT License

Copyright (c) 2023 Pdawg11239

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
