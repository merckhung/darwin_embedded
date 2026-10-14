/* Genuine native graphics prerequisites: parsers, streams, fonts and pixels.
 * The BDF below is original test input. The TrueType fixture is copied
 * unchanged from FreeType 2.15.0 tests/data/varc-static-gvar.ttf. */
#include <errno.h>
#include <expat.h>
#include <fcntl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <pixman.h>
#include <png.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zlib.h>

static int failure(const char *component, int code)
{
    fprintf(stderr, "Native graphics failure: %s check %d (errno=%d)\n", component, code, errno);
    return 1;
}

static unsigned little32(const unsigned char *bytes)
{
    return (unsigned)bytes[0] | (unsigned)bytes[1] << 8 |
           (unsigned)bytes[2] << 16 | (unsigned)bytes[3] << 24;
}

static int compression(const char *nonce)
{
    unsigned char plain[4096], packed[8192], decoded[4128];
    for (unsigned i = 0; i < sizeof(plain); ++i) plain[i] = (unsigned char)((i * 17) ^ (i >> 4));
    if (strcmp(zlibVersion(), ZLIB_VERSION) ||
        crc32(0, (const Bytef *)"123456789", 9) != 0xcbf43926UL) return 1;
    z_stream stream = {0};
    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) return 2;
    size_t fed = 0, used = 0;
    int result;
    do {
        if (!stream.avail_in && fed < sizeof(plain)) {
            unsigned count = sizeof(plain) - fed > 37 ? 37 : (unsigned)(sizeof(plain) - fed);
            stream.next_in = plain + fed;
            stream.avail_in = count;
            fed += count;
        }
        if (sizeof(packed) - used < 19) { deflateEnd(&stream); return 3; }
        stream.next_out = packed + used;
        stream.avail_out = 19;
        result = deflate(&stream, fed == sizeof(plain) ? Z_FINISH : Z_NO_FLUSH);
        used += 19 - stream.avail_out;
        if (result != Z_OK && result != Z_STREAM_END) { deflateEnd(&stream); return 4; }
    } while (result != Z_STREAM_END);
    if (stream.total_in != sizeof(plain) || deflateEnd(&stream) != Z_OK) return 5;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK) return 6;
    fed = 0;
    size_t produced = 0;
    do {
        if (!stream.avail_in && fed < used) {
            unsigned count = used - fed > 11 ? 11 : (unsigned)(used - fed);
            stream.next_in = packed + fed;
            stream.avail_in = count;
            fed += count;
        }
        if (sizeof(decoded) - produced < 29) { inflateEnd(&stream); return 7; }
        stream.next_out = decoded + produced;
        stream.avail_out = 29;
        result = inflate(&stream, Z_NO_FLUSH);
        produced += 29 - stream.avail_out;
        if (result != Z_OK && result != Z_STREAM_END) { inflateEnd(&stream); return 8; }
    } while (result != Z_STREAM_END);
    if (inflateEnd(&stream) != Z_OK || produced != sizeof(plain) ||
        memcmp(decoded, plain, sizeof(plain))) return 9;
    uLongf size = sizeof(decoded);
    packed[used - 1] ^= 1;
    result = uncompress(decoded, &size, packed, (uLong)used);
    packed[used - 1] ^= 1;
    if (result != Z_DATA_ERROR) return 10;
    size = sizeof(decoded);
    if (uncompress(decoded, &size, packed, (uLong)(used / 2)) == Z_OK) return 11;

    char path[128];
    if (snprintf(path, sizeof(path), "/tmp/native-graphics-%s.gz", nonce) >= (int)sizeof(path)) return 12;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return 13;
    gzFile file = gzdopen(fd, "wb9");
    if (!file) { close(fd); unlink(path); return 14; }
    int error = 0;
    if (gzwrite(file, plain, 1234) != 1234 ||
        gzwrite(file, plain + 1234, sizeof(plain) - 1234) != (int)(sizeof(plain) - 1234) ||
        gzseek(file, sizeof(plain) + 16, SEEK_SET) != (z_off_t)(sizeof(plain) + 16) ||
        gzwrite(file, nonce, 48) != 48 || gztell(file) != (z_off_t)(sizeof(plain) + 64)) error = 15;
    if (gzclose(file) != Z_OK) error = 16;
    unsigned char expected[4160];
    memcpy(expected, plain, sizeof(plain));
    memset(expected + sizeof(plain), 0, 16);
    memcpy(expected + sizeof(plain) + 16, nonce, 48);
    if (!error) {
        file = gzopen(path, "rb");
        if (!file) error = 17;
        else {
            unsigned char actual[sizeof(expected)];
            if (gzseek(file, 123, SEEK_SET) != 123 || gzread(file, actual, 64) != 64 ||
                memcmp(actual, expected + 123, 64) || gzrewind(file) != 0 ||
                gzread(file, actual, sizeof(actual)) != (int)sizeof(actual) ||
                memcmp(actual, expected, sizeof(actual)) || gzread(file, actual, 1) != 0 || !gzeof(file)) error = 18;
            if (gzclose(file) != Z_OK) error = 19;
        }
    }
    if (!error) {
        unsigned char trailer[8];
        fd = open(path, O_RDONLY);
        if (fd < 0) error = 20;
        else {
            if (lseek(fd, -8, SEEK_END) < 0 || read(fd, trailer, sizeof(trailer)) != (ssize_t)sizeof(trailer) ||
                little32(trailer) != crc32(0, expected, sizeof(expected)) ||
                little32(trailer + 4) != sizeof(expected)) error = 21;
            if (close(fd)) error = 22;
        }
    }
    if (unlink(path)) error = 23;
    return error;
}

