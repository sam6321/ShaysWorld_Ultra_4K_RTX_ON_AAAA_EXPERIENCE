#pragma once

#include <string>

// Tiny WAV player for OG step.wav (WinMM on Windows).
class StepAudio {
public:
  bool load(const std::string& path);
  void play();
  void shutdown();

private:
  std::string path_;
  bool ready_ = false;
};
