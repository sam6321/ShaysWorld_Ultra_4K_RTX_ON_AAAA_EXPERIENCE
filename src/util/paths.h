#pragma once

#include <string>

// Directory containing the running executable (no trailing slash).
std::string exeDirectory();

// Prefer <exe>/assets (portable), else compile-time SHAYS_ASSETS_DIR (dev builds).
std::string resolveAssetsDirectory();

// Make relative paths like "shaders/foo.spv" resolve next to the exe.
void chdirToExeDirectory();
