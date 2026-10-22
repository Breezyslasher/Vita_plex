/**
 * The song's cover for VitaPlex's quick menu plugin (qm_protocol.h,
 * QM_CMD_ART), from an image VitaPlex has fetched or has on the card:
 * decoded with stb_image, its middle square scaled to QM_ART_SIZE, and
 * written as a PNG that PAF's decoder in SceShell reads. Plain C.
 */

#ifndef VITAPLEX_QM_COVER_H
#define VITAPLEX_QM_COVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// An image (JPEG or PNG) as the cover named serial, at QM_ART_PATH_FMT.
// 0 when written; else -1, with what failed in why.
int qmCoverWrite(const void* image, size_t len, uint32_t serial, char* why, size_t whySize);

// The serial for the next cover, the last one made being last (0 for none),
// at nowUs on the system clock (sceKernelGetSystemTimeWide): the
// milliseconds since boot, past last, with last's other lowest bit, so that
// serials only grow in a boot, from process to process, and one after
// another goes to the other file (qm_protocol.h).
uint32_t qmCoverNextSerial(uint32_t last, uint64_t nowUs);

// The middle square of an RGB image, size pixels a side, into out (size *
// size * 3 bytes): each pixel the average of the pixels it covers, the
// nearest one when scaling up.
void qmCoverSquare(const uint8_t* rgb, int width, int height, int size, uint8_t* out);

// size x size RGB pixels as a PNG: 8 bits a channel, not interlaced, its
// image data in deflate's stored (uncompressed) blocks. malloc'd, its length
// in *len; NULL with no memory for it.
uint8_t* qmCoverPng(const uint8_t* rgb, int size, size_t* len);

#ifdef __cplusplus
}
#endif

#endif
