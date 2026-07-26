#include "util/paths.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef SHAYS_ASSETS_DIR
#define SHAYS_ASSETS_DIR "assets"
#endif

namespace fs = std::filesystem;

std::string exeDirectory() {
#ifdef _WIN32
  char buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return ".";
  }
  fs::path p(buf);
  p = p.parent_path();
  return p.empty() ? std::string(".") : p.string();
#else
  char buf[PATH_MAX]{};
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return ".";
  }
  buf[n] = '\0';
  fs::path p(buf);
  p = p.parent_path();
  return p.empty() ? std::string(".") : p.string();
#endif
}

std::string resolveAssetsDirectory() {
  const fs::path besideExe = fs::path(exeDirectory()) / "assets";
  if (fs::exists(besideExe / "scene.bin")) {
    return besideExe.string();
  }
  const fs::path baked = fs::path(SHAYS_ASSETS_DIR);
  if (fs::exists(baked / "scene.bin")) {
    return baked.string();
  }
  // Last resort: cwd/assets (double-click / CI package layout).
  if (fs::exists(fs::path("assets") / "scene.bin")) {
    return "assets";
  }
  return besideExe.string();
}

void chdirToExeDirectory() {
  std::error_code ec;
  fs::current_path(exeDirectory(), ec);
}
