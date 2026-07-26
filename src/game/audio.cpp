#include "game/audio.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
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
#ifndef _WIN32
  // Avoid zombie aplay/paplay children.
  std::signal(SIGCHLD, SIG_IGN);
#endif
  return true;
}

void StepAudio::play() {
  if (!ready_) return;
#ifdef _WIN32
  // Async + no-stop so overlapping stairs still click; SND_NODEFAULT avoids beep on miss.
  PlaySoundA(path_.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#else
  const pid_t pid = fork();
  if (pid == 0) {
    const int nullFd = open("/dev/null", O_WRONLY);
    if (nullFd >= 0) {
      dup2(nullFd, STDOUT_FILENO);
      dup2(nullFd, STDERR_FILENO);
      if (nullFd > STDERR_FILENO) {
        close(nullFd);
      }
    }
    // Prefer ALSA aplay; fall back to Pulse paplay.
    execlp("aplay", "aplay", "-q", path_.c_str(), static_cast<char*>(nullptr));
    execlp("paplay", "paplay", path_.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
#endif
}

void StepAudio::shutdown() {
#ifdef _WIN32
  PlaySoundA(nullptr, nullptr, 0);
#endif
  ready_ = false;
}
