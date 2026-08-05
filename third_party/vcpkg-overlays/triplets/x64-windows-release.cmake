# Release-only triplet for CI: same ABI as x64-windows but skips the Debug
# port builds. vcpkg builds every port twice by default (Release + Debug);
# the CI pipeline links Release only, and ffmpeg's Debug build alone costs
# ~19 minutes on the runner. VCPKG_BUILD_TYPE is the documented switch (see
# the community triplet x64-windows-static-release.cmake).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_BUILD_TYPE release)
