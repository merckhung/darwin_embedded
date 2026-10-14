/* Exercise actual native Pango layout and its FreeType/Cairo backend. */
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <fontconfig/fontconfig.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

int native_thai_vectors(void);

static int fail(const char *stage, int code)
{
    printf("Native pango failure: %s:%d\n", stage, code);
    fflush(stdout);
    return 1;
}

static int layout_checks(PangoLayout *layout)
{
    const char *text = "Native abc العربية DEF text layout wrapping words";
    pango_layout_set_markup(layout,
        "<span foreground=\"#e02010\" weight=\"bold\">Native</span> abc العربية DEF text layout wrapping words", -1);
    if (strcmp(pango_layout_get_text(layout), text) || !pango_layout_get_attributes(layout)) return 1;
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    if (width <= 100 || height <= 0) return 2;
    unsigned rtl = 0, ltr = 0, glyphs = 0;
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    do {
        PangoLayoutRun *run = pango_layout_iter_get_run_readonly(iter);
        if (run) {
            if (!run->item->analysis.font || run->glyphs->num_glyphs <= 0) return 3;
            glyphs += run->glyphs->num_glyphs;
            if (run->item->analysis.level & 1) ++rtl;
            else ++ltr;
        }
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
    if (!rtl || !ltr || glyphs < 20) return 4;
    pango_layout_set_width(layout, 110 * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    if (!pango_layout_is_wrapped(layout) || pango_layout_get_line_count(layout) < 2) return 5;
    pango_layout_set_height(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    if (!pango_layout_is_ellipsized(layout) || pango_layout_get_line_count(layout) != 1) return 6;
    return 0;
}

static int serialization(PangoLayout *layout)
{
    GBytes *bytes = pango_layout_serialize(layout, PANGO_LAYOUT_SERIALIZE_CONTEXT | PANGO_LAYOUT_SERIALIZE_OUTPUT);
    if (!bytes || g_bytes_get_size(bytes) < 100) return 1;
    PangoContext *context = pango_font_map_create_context(pango_context_get_font_map(pango_layout_get_context(layout)));
    GError *error = NULL;
    PangoLayout *copy = pango_layout_deserialize(context, bytes, PANGO_LAYOUT_DESERIALIZE_CONTEXT, &error);
    g_bytes_unref(bytes);
    if (!copy || error || strcmp(pango_layout_get_text(copy), pango_layout_get_text(layout)) ||
        pango_layout_get_width(copy) != pango_layout_get_width(layout) ||
        pango_layout_get_ellipsize(copy) != PANGO_ELLIPSIZE_END || !pango_layout_get_attributes(copy)) return 2;
    int w1, h1, w2, h2;
    pango_layout_get_pixel_size(layout, &w1, &h1);
    pango_layout_get_pixel_size(copy, &w2, &h2);
    if (w1 != w2 || h1 != h2) return 3;
    g_object_unref(copy);
    const char invalid[] = "{\"text\":123}";
    bytes = g_bytes_new_static(invalid, sizeof(invalid) - 1);
    copy = pango_layout_deserialize(context, bytes, PANGO_LAYOUT_DESERIALIZE_DEFAULT, &error);
    g_bytes_unref(bytes);
    /* The upstream parser maps a JSON type mismatch to INVALID. */
    if (copy || !g_error_matches(error, PANGO_LAYOUT_DESERIALIZE_ERROR, PANGO_LAYOUT_DESERIALIZE_INVALID)) return 4;
    g_clear_error(&error);
    const char bad_enum[] = "{\"text\":\"abc\",\"ellipsize\":\"invalid\"}";
    bytes = g_bytes_new_static(bad_enum, sizeof(bad_enum) - 1);
    copy = pango_layout_deserialize(context, bytes, PANGO_LAYOUT_DESERIALIZE_DEFAULT, &error);
    g_bytes_unref(bytes);
    g_object_unref(context);
    if (copy || !g_error_matches(error, PANGO_LAYOUT_DESERIALIZE_ERROR, PANGO_LAYOUT_DESERIALIZE_INVALID_VALUE)) return 5;
    g_error_free(error);
    return 0;
}

static int boundaries(PangoLayout *layout)
{
    pango_layout_set_attributes(layout, NULL);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(layout, "a\xcc\x81" "b", -1);
    int count;
    const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout, &count);
    if (count != 4 || !attrs[0].is_cursor_position || attrs[1].is_cursor_position ||
        !attrs[2].is_cursor_position || !attrs[3].is_cursor_position) return 1;
    int index, trailing;
    pango_layout_move_cursor_visually(layout, TRUE, 0, 0, 1, &index, &trailing);
    if (index < 0 || index > 3 || g_utf8_strlen(pango_layout_get_text(layout), index) + trailing != 2) return 2;
    PangoRectangle start, end;
    pango_layout_get_cursor_pos(layout, 0, &start, NULL);
    pango_layout_get_cursor_pos(layout, 3, &end, NULL);
    if (end.x <= start.x || start.height <= 0) return 3;
    /* Two dictionary words, without whitespace, from the original Thai break fixture. */
    pango_context_set_language(pango_layout_get_context(layout), pango_language_from_string("th"));
    pango_layout_set_text(layout, "ภาษาไทย", -1);
    attrs = pango_layout_get_log_attrs_readonly(layout, &count);
    if (count != 8 || !attrs[4].is_line_break || !attrs[4].is_word_start || !attrs[4].is_word_end) return 4;
    return 0;
}

static int drawing(PangoLayout *layout)
{
    pango_layout_set_text(layout, "Native Xfce4", -1);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 80);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, 12, 12);
    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout(cr, layout);
    cairo_surface_flush(surface);
    if (cairo_status(cr) || cairo_surface_status(surface) || pango_layout_get_unknown_glyphs_count(layout)) return 1;
    unsigned changed = 0;
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < 80; ++y) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < 320; ++x) {
            if ((row[x] >> 24) != 255) return 2;
            if (row[x] != UINT32_C(0xffffffff)) {
                if (x < 12 || y < 12) return 3;
                ++changed;
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return changed > 100 ? 0 : 4;
}

int main(int argc, char **argv)
{
    struct utsname platform;
    if (argc != 2 || strlen(argv[1]) != 48 || uname(&platform) || getpid() <= 1) return fail("arguments", 1);
    const char *nonce = argv[1];
    printf("NATIVE_PANGO_PID:%s:%d\n", nonce, getpid()); fflush(stdout);
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config || !FcConfigSetCurrent(config)) return fail("fonts", 1);
    PangoFontMap *map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    if (!map || !PANGO_IS_FC_FONT_MAP(map)) return fail("FreeType map", 1);
    pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(map), config);
    PangoContext *context = pango_font_map_create_context(map);
    PangoLayout *layout = pango_layout_new(context);
    PangoFontDescription *font = pango_font_description_from_string("Roboto 18");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int error;
    if ((error = layout_checks(layout))) return fail("bidi/wrap/ellipsis", error);
    printf("NATIVE_PANGO_STAGE:%s:bidi-wrap-ellipsis\n", nonce); fflush(stdout);
    if ((error = serialization(layout))) return fail("serialization", error);
    printf("NATIVE_PANGO_STAGE:%s:serialization-errors\n", nonce); fflush(stdout);
    if ((error = native_thai_vectors())) return fail("original Thai vectors", error);
    if ((error = boundaries(layout))) return fail("cursor/Thai breaks", error);
    printf("NATIVE_PANGO_STAGE:%s:cursor-thai-dictionary\n", nonce); fflush(stdout);
    if ((error = drawing(layout))) return fail("Cairo glyph pixels", error);
    printf("NATIVE_PANGO_STAGE:%s:cairo-glyph-pixels\n", nonce); fflush(stdout);
    g_object_unref(layout); g_object_unref(context); g_object_unref(map); FcConfigDestroy(config);
    printf("NATIVE_PANGO_OK:%s:%s:%s\n", nonce, platform.release, platform.machine); fflush(stdout);
    return 0;
}
