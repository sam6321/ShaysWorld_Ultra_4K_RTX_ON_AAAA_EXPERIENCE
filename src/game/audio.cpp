#include "game/audio.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <filesystem>

bool StepAudio::load(const std::string& path) {
  path_.clear();
  ready_ = false;
  if (!std::filesystem::exists(path)) {
    return false;
  }
  path_ = path;
  ready_ = true;
  return true;
}

void StepAudio::play() {
  if (!ready_) return;
#ifdef _WIN32
  // Async + no-stop so overlapping stairs still click; SND_NODEFAULT avoids beep on miss.
  PlaySoundA(path_.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#else
  (void)path_;
#endif
}

void StepAudio::shutdown() {
#ifdef _WIN32
  PlaySoundA(nullptr, nullptr, 0);
#endif
  ready_ = false;
}
