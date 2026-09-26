# build

windows x64, msvc, c++20, cmake 3.20+. abrir o developer powershell x64.
release usa `/MT`; debug usa `/MTd`.

```powershell
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --parallel
```

saída: `build/x64/Release/izanagi.dll`. para debug, trocar `Release` por `Debug`.

link: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `user32.lib`, `kernel32.lib`, `gdi32.lib`, `dwmapi.lib`, `psapi.lib`.
