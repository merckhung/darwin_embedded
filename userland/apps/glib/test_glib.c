/* Genuine native GLib/GObject/GModule behavior, not a desktop substitute. */
#include <glib.h>
#include <glib-object.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

typedef struct { GObject parent; gint value; } NativeProbe;
typedef struct { GObjectClass parent; } NativeProbeClass;
static guint finalized, weak_notified, mix_signal;
G_DEFINE_TYPE(NativeProbe, native_probe, G_TYPE_OBJECT)

static void set_property(GObject *object, guint property, const GValue *value, GParamSpec *spec)
{
    if (property == 1) ((NativeProbe *)object)->value = g_value_get_int(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}
static void get_property(GObject *object, guint property, GValue *value, GParamSpec *spec)
{
    if (property == 1) g_value_set_int(value, ((NativeProbe *)object)->value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}
static void finalize(GObject *object)
{
    ++finalized;
    G_OBJECT_CLASS(native_probe_parent_class)->finalize(object);
}
static void native_probe_class_init(NativeProbeClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    g_object_class_install_property(object, 1, g_param_spec_int("value", "value", "native value",
                                    -100, 100, 0, G_PARAM_READWRITE));
    mix_signal = g_signal_new("mix", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_DOUBLE, 2, G_TYPE_UINT64, G_TYPE_DOUBLE);
}
static void native_probe_init(NativeProbe *object) { object->value = 0; }
static double mix(NativeProbe *object, guint64 number, double fraction, gpointer context)
{
    return object->value + (double)number + fraction + *(double *)context;
}
static void weak_notify(gpointer context, GObject *object)
{
    (void)object;
    ++*(guint *)context;
}

static int objects(void)
{
    NativeProbe *object = g_object_new(native_probe_get_type(), "value", 7, NULL);
    if (!object || !G_IS_OBJECT(object) || strcmp(G_OBJECT_TYPE_NAME(object), "NativeProbe")) return 1;
    gint value = 0;
    g_object_get(object, "value", &value, NULL);
    if (value != 7) return 2;
    g_object_set(object, "value", 11, NULL);
    double bias = 2.5, result = 0;
    gulong handler = g_signal_connect(object, "mix", G_CALLBACK(mix), &bias);
    g_signal_emit(object, mix_signal, 0, (guint64)17, 0.75, &result);
    if (!handler || result != 31.25) return 3;
    g_signal_handler_disconnect(object, handler);
    g_object_weak_ref(G_OBJECT(object), weak_notify, &weak_notified);
    g_object_ref(object);
    g_object_unref(object);
    if (finalized || weak_notified) return 4;
    g_object_unref(object);
    return finalized == 1 && weak_notified == 1 ? 0 : 5;
}

static GPrivate private_state = G_PRIVATE_INIT(g_free);
static GMutex mutex;
static GCond condition;
static guint ready, go;
static gpointer thread(gpointer opaque)
{
    guint index = GPOINTER_TO_UINT(opaque);
    guint *value = g_new(guint, 1);
    *value = index + 100;
    g_private_set(&private_state, value);
    g_mutex_lock(&mutex);
    ++ready;
    g_cond_broadcast(&condition);
    while (!go) g_cond_wait(&condition, &mutex);
    g_mutex_unlock(&mutex);
    guint result = *(guint *)g_private_get(&private_state);
    return GUINT_TO_POINTER(result);
}

static gboolean tick(gpointer opaque)
{
    guint *count = opaque;
    ++*count;
    return G_SOURCE_REMOVE;
}
static int events(void)
{
    GThread *threads[4];
    for (guint index = 0; index < 4; ++index) threads[index] = g_thread_new("native-worker", thread, GUINT_TO_POINTER(index));
    g_mutex_lock(&mutex);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (ready != 4)
        if (!g_cond_wait_until(&condition, &mutex, deadline)) { g_mutex_unlock(&mutex); return 1; }
    go = 1;
    g_cond_broadcast(&condition);
    g_mutex_unlock(&mutex);
    for (guint index = 0; index < 4; ++index)
        if (GPOINTER_TO_UINT(g_thread_join(threads[index])) != index + 100) return 2;
    if (g_private_get(&private_state)) return 3;
    g_cond_clear(&condition);
    g_mutex_clear(&mutex);
    guint count = 0;
    guint source = g_timeout_add(10, tick, &count);
    if (!source) return 4;
    deadline = g_get_monotonic_time() + 5000000;
    while (!count && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
    return count == 1 ? 0 : 5;
}

static int data(const char *nonce)
{
    const char *decomposed = "Cafe\xcc\x81";
    gchar *normalized = g_utf8_normalize(decomposed, -1, G_NORMALIZE_NFC);
    if (!normalized || strcmp(normalized, "Caf\xc3\xa9")) return 1;
    g_free(normalized);
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(table, g_strdup("native"), g_strdup(nonce));
    if (strcmp(g_hash_table_lookup(table, "native"), nonce)) return 2;
    g_hash_table_unref(table);
    gchar *text = g_strdup_printf("%" G_GINT64_FORMAT ":%s", G_GINT64_CONSTANT(1234567890123), nonce);
    gchar *expected = g_strconcat("1234567890123:", nonce, NULL);
    if (strcmp(text, expected)) return 3;
    g_free(text); g_free(expected);
    gchar *path = g_strconcat("/tmp/native-glib-", nonce, NULL);
    GError *error = NULL;
    if (!g_file_set_contents(path, nonce, -1, &error)) return 4;
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, &error) || length != 48 || memcmp(contents, nonce, 48)) return 5;
    g_free(contents);
    if (g_remove(path)) return 6;
    g_free(path);
    GDir *directory = g_dir_open("/tmp", 0, &error);
    if (!directory) return 7;
    while (g_dir_read_name(directory)) { }
    g_dir_close(directory);
    GLogField field = {"MESSAGE", "no native syslog service", -1};
    return g_log_writer_syslog(G_LOG_LEVEL_MESSAGE, &field, 1, NULL) == G_LOG_WRITER_UNHANDLED ? 0 : 8;
}

static int modules(void)
{
    if (!g_module_supported()) return 1;
    GModule *module = g_module_open("/usr/lib/native-glib-module.so", G_MODULE_BIND_LOCAL);
    if (!module) { fprintf(stderr, "Native glib module error: %s\n", g_module_error()); return 2; }
    gint (*function)(gint) = NULL;
    if (!g_module_symbol(module, "native_module_value", (gpointer *)&function) || !function || function(7) != 28) return 3;
    if (!g_module_close(module)) return 4;
    module = g_module_open("/missing-native-module.so", G_MODULE_BIND_LOCAL);
    if (module || !g_module_error()) return 5;
    return 0;
}

static int versions(void)
{
    const struct mach_header_64 *header = (const void *)_dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64) return 1;
    const unsigned char *cursor = (const void *)(header + 1);
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_BUILD_VERSION) {
            const struct build_version_command *version = (const void *)command;
            /* Fixture intentionally links minimum26.0 with SDK26.5. */
            return version->minos != version->sdk && dyld_get_program_min_os_version() == version->minos ? 0 : 2;
        }
        cursor += command->cmdsize;
    }
    return 3;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 48 || strspn(argv[1], "0123456789abcdef") != 48) return 1;
    if (glib_major_version != 2 || glib_minor_version != 90 || glib_micro_version != 1) return 2;
    int result;
#define CHECK(stage, expression) do { fprintf(stderr, "Native glib stage: %s\n", stage); result=(expression); \
    if (result) { fprintf(stderr, "Native glib failure: %s check %d\n", stage, result); return 3; } } while (0)
    CHECK("version", versions());
    CHECK("data/files", data(argv[1]));
    CHECK("objects/signals", objects());
    CHECK("threads/events", events());
    CHECK("modules", modules());
    struct utsname identity;
    if (uname(&identity)) return 4;
    printf("NATIVE_GLIB_OK:%s:%s:%s\n", argv[1], identity.release, identity.machine);
    return 0;
}
