#include "raw_texture.h"

#include <cstdio>
#include <fstream>

namespace bake {

RawImage loadRawRgb(const std::string& path, int width, int height) {
  RawImage img;
  img.width = width;
  img.height = height;
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
  img.rgb.resize(bytes);

  FILE* f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
    img.rgb.clear();
    return img;
  }
#else
  f = fopen(path.c_str(), "rb");
  if (!f) {
    img.rgb.clear();
    return img;
  }
#endif
  const size_t read = fread(img.rgb.data(), 1, bytes, f);
  fclose(f);
  if (read != bytes) {
    img.rgb.clear();
  }
  return img;
}

bool writePpm(const std::string& path, const RawImage& image) {
  if (image.rgb.empty()) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(image.rgb.data()),
            static_cast<std::streamsize>(image.rgb.size()));
  return static_cast<bool>(out);
}

} // namespace bake