struct xml_state { unsigned starts, ends, namespaces, entities; int error; char text[64]; size_t used; };

static void XMLCALL xml_start(void *opaque, const XML_Char *name, const XML_Char **attributes)
{
    struct xml_state *state = opaque;
    if (state->starts == 0) {
        if (strcmp(name, "root") || attributes[0]) state->error = 1;
    } else if (state->starts == 1) {
        if (strcmp(name, "urn:native|item") || !attributes[0] || !attributes[1] ||
            strcmp(attributes[0], "urn:native|id") || strcmp(attributes[1], "7") || attributes[2]) state->error = 2;
    } else state->error = 3;
    state->starts++;
}

static void XMLCALL xml_end(void *opaque, const XML_Char *name)
{
    struct xml_state *state = opaque;
    if (strcmp(name, state->ends ? "root" : "urn:native|item")) state->error = 4;
    state->ends++;
}

static void XMLCALL xml_text(void *opaque, const XML_Char *text, int length)
{
    struct xml_state *state = opaque;
    if (length < 0 || (size_t)length >= sizeof(state->text) - state->used) { state->error = 5; return; }
    memcpy(state->text + state->used, text, (size_t)length);
    state->used += (size_t)length;
    state->text[state->used] = 0;
}

static void XMLCALL xml_namespace(void *opaque, const XML_Char *prefix, const XML_Char *uri)
{
    struct xml_state *state = opaque;
    if (!prefix || !uri || strcmp(prefix, "p") || strcmp(uri, "urn:native")) state->error = 6;
    state->namespaces++;
}

static void XMLCALL xml_entity(void *opaque, const XML_Char *name, int parameter,
    const XML_Char *value, int length, const XML_Char *base, const XML_Char *system,
    const XML_Char *public_id, const XML_Char *notation)
{
    (void)base;
    struct xml_state *state = opaque;
    if (strcmp(name, "text") || parameter || !value || length <= 0 || system || public_id || notation) state->error = 7;
    state->entities++;
}

