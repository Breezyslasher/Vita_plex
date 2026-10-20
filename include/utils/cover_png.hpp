#pragma once

// A song's cover made small, as the quick menu plugin shows it on the Vita
// (qm_protocol.h, QM_CMD_ART): the decoded image's middle square scaled down,
// and written as a PNG any decoder reads, PAF's among them.

#include <cstdint>
#include <string>
#include <vector>

namespace vitaplex {
namespace cover {

// The middle square of an RGB image, size pixels a side, each the average of
// the pixels it covers (the nearest one, scaling up).
std::vector<uint8_t> squareRgb(const uint8_t* rgb, int width, int height, int size);

// size x size RGB pixels as a PNG: 8 bits a channel, not interlaced, its
// image data stored in deflate's uncompressed blocks.
std::string pngRgb(const std::vector<uint8_t>& rgb, int size);

}  // namespace cover
}  // namespace vitaplex
