# Embedded kernel collection builder

This is the pinned PureDarwin collection builder, narrowed to the project's
thin x86_64 and ARM64 Mach-O inputs. Provenance and modifications are recorded
in `UPSTREAM.json`; the original BSD-3-Clause notice remains in `LICENSE`.

Build with `bazel build //tools/kc-tools:kc-builder` and test the input view
with `bazel test //tools/kc-tools:view_test`. The kernel and desktop Bazel
targets supply kernels, linked extensions and codeless KPI bundles.

The tool emits `MH_FILESET` collections. The shared prelink assembly library
is still required internally to construct the collection payload. Standalone
prelinked-kernel output, 32-bit ARM/Thumb, RISC-V and inactive CMake/Nix recipes
have been removed. x86_64's 32-bit relocation fields remain necessary even in
64-bit images.
