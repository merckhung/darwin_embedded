/* Native font/drawing prerequisites; all libraries are signed Mach-O.
 * Arabic expectations come unchanged from HarfBuzz's upstream fixture. */
#include <cairo.h>
#include <cairo-ft.h>
#include <cairo-gobject.h>
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <glib-object.h>
#include <hb.h>
#include <hb-glib.h>
#include <hb-ot.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

int native_udivti3_vectors(void);
int native_udivmodti4_vectors(void);

static const uint32_t arabic_text[] = {0x633,0x64f,0x644,0x64e,0x651,0x627,0x651,0x650,0x645,0x62a,0x6cc};
static const struct {
    const char *name;
    uint32_t cluster;
    int advance, x, y;
} arabic_expected[] = {
    {"uni06CC.fina",10,1655,0,0}, {"uni062A.medi",9,868,0,0}, {"uni0645.init",8,1098,0,0},
    {"uni0650",2,0,148,0}, {"uni0651",2,0,187,736}, {"uni064E",2,0,883,1259},
    {"uni0651",2,0,922,736}, {"uni06440627.fina",2,1470,0,0}, {"uni064F",0,0,629,-10},
    {"uni0633.init",0,1585,0,0},
};

static int fail(const char *stage, int code)
{
    fprintf(stderr,"Native fonts failure: %s check %d (errno=%d)\n",stage,code,errno);
    return 1;
}

static int discovery(FcConfig **output)
{
    if (FcGetVersion()!=FC_VERSION) return 1;
    FcConfig *config=FcInitLoadConfigAndFonts();
    if (!config || !FcConfigSetCurrent(config)) return 2;
    FcFontSet *fonts=FcConfigGetFonts(config,FcSetSystem);
    if (!fonts || fonts->nfont<2) return 3;
    FcPattern *request=FcPatternCreate();
    if (!request || !FcPatternAddString(request,FC_FAMILY,(const FcChar8 *)"Roboto")) return 4;
    if (!FcConfigSubstitute(config,request,FcMatchPattern)) return 5;
    FcDefaultSubstitute(request);
    FcResult result;
    FcPattern *match=FcFontMatch(config,request,&result);
    FcChar8 *file=NULL,*family=NULL;
    if (!match || FcPatternGetString(match,FC_FILE,0,&file)!=FcResultMatch ||
        strcmp((const char *)file,"/usr/share/fonts/native-latin.ttf") ||
        FcPatternGetString(match,FC_FAMILY,0,&family)!=FcResultMatch ||
        strcmp((const char *)family,"Roboto")) return 6;
    FcPatternDestroy(match); FcPatternDestroy(request);
    FcCache *cache=FcDirCacheRead((const FcChar8 *)"/usr/share/fonts",FcTrue,config);
    if (!cache || FcCacheNumFont(cache)<2) return 7;
    FcDirCacheUnload(cache);
    FcChar8 *cache_file=NULL;
    cache=FcDirCacheLoad((const FcChar8 *)"/usr/share/fonts",config,&cache_file);
    if (!cache || !cache_file || strncmp((const char *)cache_file,"/var/cache/fontconfig/",22)) return 8;
    struct stat status;
    if (stat((const char *)cache_file,&status) || !S_ISREG(status.st_mode) || status.st_size<1024 ||
        !FcDirCacheValid((const FcChar8 *)"/usr/share/fonts")) return 9;
    FcCache *from_file=FcDirCacheLoadFile(cache_file,&status);
    if (!from_file || FcCacheNumFont(from_file)!=FcCacheNumFont(cache)) return 10;
    FcDirCacheUnload(from_file); FcDirCacheUnload(cache); FcStrFree(cache_file);
    *output=config;
    return 0;
}

static int shape_once(hb_face_t *face)
{
    hb_font_t *font=hb_font_create(face);
    hb_buffer_t *buffer=hb_buffer_create();
    hb_ot_font_set_funcs(font);
    hb_buffer_set_unicode_funcs(buffer,hb_glib_get_unicode_funcs());
    hb_buffer_add_codepoints(buffer,arabic_text,sizeof(arabic_text)/sizeof(arabic_text[0]),0,-1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font,buffer,NULL,0);
    unsigned count=0;
    hb_glyph_info_t *info=hb_buffer_get_glyph_infos(buffer,&count);
    hb_glyph_position_t *positions=hb_buffer_get_glyph_positions(buffer,NULL);
    int error=0;
    if (count!=sizeof(arabic_expected)/sizeof(arabic_expected[0]) || hb_buffer_get_direction(buffer)!=HB_DIRECTION_RTL) error=1;
    for (unsigned i=0; !error && i<count; ++i) {
        char name[64];
        if (!hb_font_get_glyph_name(font,info[i].codepoint,name,sizeof(name)) || strcmp(name,arabic_expected[i].name) ||
            info[i].cluster!=arabic_expected[i].cluster || positions[i].x_advance!=arabic_expected[i].advance ||
            positions[i].y_advance || positions[i].x_offset!=arabic_expected[i].x || positions[i].y_offset!=arabic_expected[i].y) error=2;
    }
    hb_buffer_destroy(buffer); hb_font_destroy(font);
    return error;
}

