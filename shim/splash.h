#pragma once
#include <stdint.h>
/* The boot screen (splash.c): returns when xnu should start; with Volume up held it restarts into recovery. */
void splash_run(uintptr_t dtb);
/*
 * Once the boot screen is on the panel: the picture's physical address (splash.bin), which loader.c
 * passes to xnu as sweet_splash=0x... (0299: xnu paints the boot screen, no log); 0 otherwise.
 */
extern uintptr_t splash_shown;
