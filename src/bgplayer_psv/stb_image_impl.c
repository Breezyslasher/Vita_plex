/**
 * stb_image for the background player, which has no borealis to bring it,
 * as VitaPlex BG's nanovg does: JPEG and PNG, what Plex's photo transcoder
 * and VitaPlex's downloads give (qm_cover.c).
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
