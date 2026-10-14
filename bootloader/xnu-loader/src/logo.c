#include "boot.h"
#include "pd_logo.h"

// the graphical boot screen: black with the PureDarwin mark, xnu then draws its bar below
// the mark is a fifth of the screen height, centred a little above the middle
void boot_draw_logo(boot_args *args) {
  UINT32 *fb;
  UINT32 width, height, pitch, size, ox, oy, x, y;

  if (!args || args->Video.v_display != GRAPHICS_MODE || args->Video.v_depth != 32)
    return;

  fb = (UINT32 *)(UINTN)(args->Video.v_baseAddr & ~3ULL);
  width = args->Video.v_width, height = args->Video.v_height;
  pitch = args->Video.v_rowBytes / 4;
  if (!fb || !width || !height)
    return;

  for (y = 0; y < height; y++) {
    for (x = 0; x < width; x++) {
      fb[y * pitch + x] = 0;
    }
  }

  size = height / 5;
  ox = (width - size) / 2, oy = (height * 45) / 100 - size / 2;
  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      UINT32 a = pd_logo_alpha[(y * PD_LOGO_SIZE / size) * PD_LOGO_SIZE + x * PD_LOGO_SIZE / size];

      fb[(oy + y) * pitch + ox + x] = (a << 16) | (a << 8) | a;
    }
  }
}
