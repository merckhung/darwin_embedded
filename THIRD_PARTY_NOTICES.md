# License scope and third-party notices

The project's original integration code is licensed under [BSD-2-Clause](LICENSE).
[NOTICE](NOTICE) defines its scope. Imported code and changes to imported code
retain their upstream terms; the project license at the repository root does not
relicense them. Copyright and attribution notices are preserved even when they
refer to other projects.

PureDarwin-original code retains the BSD 2-Clause license and copyright notice
in [PUREDARWIN_LICENSE.txt](PUREDARWIN_LICENSE.txt), copied unchanged from the
[pinned upstream revision](https://github.com/PureDarwin/PureDarwin/blob/bd832c0035744ff3edc2f6259853a30bf1c624e7/PUREDARWIN_LICENSE.txt).
Apple-derived code retains its APSL/BSD and other per-file terms. The related
EFI loader retains its BSD 3-Clause license.

The [component inventory](docs/licenses.json) lists provenance records and
retained license files with SHA-256 digests. License texts and individual source
headers determine the terms for a file. A component can contain multiple
licenses, including different terms for tests, documentation and bundled code.
The inventory is a navigation aid, not a declaration that all files share one
license. Regenerate it with `python3 scripts/license_inventory.py`.

| Component | Principal terms and retained notices |
| --- | --- |
| PureDarwin-original code | BSD-2-Clause; retain PUREDARWIN_LICENSE.txt and individual source notices. |
| XNU, Darwin libraries and kernel extensions | APSL 2.0, BSD and other per-file terms; Apple's license files and source headers are retained. Changes to imported sources retain those terms. |
| LLVM libc++, libc++abi, libunwind and compiler-rt | Apache 2.0 with LLVM exceptions, plus per-file notices; see each import's license files. |
| dash and OpenLibm | BSD, MIT, ISC and other per-file terms; see dash's COPYING and OpenLibm's LICENSE.md. |
| BusyBox 1.38.0 and the Darwin ps/df ports | GPL-2.0-only; see BusyBox LICENSE and the two port source headers. |
| Xorg and X11 libraries | Primarily permissive X11/MIT/BSD terms; retain each COPYING file and source notices. |
| GLib, GTK, Pango, GdkPixbuf, accessibility and text libraries | Mainly LGPL 2.1 or later, with component-specific and bundled-file exceptions. libffi is MIT and PCRE2 is BSD. |
| Cairo and rendering dependencies | Cairo: LGPL 2.1 or MPL 1.1; FreeType: its FTL/GPL licensing choices; other libraries retain their own permissive notices. |
| Xfce desktop components | GPL/LGPL as specified by each component and file. xfce4-terminal source is GPL 2 or later. |
| VTE 0.84.1 | Mainly LGPL 3; some GPL 3, Xterm and other compatible sources; documentation includes CC-BY 4.0. See COPYING.README and source headers. |
| D-Bus | AFL 2.1 or GPL 2 or later, with permissive files and GPL-only utilities; see COPYING. |
| DejaVu fonts and desktop assets | Bitstream Vera/Arev font terms, public-domain DejaVu changes, and asset-specific notices; these assets are not covered by the project's BSD license. |
| GNU-EFI and EFI loader | BSD-style and per-file terms; retain source headers and the loader's BSD license. |
| OVMF / EDK II firmware | Pinned publisher package; the complete publisher COPYRIGHT inventory is retained in bootloader/ovmf. It includes BSD and other per-file notices. |
| Host tools | libdmg-hfsplus is GPL 3; Apple-derived tools include APSL/BSD code; other tools retain their source licenses. These tools run on the build host. |

## Source and binary redistribution

This repository supplies source, build rules, provenance and notices. It does
not ship a prebuilt guest disk image. Keep the imported license texts and dated
modification notices with a source distribution. Do not strip upstream test
fixtures or attribution merely because they contain keys or external names.

A built root contains components with separate licensing obligations. GPL
binaries require compliant corresponding source and installation/build
information where applicable. LGPL libraries can impose relinking and
modification requirements. APSL-covered modifications have source-availability
and notice obligations; consult [Apple's APSL text](https://opensource.apple.com/apsl/).

BusyBox and the original Darwin applet ports remain GPL-2.0-only; see
[BusyBox's license guidance](https://busybox.net/license.html). VTE's LGPL/GPL 3
terms also need to be considered when distributing the terminal executable.
The BSD license for original integration code does not replace these terms.

Binary redistribution of BusyBox linked to this source-built Darwin runtime
still requires a component-level assessment of GPL-2.0-only, APSL-covered code,
and the applicable system-library exception. This review does **not** assert
that the exception applies or that every combined binary is cleared for
redistribution. Publish the source and notices; complete that assessment before
publishing guest image releases under a claimed uniform license.
