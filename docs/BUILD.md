# build

windows x64, msvc, c++20, cmake 3.20+. abrir o developer powershell x64.
release usa `/MT`; debug usa `/MTd`.

```powershell
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --parallel
ctest --test-dir build/x64 -C Release --output-on-failure
```

saída: `build/x64/Release/izanagi.dll`. para debug, trocar `Release` por `Debug`.

link: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `user32.lib`, `kernel32.lib`, `gdi32.lib`, `dwmapi.lib`, `psapi.lib`.

o milestone 5 adiciona `izanagi_schema_tests` no CMake. a lista de fontes da DLL
inclui `src/reflection/*.cpp` e `src/source2/abi/schema_abi.cpp`; não há flags
MSVC ou bibliotecas novas. detalhes do perfil ABI em `MILESTONE5.md`.
