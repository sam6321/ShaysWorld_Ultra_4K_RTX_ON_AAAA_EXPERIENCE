#include "mesh_gen.h"
#include "raw_texture.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool writeMeshObj(const fs::path& path, const bake::Mesh& mesh) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "# shays_bake preview mesh\n";
  for (const auto& v : mesh.vertices) {
    out << "v " << v.px << ' ' << v.py << ' ' << v.pz << '\n';
  }
  for (const auto& v : mesh.vertices) {
    out << "vt " << v.u << ' ' << v.v << '\n';
  }
  for (const auto& v : mesh.vertices) {
    out << "vn " << v.nx << ' ' << v.ny << ' ' << v.nz << '\n';
  }
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const auto a = mesh.indices[i] + 1;
    const auto b = mesh.indices[i + 1] + 1;
    const auto c = mesh.indices[i + 2] + 1;
    out << "f " << a << '/' << a << '/' << a << ' ' << b << '/' << b << '/' << b << ' ' << c
        << '/' << c << '/' << c << '\n';
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  fs::path shaysData = R"(H:\ShaysWorld\Shays Code\bin\data)";
  fs::path outDir = R"(H:\ShaysWorld\modern\assets)";
  if (argc >= 2) {
    shaysData = argv[1];
  }
  if (argc >= 3) {
    outDir = argv[2];
  }

  fs::create_directories(outDir);
  fs::create_directories(outDir / "textures");
  fs::create_directories(outDir / "meshes");

  // v0: one grass hill plane from Shay coords, then WORLD_SCALE.
  // Mirrors a typical DrawGrass CreateDisplayList(XZ, ...) call shape.
  bake::Mesh grass = bake::createAxisAlignedQuad(
      bake::AxisMode::XZ,
      /*xImgSize*/ 64.0f, /*zImgSize*/ 64.0f,
      /*xStart*/ 26000.0f, /*yStart*/ 10000.0f, /*zStart*/ 10000.0f,
      /*xTimes*/ 40.0f, /*zTimes*/ 40.0f);
  grass.materialId = 1;
  bake::applyWorldScale(grass);

  const fs::path objPath = outDir / "meshes" / "grass_chunk_v0.obj";
  if (!writeMeshObj(objPath, grass)) {
    std::fprintf(stderr, "Failed to write %s\n", objPath.string().c_str());
    return 1;
  }

  // Convert a couple of common albedos to PPM for inspection (KTX2 comes later).
  struct Sample {
    const char* name;
    int w;
    int h;
  };
  const Sample samples[] = {
      {"grass.raw", 64, 64},
      {"pavement.raw", 128, 64},
      {"bricks1.raw", 128, 128},
  };
  for (const auto& s : samples) {
    const fs::path rawPath = shaysData / s.name;
    if (!fs::exists(rawPath)) {
      std::printf("skip missing %s\n", rawPath.string().c_str());
      continue;
    }
    bake::RawImage img = bake::loadRawRgb(rawPath.string(), s.w, s.h);
    if (img.rgb.empty()) {
      std::fprintf(stderr, "Failed to load %s\n", rawPath.string().c_str());
      continue;
    }
    const fs::path ppmPath = outDir / "textures" / (std::string(s.name) + ".ppm");
    if (!bake::writePpm(ppmPath.string(), img)) {
      std::fprintf(stderr, "Failed to write %s\n", ppmPath.string().c_str());
    } else {
      std::printf("wrote %s\n", ppmPath.string().c_str());
    }
  }

  std::ofstream meta(outDir / "meta.json");
  meta << "{\n"
       << "  \"worldScale\": " << bake::kWorldScale << ",\n"
       << "  \"originOffset\": [0, 0, 0],\n"
       << "  \"note\": \"bake v0 — single grass chunk + sample textures\"\n"
       << "}\n";

  std::printf("bake v0 complete: %zu verts, %zu indices -> %s\n", grass.vertices.size(),
              grass.indices.size(), objPath.string().c_str());
  return 0;
}
