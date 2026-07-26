#include "scene/scene.h"

#include <cstdio>
#include <fstream>

namespace scene {
namespace {

template <typename T>
bool readPod(std::ifstream& in, T& value) {
  in.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(in);
}

} // namespace

bool loadScene(const std::string& binPath, const std::string& assetsRoot, Scene& out) {
  out = {};
  std::ifstream in(binPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "Failed to open %s\n", binPath.c_str());
    return false;
  }

  char magic[4]{};
  in.read(magic, 4);
  if (magic[0] != 'S' || magic[1] != 'H' || magic[2] != 'A' || magic[3] != 'Y') {
    std::fprintf(stderr, "Bad scene magic\n");
    return false;
  }

  uint32_t version = 0;
  readPod(in, version);
  readPod(in, out.worldScale);
  uint32_t vertexCount = 0, indexCount = 0, submeshCount = 0, materialCount = 0;
  readPod(in, vertexCount);
  readPod(in, indexCount);
  readPod(in, submeshCount);
  readPod(in, materialCount);

  out.vertices.resize(vertexCount);
  in.read(reinterpret_cast<char*>(out.vertices.data()),
          static_cast<std::streamsize>(vertexCount * sizeof(Vertex)));
  out.indices.resize(indexCount);
  in.read(reinterpret_cast<char*>(out.indices.data()),
          static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));

  out.submeshes.resize(submeshCount);
  for (uint32_t i = 0; i < submeshCount; ++i) {
    readPod(in, out.submeshes[i].material);
    readPod(in, out.submeshes[i].firstIndex);
    readPod(in, out.submeshes[i].indexCount);
  }

  out.materials.resize(materialCount);
  for (uint32_t i = 0; i < materialCount; ++i) {
    char name[64]{};
    char tex[128]{};
    in.read(name, 64);
    in.read(tex, 128);
    out.materials[i].name = name;
    out.materials[i].texturePath = tex;
    readPod(in, out.materials[i].width);
    readPod(in, out.materials[i].height);
    readPod(in, out.materials[i].metallic);
    readPod(in, out.materials[i].roughness);

    if (!out.materials[i].texturePath.empty()) {
      const std::string full = assetsRoot + "/" + out.materials[i].texturePath;
      std::ifstream texIn(full, std::ios::binary);
      if (texIn) {
        const size_t bytes =
            static_cast<size_t>(out.materials[i].width) * out.materials[i].height * 3u;
        out.materials[i].rgb.resize(bytes);
        texIn.read(reinterpret_cast<char*>(out.materials[i].rgb.data()),
                   static_cast<std::streamsize>(bytes));
        if (!texIn) {
          out.materials[i].rgb.clear();
        }
      }
    }
    if (out.materials[i].rgb.empty()) {
      // Magenta placeholder
      out.materials[i].width = 1;
      out.materials[i].height = 1;
      out.materials[i].rgb = {255, 0, 255};
    }
  }

  std::printf("Loaded scene: %u verts, %u indices, %u submeshes, %u materials\n", vertexCount,
              indexCount, submeshCount, materialCount);
  return static_cast<bool>(in) || in.eof();
}

} // namespace scene