static int xml(void)
{
    XML_Expat_Version version = XML_ExpatVersionInfo();
    if (version.major != XML_MAJOR_VERSION || version.minor != XML_MINOR_VERSION ||
        version.micro != XML_MICRO_VERSION) return 1;
    static const char document[] = "<!DOCTYPE root [<!ENTITY text 'native &amp; Darwin'>]>"
        "<root xmlns:p='urn:native'><p:item p:id='7'>&text; &#x3bb;</p:item></root>";
    XML_Parser parser = XML_ParserCreateNS(NULL, '|');
    if (!parser) return 2;
    struct xml_state state = {0};
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, xml_start, xml_end);
    XML_SetCharacterDataHandler(parser, xml_text);
    XML_SetStartNamespaceDeclHandler(parser, xml_namespace);
    XML_SetEntityDeclHandler(parser, xml_entity);
    for (size_t at = 0; at < sizeof(document) - 1;) {
        size_t count = sizeof(document) - 1 - at;
        if (count > 7) count = 7;
        if (XML_Parse(parser, document + at, (int)count, at + count == sizeof(document) - 1) != XML_STATUS_OK) {
            XML_ParserFree(parser); return 3;
        }
        at += count;
    }
    int error = state.error || state.starts != 2 || state.ends != 2 || state.namespaces != 1 ||
                state.entities != 1 || strcmp(state.text, "native & Darwin \xce\xbb");
    XML_ParserFree(parser);
    if (error) return 4;
    parser = XML_ParserCreate(NULL);
    if (!parser) return 5;
    const char malformed[] = "<root><item></root>";
    error = XML_Parse(parser, malformed, sizeof(malformed) - 1, XML_TRUE) != XML_STATUS_ERROR ||
            XML_GetErrorCode(parser) != XML_ERROR_TAG_MISMATCH;
    XML_ParserFree(parser);
    if (error) return 6;
    parser = XML_ParserCreate(NULL);
    if (!parser) return 7;
    error = XML_Parse(parser, "<root>", 6, XML_TRUE) != XML_STATUS_ERROR ||
            XML_GetErrorCode(parser) == XML_ERROR_NONE;
    XML_ParserFree(parser);
    return error ? 8 : 0;
}

static int png_roundtrip(void)
{
    if (png_access_version_number() != PNG_LIBPNG_VER) return 1;
    const unsigned char pixels[24] = {255,0,0,255, 0,255,0,128, 0,0,255,255,
                                     17,33,49,255, 127,63,31,64, 255,255,255,255};
    png_image output = {0};
    output.version = PNG_IMAGE_VERSION;
    output.width = 3;
    output.height = 2;
    output.format = PNG_FORMAT_RGBA;
    png_alloc_size_t length = 0;
    if (!png_image_write_to_memory(&output, NULL, &length, 0, pixels, 0, NULL) || length < 33 || length > 65536) return 2;
    unsigned char *encoded = malloc(length);
    if (!encoded) return 3;
    if (!png_image_write_to_memory(&output, encoded, &length, 0, pixels, 0, NULL)) {
        png_image_free(&output); free(encoded); return 4;
    }
    png_image_free(&output);
    png_image input = {0};
    input.version = PNG_IMAGE_VERSION;
    unsigned char decoded[sizeof(pixels)];
    int error = 0;
    if (!png_image_begin_read_from_memory(&input, encoded, length)) error = 5;
    else {
        input.format = PNG_FORMAT_RGBA;
        if (input.width != 3 || input.height != 2 || PNG_IMAGE_SIZE(input) != sizeof(pixels) ||
            !png_image_finish_read(&input, NULL, decoded, 0, NULL) || memcmp(decoded, pixels, sizeof(pixels))) error = 6;
    }
    png_image_free(&input);
    if (!error) {
        memset(&input, 0, sizeof(input)); input.version = PNG_IMAGE_VERSION;
        encoded[29] ^= 1; /* Corrupt the IHDR CRC; critical chunk errors must fail. */
        if (png_image_begin_read_from_memory(&input, encoded, length)) error = 7;
        encoded[29] ^= 1;
        png_image_free(&input);
    }
    if (!error) {
        memset(&input, 0, sizeof(input)); input.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&input, encoded, 24)) error = 8;
        png_image_free(&input);
    }
    free(encoded);
    return error;
}

