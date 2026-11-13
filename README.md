# VXN

## Compile on Linux

Requires a C11 compiler (GCC or Clang), CMake 3.16+, Make or Ninja, and Clang
15+ for LLVM IR compilation. SoftFloat is bundled. Linux support is untested;
native windows and input are Windows-only.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/vxnc build examples/hello.vxn -o build/hello
./build/hello
```

## Compile on Windows

Put MinGW-w64 GCC, Clang 15+, CMake 3.16+ and mingw32-make on `PATH`.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
.\build\vxnc.exe build examples/hello.vxn -o build/hello.exe
.\build\hello.exe
```

## Compile programs

Run from the repository root; on Windows use `.\build\vxnc.exe`.

```sh
./build/vxnc build examples/functional_values.vxn -o build/functional-values
./build/vxnc build examples/collections/main.vxn --manifest examples/collections/project.vxnp -o build/collections
./build/vxnc emit-llvm examples/hello.vxn -o build/hello.ll
./build/vxnc build examples/hello.vxn --backend c -o build/hello-c
```

Native builds use LLVM by default. Add `--no-opt` to disable optimization.
Select tools with `--llvm` / `VXN_LLVM`, `--cc` / `VXN_CC`, and runtime archives
with `--runtime` / `VXN_RUNTIME` (`VXN_EXTENDED` for the C backend's SoftFloat
archive). Tools and archives must match the configured target.

## Test, install and package

```sh
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
cmake --build build --target package
cmake --build build --target package_source
```

Configure with `-DBUILD_TESTING=OFF` to omit tests. Keep the installed `bin`,
`lib` and `share` directories together. Native compilation from an installed
package still requires Clang and a compatible C linker driver on `PATH`.

## Language examples

[Terse feature examples](LANGUAGE.txt). Runnable examples are in [examples/](examples/).
