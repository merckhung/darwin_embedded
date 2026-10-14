# Linux IOKit interface generator

This imports PureDarwin's BSD-licensed generator at the commit recorded in
`UPSTREAM.json`. It replaces the unavailable macOS host IIG for Linux builds.
Local updates preserve symbolic fixed-size array bounds in current
`IOUserServer.iig` and generate the `IORPC.kernelContent` field introduced in
the current XNU kernel ABI. Kernel dispatch rejects null content.

Run `bazel test //tools/iig:generator_test //tools/mig:generator_test` for
generator regressions. Compiling XNU's generated DriverKit implementations
provides integration coverage against the actual kernel headers.

This generator implements a kernel-oriented subset of IIG. Its method IDs use
PureDarwin's FNV-1a hashing. Compatibility with Apple's independently generated
DriverKit user drivers is not established; an in-kernel build does not prove
that compatibility.
