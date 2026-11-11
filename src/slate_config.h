/* slate_config.h - Slate's build options, read once for every source of the package.
   The build defines PHPBM_OPTION_SLATE_EFFECTS_<VALUE> (1 or 0 for each value of the slate-effects set) and
   PHPBM_OPTION_SLATE_DEBUG in phpbm_options.h, and PHPBM_OPTION_SLATE_IMAGES_<VALUE> for the slate-images set
   (png). Without that header (or without a macro) every effect and image format is built and the debug profiler is
   not. An effect that is off is not compiled: its graphics-layer entry is NULL and a paint
   that would need it draws nothing (the PHP API throws before it gets there). */
#ifndef SLATE_CONFIG_H
#define SLATE_CONFIG_H

#if defined(__has_include)
#if __has_include("phpbm_options.h")
#include "phpbm_options.h"
#endif
#endif

#ifndef PHPBM_OPTION_SLATE_EFFECTS_GRADIENTS
#define PHPBM_OPTION_SLATE_EFFECTS_GRADIENTS 1
#endif
#ifndef PHPBM_OPTION_SLATE_EFFECTS_SHADOWS
#define PHPBM_OPTION_SLATE_EFFECTS_SHADOWS 1
#endif
#ifndef PHPBM_OPTION_SLATE_EFFECTS_BLUR
#define PHPBM_OPTION_SLATE_EFFECTS_BLUR 1
#endif
#ifndef PHPBM_OPTION_SLATE_IMAGES_PNG
#define PHPBM_OPTION_SLATE_IMAGES_PNG 1
#endif
#ifndef PHPBM_OPTION_SLATE_DEBUG
#define PHPBM_OPTION_SLATE_DEBUG 0
#endif

#define SLATE_HAS_GRADIENTS (PHPBM_OPTION_SLATE_EFFECTS_GRADIENTS != 0)
#define SLATE_HAS_SHADOWS (PHPBM_OPTION_SLATE_EFFECTS_SHADOWS != 0)
#define SLATE_HAS_BLUR (PHPBM_OPTION_SLATE_EFFECTS_BLUR != 0)
#define SLATE_DEBUG (PHPBM_OPTION_SLATE_DEBUG != 0)
/* PNG decoded on the board (png.c, inflate.c); .sli images are always supported. */
#define SLATE_HAS_PNG (PHPBM_OPTION_SLATE_IMAGES_PNG != 0)
/* The graphics layer's blur: the blur effect, and the alpha-only blur that makes a shadow. */
#define SLATE_GFX_BLUR (SLATE_HAS_BLUR || SLATE_HAS_SHADOWS)

#endif
