  cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/release
  build/release/vxnc.exe build examples/functional_orbits/viewer.vxn --manifest examples/functional_orbits/viewer.vxnp -o build/orbit-gallery.exe
  build/orbit-gallery.exe