struct shaping_thread { hb_face_t *face; int result; unsigned iterations; };
static void *shape_worker(void *data)
{
    struct shaping_thread *state=data;
    for (unsigned i=0; i<128; ++i) {
        state->result=shape_once(state->face);
        if (state->result) break;
        ++state->iterations;
    }
    return NULL;
}

static int shaping(void)
{
    if (strcmp(hb_version_string(),HB_VERSION_STRING)) return 1;
    hb_blob_t *blob=hb_blob_create_from_file_or_fail("/usr/share/fonts/native-arabic.ttf");
    if (!blob || hb_blob_get_length(blob)<1024) return 2;
    hb_face_t *face=hb_face_create(blob,0);
    hb_blob_destroy(blob);
    if (hb_face_get_glyph_count(face)<10 || shape_once(face)) return 3;
    pthread_t threads[4];
    struct shaping_thread state[4];
    for (unsigned i=0;i<4;++i) {
        state[i]=(struct shaping_thread){face,0,0};
        if (pthread_create(&threads[i],NULL,shape_worker,&state[i])) return 4;
    }
    for (unsigned i=0;i<4;++i)
        if (pthread_join(threads[i],NULL) || state[i].result || state[i].iterations!=128) return 5;
    hb_face_destroy(face);
    return 0;
}

static uint32_t pixel(cairo_surface_t *surface,unsigned x,unsigned y)
{
    uint32_t value;
    memcpy(&value,cairo_image_surface_get_data(surface)+y*cairo_image_surface_get_stride(surface)+x*4,4);
    return value;
}

static int drawing(const char *nonce,FcConfig *config)
{
    if (cairo_version()!=CAIRO_VERSION || strcmp(cairo_version_string(),CAIRO_VERSION_STRING)) return 1;
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,64,32);
    cairo_t *cr=cairo_create(surface);
    cairo_set_source_rgb(cr,1,1,1); cairo_paint(cr);
    cairo_save(cr); cairo_rectangle(cr,4,4,12,12); cairo_clip(cr);
    cairo_set_source_rgb(cr,1,0,0); cairo_paint(cr); cairo_restore(cr);
    cairo_set_source_rgb(cr,0,0,1); cairo_rectangle(cr,20,4,8,12); cairo_fill(cr);
    cairo_set_source_rgba(cr,0,0,1,0.5); cairo_rectangle(cr,6,6,4,4); cairo_fill(cr);
    cairo_surface_flush(surface);
    if (pixel(surface,0,0)!=0xffffffff || pixel(surface,5,5)!=0xffff0000 ||
        pixel(surface,7,7)!=0xff7f0080 || pixel(surface,24,8)!=0xff0000ff) return 2;
    cairo_pattern_t *gradient=cairo_pattern_create_linear(0,0,64,0);
    cairo_pattern_add_color_stop_rgb(gradient,0,0,0,0); cairo_pattern_add_color_stop_rgb(gradient,1,1,1,1);
    cairo_set_source(cr,gradient); cairo_rectangle(cr,0,24,64,8); cairo_fill(cr); cairo_pattern_destroy(gradient);
    cairo_surface_flush(surface);
    unsigned center=pixel(surface,32,28),r=(center>>16)&255,g=(center>>8)&255,b=center&255;
    if (r<100 || r>160 || r!=g || r!=b || (center>>24)!=255) return 3;
    char path[128];
    snprintf(path,sizeof(path),"/tmp/native-fonts-%s.png",nonce);
    if (cairo_surface_write_to_png(surface,path)!=CAIRO_STATUS_SUCCESS) return 4;
    cairo_surface_t *decoded=cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(decoded)!=CAIRO_STATUS_SUCCESS || cairo_image_surface_get_width(decoded)!=64 ||
        cairo_image_surface_get_height(decoded)!=32 || pixel(decoded,7,7)!=0xff7f0080 || pixel(decoded,32,28)!=center) return 5;
    cairo_surface_destroy(decoded);
    if (unlink(path)) return 6;
    FcPattern *request=FcPatternCreate(); FcPatternAddString(request,FC_FAMILY,(const FcChar8 *)"Roboto");
    FcConfigSubstitute(config,request,FcMatchPattern); FcDefaultSubstitute(request);
    FcResult result;
    FcPattern *match=FcFontMatch(config,request,&result);
    cairo_font_face_t *font_face=cairo_ft_font_face_create_for_pattern(match);
    cairo_set_font_face(cr,font_face); cairo_set_font_size(cr,14);
    cairo_text_extents_t extents;
    cairo_text_extents(cr,"Native Darwin",&extents);
    if (cairo_status(cr)!=CAIRO_STATUS_SUCCESS || extents.width<20 || extents.height<5 || extents.x_advance<20) return 7;
    cairo_set_source_rgb(cr,0,0,0); cairo_move_to(cr,0,22); cairo_show_text(cr,"Darwin");
    cairo_surface_flush(surface);
    unsigned changed=0;
    for (unsigned y=16;y<24;++y) for (unsigned x=0;x<48;++x) changed+=pixel(surface,x,y)!=0xffffffff;
    if (!changed || cairo_status(cr)!=CAIRO_STATUS_SUCCESS) return 8;
    cairo_font_face_destroy(font_face); FcPatternDestroy(match); FcPatternDestroy(request);
    cairo_destroy(cr); cairo_surface_destroy(surface);
    return 0;
}