static const unsigned char bdf[] =
    "STARTFONT 2.1\nFONT -native-test-medium-r-normal--8-80-75-75-c-80-iso10646-1\n"
    "SIZE 8 75 75\nFONTBOUNDINGBOX 8 8 0 -1\nSTARTPROPERTIES 2\nFONT_ASCENT 7\n"
    "FONT_DESCENT 1\nENDPROPERTIES\nCHARS 1\nSTARTCHAR A\nENCODING 65\n"
    "SWIDTH 500 0\nDWIDTH 8 0\nBBX 8 8 0 -1\nBITMAP\n18\n3C\n66\n66\n7E\n66\n66\n00\nENDCHAR\nENDFONT\n";

static int fonts(void)
{
    FT_Library library;
    if (FT_Init_FreeType(&library)) return 1;
    FT_Int major, minor, patch;
    FT_Library_Version(library, &major, &minor, &patch);
    int error = major != FREETYPE_MAJOR || minor != FREETYPE_MINOR || patch != FREETYPE_PATCH ? 2 : 0;
    FT_Face face = NULL;
    if (!error && FT_New_Memory_Face(library, bdf, sizeof(bdf) - 1, 0, &face)) error = 3;
    if (!error) {
        const unsigned char expected[] = {0x18,0x3c,0x66,0x66,0x7e,0x66,0x66,0};
        if (FT_IS_SCALABLE(face) || face->num_fixed_sizes != 1 || FT_Select_Size(face, 0) ||
            !FT_Get_Char_Index(face, 'A') || FT_Load_Char(face, 'A', FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)) error = 4;
        else if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP || face->glyph->bitmap.width != 8 ||
                 face->glyph->bitmap.rows != 8 || face->glyph->bitmap.pitch != 1 ||
                 face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_MONO || face->glyph->advance.x != 8 * 64 ||
                 face->glyph->bitmap_top != 7 || memcmp(face->glyph->bitmap.buffer, expected, sizeof(expected))) error = 5;
    }
    if (face) { FT_Done_Face(face); face = NULL; }
    if (!error && FT_New_Face(library, "/usr/share/fonts/native-varc-static-gvar.ttf", 0, &face)) error = 6;
    if (!error) {
        /* Exact expectations from upstream tests/varc/main.c, followed by rasterization. */
        if (face->num_glyphs != 3 || !FT_IS_SCALABLE(face) ||
            FT_Load_Glyph(face, 1, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) ||
            face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points != 3 ||
            face->glyph->outline.points[0].x != 50) error = 7;
        else if (FT_Set_Pixel_Sizes(face, 0, 32) ||
                 FT_Load_Glyph(face, 1, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
                 FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) error = 8;
        else {
            FT_Bitmap *bitmap = &face->glyph->bitmap;
            unsigned coverage = 0;
            if (!bitmap->width || !bitmap->rows || bitmap->pitch <= 0 || bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) error = 9;
            else {
                for (unsigned row = 0; row < bitmap->rows; ++row)
                    for (unsigned column = 0; column < bitmap->width; ++column)
                        coverage += bitmap->buffer[(size_t)row * (unsigned)bitmap->pitch + column];
                if (!coverage) error = 10;
            }
        }
    }
    if (face) { FT_Done_Face(face); face = NULL; }
    if (!error && !FT_New_Memory_Face(library, (const FT_Byte *)"not a font", 10, 0, &face)) {
        FT_Done_Face(face); error = 11;
    }
    if (FT_Done_FreeType(library) && !error) error = 12;
    return error;
}

