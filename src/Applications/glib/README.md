# GLib upstream sources

These are source imports for native Darwin builds, with no Linux ABI layer. The
archives are pinned to the upstream releases available on 2026-10-12. Every
release directory includes `UPSTREAM.json` with its archive URL, SHA-256, license,
complete regular-file hashes, and any packaging-only path changes. Original
license notices and source bytes are retained.

| Project | Release | Primary archive |
| --- | --- | --- |
| GLib | 2.90.1 | [GNOME](https://download.gnome.org/sources/glib/2.90/glib-2.90.1.tar.xz) |
| PCRE2 | 10.49 | [PCRE2 release](https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49) |
| libffi | 3.8.0 | [libffi release](https://github.com/libffi/libffi/releases/tag/v3.8.0) |
| libiconv | 1.19 | [GNU](https://ftp.gnu.org/gnu/libiconv/libiconv-1.19.tar.gz) |
| gettext | 1.0 | [GNU](https://ftp.gnu.org/gnu/gettext/gettext-1.0.tar.xz) |

GLib's hash was checked against the published GNOME SHA-256 file. PCRE2 and
libffi hashes were checked against the primary GitHub release asset digests.
GNU archive hashes record the bytes downloaded from GNU's HTTPS archive; the
manifest does not claim a separate signature or published-checksum verification.

Nested upstream Bazel files in PCRE2 are renamed with their original names
recorded in the manifest so this repository controls package discovery. Upstream
Meson, Autoconf, and other recipes are retained as documentation; the native build
reads their inventories and executes only Bazel-managed compiler actions.

The corresponding build and current limitations are documented in
[`userland/apps/glib`](../../../userland/apps/glib/README.md).
