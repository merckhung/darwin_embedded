#include <mach-o/dyld_priv.h>
static unsigned notifications;
static void notify(unsigned count, const struct mach_header *headers[], const char *paths[])
{
    if (count && headers[0] && paths[0]) ++notifications;
}
__attribute__((constructor)) static void initialize(void)
{
    _dyld_register_for_bulk_image_loads(notify);
}
unsigned native_notifier_count(void) { return notifications; }
