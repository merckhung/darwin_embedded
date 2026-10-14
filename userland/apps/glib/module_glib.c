#include <glib.h>
#include <gmodule.h>
static guint initialized;
__attribute__((constructor)) static void construct(void) { initialized = 7; }
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
{
    return module && g_module_name(module) && initialized == 7 ? NULL : "native module initialization failed";
}
G_MODULE_EXPORT gint native_module_value(gint value) { return value * 3 + initialized; }
