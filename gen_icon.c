/* gen_icon.c — export the bento window icon to PNG files at several sizes.
 *
 * Reuses guiBuildBentoIcon from bento.c (compiled with bento_RAYLIB and
 * bento_NO_MAIN). Produces icon-NN.png for NN in {16,32,64,128,256,512,1024}
 * so platform-specific bundlers (iconutil, png2icns, ImageMagick) can pack
 * them into .icns / .ico / .desktop. */

#define bento_NO_MAIN
#include "bento.c"

#include <stdio.h>

static const int icon_sizes[] = {16, 32, 64, 128, 256, 512, 1024};
#define ICON_SIZE_COUNT (int)(sizeof(icon_sizes) / sizeof(icon_sizes[0]))

int main(void) {
  /* Headless raylib still needs a window for image utilities to function on
   * some platforms; an invisible 1x1 window is enough. */
  SetTraceLogLevel(LOG_WARNING);
  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(1, 1, "bento-icon-gen");

  for (int i = 0; i < ICON_SIZE_COUNT; i++) {
    int sz = icon_sizes[i];
    Image img = guiBuildBentoIcon(sz);
    char path[64];
    snprintf(path, sizeof(path), "icon-%d.png", sz);
    if (!ExportImage(img, path)) {
      fprintf(stderr, "failed to write %s\n", path);
      UnloadImage(img);
      CloseWindow();
      return 1;
    }
    UnloadImage(img);
    printf("wrote %s\n", path);
  }

  CloseWindow();
  return 0;
}
