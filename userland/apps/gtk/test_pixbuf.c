/* Native image behavior; PNG/raw pixel and GIF fixtures are original upstream bytes. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixdata.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define FIXTURES "/usr/share/native-pixbuf/"

static int fail(const char *stage, int code)
{
    printf("Native pixbuf failure: %s:%d\n", stage, code);
    fflush(stdout);
    return 1;
}

static int equal(GdkPixbuf *a, GdkPixbuf *b)
{
    if (!a || !b || gdk_pixbuf_get_width(a) != gdk_pixbuf_get_width(b) ||
        gdk_pixbuf_get_height(a) != gdk_pixbuf_get_height(b) ||
        gdk_pixbuf_get_n_channels(a) != gdk_pixbuf_get_n_channels(b)) return 0;
    int bytes = gdk_pixbuf_get_width(a) * gdk_pixbuf_get_n_channels(a);
    for (int y = 0; y < gdk_pixbuf_get_height(a); ++y)
        if (memcmp(gdk_pixbuf_read_pixels(a) + y * gdk_pixbuf_get_rowstride(a),
                   gdk_pixbuf_read_pixels(b) + y * gdk_pixbuf_get_rowstride(b), bytes)) return 0;
    return 1;
}

static int original_pixels(GdkPixbuf **out)
{
    GError *error = NULL;
    GdkPixbuf *png = gdk_pixbuf_new_from_file(FIXTURES "test-image.png", &error);
    if (!png || error) return 1;
    const char *names[] = {FIXTURES "test-image.pixdata", FIXTURES "test-image-rle.pixdata"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i) {
        char *data;
        gsize size;
        GdkPixdata pixels;
        if (!g_file_get_contents(names[i], &data, &size, &error) || error ||
            !gdk_pixdata_deserialize(&pixels, size, (const guint8 *)data, &error)) return 2;
        GdkPixbuf *raw = gdk_pixbuf_from_pixdata(&pixels, TRUE, &error);
        if (!raw || error || !equal(png, raw)) return 3;
        g_object_unref(raw);
        g_free(data);
    }
    *out = png;
    return 0;
}

static unsigned prepared, updated;
static void size_prepared(GdkPixbufLoader *loader, int width, int height, void *user)
{
    (void)loader; (void)user;
    if (width > 0 && height > 0) ++prepared;
}
static void area_updated(GdkPixbufLoader *loader, int x, int y, int width, int height, void *user)
{
    (void)loader; (void)user;
    if (x >= 0 && y >= 0 && width > 0 && height > 0) ++updated;
}

static int streaming(GdkPixbuf *reference)
{
    char *data;
    gsize size;
    GError *error = NULL;
    if (!g_file_get_contents(FIXTURES "test-image.png", &data, &size, &error)) return 1;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("png", &error);
    if (!loader || error) return 2;
    g_signal_connect(loader, "size-prepared", G_CALLBACK(size_prepared), NULL);
    g_signal_connect(loader, "area-updated", G_CALLBACK(area_updated), NULL);
    for (gsize offset = 0; offset < size; offset += 13)
        if (!gdk_pixbuf_loader_write(loader, (guint8 *)data + offset, MIN((gsize)13, size - offset), &error)) return 3;
    if (!gdk_pixbuf_loader_close(loader, &error) || error || prepared != 1 || !updated) return 4;
    GdkPixbuf *image = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!equal(image, reference)) return 5;
    g_object_ref(image); g_object_unref(loader);
    if (!equal(image, reference)) return 6;
    g_object_unref(image); g_free(data);
    loader = gdk_pixbuf_loader_new_with_type("png", &error);
    const guint8 invalid[] = "not a PNG image";
    gboolean accepted = gdk_pixbuf_loader_write(loader, invalid, sizeof(invalid), &error);
    if (accepted || !g_error_matches(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE)) return 7;
    g_clear_error(&error);
    gdk_pixbuf_loader_close(loader, NULL);
    g_object_unref(loader);
    loader = gdk_pixbuf_loader_new_with_type("native-unsupported", &error);
    if (loader || !g_error_matches(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE)) return 8;
    g_error_free(error);
    return 0;
}

static int saving(GdkPixbuf *reference, const char *nonce)
{
    GError *error = NULL;
    char *data;
    gsize size;
    if (!gdk_pixbuf_save_to_buffer(reference, &data, &size, "png", &error, "tEXt::Title", nonce, NULL) || error) return 1;
    GInputStream *stream = g_memory_input_stream_new_from_data(data, size, g_free);
    GdkPixbuf *copy = gdk_pixbuf_new_from_stream(stream, NULL, &error);
    const char *title = copy ? gdk_pixbuf_get_option(copy, "tEXt::Title") : NULL;
    if (!equal(reference, copy) || error || !title || strcmp(title, nonce)) return 2;
    g_object_unref(copy); g_object_unref(stream);
    char path[128];
    snprintf(path, sizeof(path), "/var/pixbuf-%s.png", nonce);
    if (!gdk_pixbuf_save(reference, path, "png", &error, NULL) || error) return 3;
    copy = gdk_pixbuf_new_from_file(path, &error);
    if (!equal(reference, copy) || error || unlink(path)) return 4;
    g_object_unref(copy);
    return 0;
}

static int operations(void)
{
    GdkPixbuf *source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 3, 2);
    gdk_pixbuf_fill(source, UINT32_C(0xe02010ff));
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, 6, 4, GDK_INTERP_BILINEAR);
    GdkPixbuf *rotated = gdk_pixbuf_rotate_simple(source, GDK_PIXBUF_ROTATE_CLOCKWISE);
    if (!scaled || !rotated || gdk_pixbuf_get_width(rotated) != 2 || gdk_pixbuf_get_height(rotated) != 3) return 1;
    const guint8 expected[] = {224, 32, 16, 255};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            if (memcmp(gdk_pixbuf_read_pixels(scaled) + y * gdk_pixbuf_get_rowstride(scaled) + 4 * x, expected, 4)) return 2;
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(source, 1, 0, 1, 1);
    gdk_pixbuf_fill(sub, UINT32_C(0x102030ff));
    const guint8 changed[] = {16, 32, 48, 255};
    if (memcmp(gdk_pixbuf_read_pixels(source) + 4, changed, 4)) return 3;
    GdkPixbuf *owned = gdk_pixbuf_copy(sub);
    g_object_unref(source); g_object_unref(sub);
    if (!owned || memcmp(gdk_pixbuf_read_pixels(owned), changed, 4)) return 4;
    g_object_unref(owned); g_object_unref(rotated); g_object_unref(scaled);
    return 0;
}

static int animation(void)
{
    GError *error = NULL;
    /* Use an enabled original suite case; multi-image cases are disabled upstream. */
    GdkPixbufAnimation *animation = gdk_pixbuf_animation_new_from_file(FIXTURES "dispose-keep.gif", &error);
    if (!animation || error || gdk_pixbuf_animation_is_static_image(animation) ||
        gdk_pixbuf_animation_get_width(animation) != 2 || gdk_pixbuf_animation_get_height(animation) != 2) return 1;
    GTimeVal time = {100, 0};
    GdkPixbufAnimationIter *iter = gdk_pixbuf_animation_get_iter(animation, &time);
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (frame) {
            /* Upstream fixture delay 50 is in GIF centiseconds: 500 ms. */
            time.tv_sec = 100 + frame / 2;
            time.tv_usec = 500000 * (frame % 2);
            if (!gdk_pixbuf_animation_iter_advance(iter, &time)) return 2;
        }
        char path[128];
        snprintf(path, sizeof(path), FIXTURES "animation-fill.%u.rgba", frame);
        char *expected;
        gsize size;
        if (!g_file_get_contents(path, &expected, &size, &error) || error || size != 16) return 3;
        GdkPixbuf *pixbuf = gdk_pixbuf_animation_iter_get_pixbuf(iter);
        if (!pixbuf || gdk_pixbuf_get_n_channels(pixbuf) != 4 || gdk_pixbuf_animation_iter_get_delay_time(iter) != 500) return 4;
        for (int y = 0; y < 2; ++y) {
            const guint8 *actual = gdk_pixbuf_read_pixels(pixbuf) + y * gdk_pixbuf_get_rowstride(pixbuf);
            if (memcmp(expected + 8 * y, actual, 8)) {
                printf("Native GIF mismatch: frame=%u row=%d stride=%d actual=", frame, y, gdk_pixbuf_get_rowstride(pixbuf));
                for (int i = 0; i < 8; ++i) printf("%02x", actual[i]);
                printf(" expected=");
                for (int i = 0; i < 8; ++i) printf("%02x", (guint8)expected[8 * y + i]);
                printf("\n");
                return 5;
            }
        }
        g_free(expected);
    }
    time.tv_sec = 102;
    time.tv_usec = 0;
    if (!gdk_pixbuf_animation_iter_advance(iter, &time)) return 6;
    g_object_unref(iter); g_object_unref(animation);
    return 0;
}

