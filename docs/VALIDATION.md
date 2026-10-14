# Runtime validation and screenshots

The README screenshots were refreshed on October 14, 2026, from fresh boots
of both roots on ARM64 and x86_64 using `BCM2711Platform` and `pc_uefi`.
Commands execute inside each guest: `uname -srm`, `uname -v`, a Mach-O header
read from `/bin/busybox`, and `ps aux`.

`uname` queries real Darwin kernel sysctl data. Its version string identifies
Darwin 25.5.0 and XNU 12377.121.6. `od` reads the installed executable: first
word `feedfacf` is 64-bit Mach-O magic, followed by CPU `0100000c` for ARM64 or
`01000007` for x86_64. `ps aux` lists actual processes, including PID 1 and,
in graphical roots, the desktop processes.

Desktop images are lossless PNG conversions of QEMU framebuffer dumps.
For those captures, the guest serial shell sends the inspection output to the
original terminal through its real Darwin PTY. Desktop process rows and
columns are selected for readability. Hardware keyboard/interactive-shell input is verified
separately by the fresh challenge checks described below.
Minimal images capture only a live xterm serial-console window on a private
Xvfb display; `df -h /` also shows the 64 MiB RAM disk. No kernel banners,
output labels or desktop pixels are synthesized. Screenshots show runtime
evidence, not cryptographic attestation. The [validation record](validation.json)
binds captures to hashed boot inputs, inventories and completed checks,
without workstation paths.

## Reproduce

```sh
bazel build //minimal_rootfs:images //rootfs:images
bazel run //userland/apps/init:verify_arm64
bazel run //userland/apps/init:verify_x86_64
bazel run //rootfs:verify_arm64
bazel run //rootfs:verify_x86_64
```

Launchers retain private copies and SHA-256 digests of boot inputs. Checks
require the actual Darwin boot banner, correct kernel/architecture and a fresh
nonce returned by native programs. Panics, wrong identity, timeouts and failed
child status fail verification. Generated logs stay in ignored `.cache` paths.

Minimal checks exercise PID 1 parentage, reaping, shell respawn, files,
permissions, pipelines, text processing, SHA-256 and archive/compression round
trips. The process check observes a real sleep child, then requires its absence
after kill/wait. Desktop checks additionally verify automatic original Xfce
startup, window geometry, framebuffer content and original terminal/VTE input
through emulated hardware and the real Darwin PTY. A child must read a fresh
challenge and its shell must execute a hardware-typed command.

Inspect the visible evidence interactively with:

```sh
uname -srm
uname -v
od -A x -t x4 -N 32 /bin/busybox
ps aux
```

Use `bazel query 'kind(sh_binary, //userland/...)'` to list component checks.
Compilation alone does not establish runtime behavior.

All four refreshed captures use the replacement Pi 4B platform module or PC
UEFI loader. The [validation record](validation.json) contains their current
boot-input hashes, kernel banners, utility and desktop check results, and
screenshot hashes. The earlier [platform validation record](platform-validation.json)
also covers framebuffer ownership and keyboard/mouse driver checks. Neither
record establishes physical-board boot or ARM SMP support.

## Publication checks

The source snapshot retains upstream notices, provenance and licenses.
Superseded notes, workstation paths, private review configuration and captures
of the host desktop were removed. Firmware comes from a pinned publisher
package with package/member hashes and its copyright inventory. The kernel
build identity uses a project name.

The earlier publication secret scan checked the cleaned source snapshot;
it was not repeated for this platform and screenshot update. Original upstream
crypto test keys and known vectors remain in their source imports; scanner matches
do not make those fixtures private credentials. See the validation record for
counts and classifications. The public repository starts from a clean source
snapshot, excluding earlier development history, build caches and local metadata.