static int pixels(unsigned seed)
{
    uint32_t source[16], destination[16] = {0};
    for (unsigned i = 0; i < 16; ++i) source[i] = 0xff000000U | ((seed * 19 + i * 0x030507U) & 0xffffffU);
    pixman_image_t *src = pixman_image_create_bits(PIXMAN_a8r8g8b8, 4, 4, source, 16);
    pixman_image_t *dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, 4, 4, destination, 16);
    if (!src || !dst) { if (src) pixman_image_unref(src); if (dst) pixman_image_unref(dst); return 1; }
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 1, 1, 2, 2);
    int error = !pixman_image_set_clip_region32(dst, &clip) ? 2 : 0;
    pixman_region32_fini(&clip);
    if (!error) {
        pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst, 0,0,0,0,0,0,4,4);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
                if (destination[y * 4 + x] != ((x == 1 || x == 2) && (y == 1 || y == 2) ? source[y * 4 + x] : 0)) error = 3;
    }
    pixman_transform_t transform;
    pixman_transform_init_translate(&transform, pixman_int_to_fixed(1), 0);
    if (!pixman_image_set_clip_region32(dst, NULL) || !pixman_image_set_transform(src, &transform) ||
        !pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0)) error = 4;
    if (!error) {
        memset(destination, 0, sizeof(destination));
        pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst, 0,0,0,0,0,0,4,4);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
                if (destination[y * 4 + x] != (x < 3 ? source[y * 4 + x + 1] : 0)) error = 5;
    }
    pixman_image_unref(src);
    uint32_t red = 0xffff0000U, mask_pixel = 0x80808080U;
    src = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, &red, 4);
    pixman_image_t *mask = pixman_image_create_bits(PIXMAN_a8, 1, 1, &mask_pixel, 4);
    if (!src || !mask) error = 6;
    if (!error) {
        pixman_image_set_repeat(src, PIXMAN_REPEAT_NORMAL);
        pixman_image_set_repeat(mask, PIXMAN_REPEAT_NORMAL);
        for (unsigned i = 0; i < 16; ++i) destination[i] = 0xff0000ffU;
        pixman_image_composite32(PIXMAN_OP_OVER, src, mask, dst, 0,0,0,0,0,0,4,4);
        for (unsigned i = 0; i < 16; ++i) if (destination[i] != 0xff80007fU) error = 7;
    }
    if (src) pixman_image_unref(src);
    if (mask) pixman_image_unref(mask);
    pixman_image_unref(dst);
    return error;
}

static _Thread_local unsigned thread_tag;
static atomic_uint workers_ready, workers_go;
struct pixel_worker { unsigned index; int result; };

static void *pixel_thread(void *opaque)
{
    struct pixel_worker *worker = opaque;
    thread_tag = 0xabc000 + worker->index;
    atomic_fetch_add(&workers_ready, 1);
    while (!atomic_load(&workers_go)) sched_yield();
    for (unsigned round = 0; round < 128; ++round) {
        int error = pixels(worker->index * 128 + round);
        if (error || thread_tag != 0xabc000 + worker->index) {
            worker->result = error ? error : 8;
            break;
        }
    }
    return NULL;
}

static int pixman(void)
{
    if (pixman_version() != PIXMAN_VERSION) return 1;
    int error = pixels(1);
    if (error) return 10 + error;
    thread_tag = 0xdecafbad;
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        pthread_t threads[4];
        struct pixel_worker workers[4];
        atomic_store(&workers_ready, 0);
        atomic_store(&workers_go, 0);
        unsigned created = 0;
        for (; created < 4; ++created) {
            workers[created] = (struct pixel_worker){created + cycle * 4, 0};
            if (pthread_create(&threads[created], NULL, pixel_thread, &workers[created])) break;
        }
        if (created == 4) while (atomic_load(&workers_ready) != 4) sched_yield();
        atomic_store(&workers_go, 1);
        error = created != 4 ? 20 : 0;
        for (unsigned i = 0; i < created; ++i) {
            if (pthread_join(threads[i], NULL)) error = 21;
            else if (workers[i].result) error = 30 + workers[i].result;
        }
        if (error) return error;
        if (thread_tag != 0xdecafbad) return 40;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 48 || strspn(argv[1], "0123456789abcdef") != 48) return failure("nonce", 1);
    int error = compression(argv[1]);
    if (error) return failure("zlib", error);
    error = xml();
    if (error) return failure("Expat", error);
    error = png_roundtrip();
    if (error) return failure("libpng", error);
    error = fonts();
    if (error) return failure("FreeType", error);
    error = pixman();
    if (error) return failure("Pixman", error);
    struct utsname identity;
    if (uname(&identity) || !identity.release[0] || !identity.machine[0]) return failure("uname", 1);
    printf("NATIVE_GRAPHICS_OK:%s:%s:%s\n", argv[1], identity.release, identity.machine);
    return 0;
}
