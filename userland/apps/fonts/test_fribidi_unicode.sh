#!/usr/bin/env bash
set -euo pipefail
native_bidi_source=src/Applications/text/fribidi/1.0.17
native_bidi_directory="$TEST_TMPDIR/fribidi"
mkdir -p "$native_bidi_directory"
cp -R "$native_bidi_source/lib" "$native_bidi_directory/lib"
tar -xf userland/apps/fonts/fribidi-generated.arm64.tar -C "$native_bidi_directory/lib"
tar -xf userland/apps/fonts/pango.arm64.tar -C "$native_bidi_directory" usr/include/fribidi
cat > "$native_bidi_directory/config.h" <<'EOF'
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRDUP 1
#define HAVE_STRINGIZE 1
#define STDC_HEADERS 1
#define FRIBIDI_NO_DEPRECATED 0
EOF
native_bidi_sources=(fribidi fribidi-arabic fribidi-bidi fribidi-bidi-types fribidi-char-sets
    fribidi-char-sets-cap-rtl fribidi-char-sets-cp1255 fribidi-char-sets-cp1256
    fribidi-char-sets-iso8859-6 fribidi-char-sets-iso8859-8 fribidi-char-sets-utf8
    fribidi-deprecated fribidi-joining fribidi-joining-types fribidi-mirroring
    fribidi-brackets fribidi-run fribidi-shape)
native_bidi_inputs=()
for native_bidi_file in "${native_bidi_sources[@]}"; do
    native_bidi_inputs+=("$native_bidi_directory/lib/$native_bidi_file.c")
done
for native_bidi_case in BidiTest BidiCharacterTest; do
    clang-20 -std=gnu11 -O2 -DHAVE_CONFIG_H -I "$native_bidi_directory" \
        -I "$native_bidi_directory/usr/include/fribidi" -I "$native_bidi_directory/lib" \
        "$native_bidi_source/test/unicode-conformance/$native_bidi_case.c" \
        "${native_bidi_inputs[@]}" -o "$native_bidi_directory/$native_bidi_case"
    "$native_bidi_directory/$native_bidi_case" \
        "$native_bidi_source/test/unicode-conformance/$native_bidi_case.txt"
done
