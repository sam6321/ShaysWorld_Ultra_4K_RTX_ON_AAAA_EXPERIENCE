#include "settings/graphics_settings.h"

void GraphicsSettings::applyPreset(GraphicsPreset p) {
  preset = p;
  const bool quality = (p == GraphicsPreset::Quality);

  shadows = quality;
  volumetricSky = quality;
  clouds = quality;
  dayNightCycle = quality;
  pbrIbl = quality;
  depthPrepass = quality;
  ambientOcclusion = quality;
  antiAliasing = quality;
  chromaticAberration = quality;
  sharpening = quality;
  bloom = quality;
  volumetrics = quality;
  distanceFog = quality;
  autoExposure = quality;
  contactShadows = quality;
  localLights = quality;
  // rain stays user-toggled; default off even in Quality
  if (!quality) {
    rain = false;
    lightsForcedOn = false;
    timeOfDayHours = 14.0f;
  } else if (timeOfDayHours < 0.0f || timeOfDayHours > 24.0f) {
    timeOfDayHours = 16.5f;
  }
}

void GraphicsSettings::togglePreset() {
  applyPreset(preset == GraphicsPreset::Performance ? GraphicsPreset::Quality
                                                    : GraphicsPreset::Performance);
}

std::string_view GraphicsSettings::presetName() const {
  return preset == GraphicsPreset::Performance ? "Performance" : "Quality";
}