static int objects(void)
{
    GEnumClass *type=g_type_class_ref(cairo_gobject_status_get_type());
    GEnumValue *entry=g_enum_get_value(type,CAIRO_STATUS_INVALID_SIZE);
    if (!entry || strcmp(entry->value_nick,"invalid-size")) return 1;
    g_type_class_unref(type);
    cairo_matrix_t matrix; cairo_matrix_init_translate(&matrix,17,23);
    GValue value=G_VALUE_INIT;
    g_value_init(&value,cairo_gobject_matrix_get_type()); g_value_set_boxed(&value,&matrix);
    cairo_matrix_t *copy=g_value_get_boxed(&value);
    matrix.x0=99;
    if (!copy || copy==&matrix || copy->x0!=17 || copy->y0!=23) return 2;
    g_value_unset(&value);
    return 0;
}

static int shared_memory(const char *nonce)
{
    int id=shmget(IPC_PRIVATE,65536,IPC_CREAT|0600);
    if (id<0) return 1;
    char *mapping=shmat(id,NULL,0);
    if (mapping==(void *)-1) {shmctl(id,IPC_RMID,NULL);return 2;}
    memcpy(mapping,nonce,48);
    pid_t child=fork();
    if (child<0) return 3;
    if (!child) {
        char *second=shmat(id,NULL,0);
        if (second==(void *)-1 || memcmp(second,nonce,48)) _exit(1);
        memcpy(second+64,nonce,48);
        if (shmdt(second) || shmdt(mapping)) _exit(2);
        _exit(0);
    }
    int status;
    if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status) || memcmp(mapping+64,nonce,48)) return 4;
    struct shmid_ds information;
    if (shmctl(id,IPC_STAT,&information) || information.shm_segsz!=65536 || information.shm_nattch!=1) return 5;
    if (shmctl(id,IPC_RMID,NULL)) return 6;
    errno=0;
    if (shmat(id,NULL,0)!=(void *)-1 || errno!=EINVAL || memcmp(mapping,nonce,48)) return 7;
    if (shmdt(mapping)) return 8;
    errno=0;
    if (shmctl(id,IPC_STAT,&information)!=-1 || errno!=EINVAL) return 9;
    return 0;
}

int main(int argc,char **argv)
{
    struct utsname platform;
    if (argc!=2 || strlen(argv[1])!=48 || uname(&platform) || getpid()<=1) return fail("arguments",1);
    const char *nonce=argv[1];
    printf("NATIVE_FONTS_PID:%s:%d\n",nonce,getpid()); fflush(stdout);
    int error;
    if ((error=native_udivti3_vectors()) || (error=native_udivmodti4_vectors())) return fail("wide arithmetic",error);
    printf("NATIVE_FONTS_STAGE:%s:arithmetic\n",nonce); fflush(stdout);
    FcConfig *config=NULL;
    if ((error=discovery(&config))) return fail("discovery/cache",error);
    printf("NATIVE_FONTS_STAGE:%s:discovery-cache\n",nonce); fflush(stdout);
    if ((error=shaping())) return fail("shaping/threads",error);
    printf("NATIVE_FONTS_STAGE:%s:arabic-threads\n",nonce); fflush(stdout);
    if ((error=drawing(nonce,config))) return fail("drawing/png/text",error);
    printf("NATIVE_FONTS_STAGE:%s:pixels-png-text\n",nonce); fflush(stdout);
    if ((error=objects())) return fail("GObject",error);
    printf("NATIVE_FONTS_STAGE:%s:objects\n",nonce); fflush(stdout);
    if ((error=shared_memory(nonce))) return fail("shared memory",error);
    printf("NATIVE_FONTS_STAGE:%s:shared-memory\n",nonce); fflush(stdout);
    FcConfigDestroy(config);
    printf("NATIVE_FONTS_OK:%s:%s:%s\n",nonce,platform.release,platform.machine); fflush(stdout);
    return 0;
}
