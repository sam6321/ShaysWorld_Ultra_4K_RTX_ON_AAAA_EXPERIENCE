#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bake {

struct RawImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb; // tightly packed RGB8
};

// Loads Shay-style headerless RGB .raw files.
RawImage loadRawRgb(const std::string& path, int width, int height);

// Writes a minimal binary PPM (P6) for quick inspection / intermediate assets.
bool writePpm(const std::string& path, const RawImage& image);

} // namespace bake
