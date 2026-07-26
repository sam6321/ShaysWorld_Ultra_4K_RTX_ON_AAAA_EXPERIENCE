#pragma once

#include <string_view>

enum class GraphicsPreset {
  Performance,
  Quality,
};

struct GraphicsSettings {
  GraphicsPreset preset = GraphicsPreset::Performance;

  bool shadows = false;
  bool volumetricSky = false;
  bool clouds = false;
  bool dayNightCycle = false;
  bool pbrIbl = false;
  bool depthPrepass = false;
  bool ambientOcclusion = false;
  bool antiAliasing = false;
  bool chromaticAberration = false;
  bool sharpening = false;
  bool bloom = false;

  bool volumetrics = false;     // god rays
  bool distanceFog = false;
  bool autoExposure = false;
  bool contactShadows = false;
  bool rain = false;
  bool localLights = false;
  bool lightsForcedOn = false; // L key — force walkway lamps

  float chromaticAberrationStrength = 0.15f;
  float sharpenStrength = 0.25f;
  float bloomStrength = 0.35f;
  float fogDensity = 0.00055f;
  float godrayIntensity = 0.45f;
  float timeOfDayHours = 14.0f;
  bool timeOfDayAuto = false;   // ImGui: advance clock in realtime
  float timeOfDaySpeed = 0.08f; // game-hours per real second (~5 min/day)
  float exposureBias = 0.0f; // EV stops, applied with auto-exposure

  void applyPreset(GraphicsPreset p);
  void togglePreset();
  [[nodiscard]] std::string_view presetName() const;
};