int main(int argc, char **argv)
{
    struct utsname platform;
    if (argc != 2 || strlen(argv[1]) != 48 || uname(&platform) || getpid() <= 1) return fail("arguments", 1);
    const char *nonce = argv[1];
    printf("NATIVE_PIXBUF_PID:%s:%d\n", nonce, getpid()); fflush(stdout);
    int error;
    GdkPixbuf *reference = NULL;
    if ((error = original_pixels(&reference))) return fail("original PNG/raw/RLE pixels", error);
    printf("NATIVE_PIXBUF_STAGE:%s:original-png-pixels\n", nonce); fflush(stdout);
    if ((error = streaming(reference))) return fail("incremental/signals/errors", error);
    printf("NATIVE_PIXBUF_STAGE:%s:incremental-signals-errors\n", nonce); fflush(stdout);
    if ((error = saving(reference, nonce))) return fail("PNG buffers/files/options", error);
    printf("NATIVE_PIXBUF_STAGE:%s:png-buffer-file-options\n", nonce); fflush(stdout);
    if ((error = operations())) return fail("scale/rotate/ownership", error);
    printf("NATIVE_PIXBUF_STAGE:%s:scale-rotate-ownership\n", nonce); fflush(stdout);
    if ((error = animation())) return fail("original GIF frames/delays", error);
    printf("NATIVE_PIXBUF_STAGE:%s:original-gif-frames\n", nonce); fflush(stdout);
    g_object_unref(reference);
    printf("NATIVE_PIXBUF_OK:%s:%s:%s\n", nonce, platform.release, platform.machine); fflush(stdout);
    return 0;
}
