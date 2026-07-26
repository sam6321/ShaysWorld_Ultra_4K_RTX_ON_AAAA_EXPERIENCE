#include "game/collision.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {

// Minimal JSON number/array reader for our fixed collision.json schema (no deps).
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool parseNumberAfter(const std::string& s, size_t& i, float& out) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ||
                          s[i] == ':' || s[i] == ','))
    ++i;
  size_t start = i;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
  if (start == i) return false;
  out = std::stof(s.substr(start, i - start));
  return true;
}

bool findKey(const std::string& s, size_t from, const char* key, size_t& outPos) {
  const std::string pat = std::string("\"") + key + "\"";
  size_t p = s.find(pat, from);
  if (p == std::string::npos) return false;
  outPos = p + pat.size();
  return true;
}

} // namespace

bool CollisionWorld::load(const std::string& path) {
  const std::string s = readFile(path);
  if (s.empty()) return false;

  plains_.clear();
  aabbs_.clear();

  // Parse aabbs array objects
  size_t aabbsKey = 0;
  if (!findKey(s, 0, "aabbs", aabbsKey)) return false;
  size_t arr = s.find('[', aabbsKey);
  size_t arrEnd = s.find(']', arr);
  if (arr == std::string::npos || arrEnd == std::string::npos) return false;
  size_t i = arr + 1;
  while (i < arrEnd) {
    size_t obj = s.find('{', i);
    if (obj == std::string::npos || obj > arrEnd) break;
    size_t objEnd = s.find('}', obj);
    AabbXZ box{};
    size_t p = obj;
    size_t k = 0;
    if (findKey(s, p, "minX", k) && k < objEnd) parseNumberAfter(s, k, box.minX);
    if (findKey(s, p, "maxX", k) && k < objEnd) parseNumberAfter(s, k, box.maxX);
    if (findKey(s, p, "minZ", k) && k < objEnd) parseNumberAfter(s, k, box.minZ);
    if (findKey(s, p, "maxZ", k) && k < objEnd) parseNumberAfter(s, k, box.maxZ);
    aabbs_.push_back(box);
    i = objEnd + 1;
  }

  size_t plainsKey = 0;
  if (!findKey(s, 0, "plains", plainsKey)) return false;
  arr = s.find('[', plainsKey);
  arrEnd = s.find(']', arr);
  // plains is last array — find matching close by scanning braces depth from arr
  {
    int depth = 0;
    arrEnd = std::string::npos;
    for (size_t j = arr; j < s.size(); ++j) {
      if (s[j] == '[') ++depth;
      else if (s[j] == ']') {
        --depth;
        if (depth == 0) {
          arrEnd = j;
          break;
        }
      }
    }
  }
  if (arr == std::string::npos || arrEnd == std::string::npos) return false;
  i = arr + 1;
  while (i < arrEnd) {
    size_t obj = s.find('{', i);
    if (obj == std::string::npos || obj > arrEnd) break;
    size_t objEnd = s.find('}', obj);
    Plain pl{};
    float typeF = 0;
    size_t p = obj;
    size_t k = 0;
    if (findKey(s, p, "type", k) && k < objEnd) parseNumberAfter(s, k, typeF);
    pl.type = static_cast<PlainType>(static_cast<int>(typeF + 0.5f));
    if (findKey(s, p, "minX", k) && k < objEnd) parseNumberAfter(s, k, pl.minX);
    if (findKey(s, p, "maxX", k) && k < objEnd) parseNumberAfter(s, k, pl.maxX);
    if (findKey(s, p, "minY", k) && k < objEnd) parseNumberAfter(s, k, pl.minY);
    if (findKey(s, p, "maxY", k) && k < objEnd) parseNumberAfter(s, k, pl.maxY);
    if (findKey(s, p, "minZ", k) && k < objEnd) parseNumberAfter(s, k, pl.minZ);
    if (findKey(s, p, "maxZ", k) && k < objEnd) parseNumberAfter(s, k, pl.maxZ);
    // Ensure min<=max on XZ
    if (pl.minX > pl.maxX) std::swap(pl.minX, pl.maxX);
    if (pl.minZ > pl.maxZ) std::swap(pl.minZ, pl.maxZ);
    plains_.push_back(pl);
    i = objEnd + 1;
  }

  return !aabbs_.empty() && !plains_.empty();
}

bool CollisionWorld::collidesXZ(float x, float z) const {
  for (const AabbXZ& b : aabbs_) {
    if (x > b.minX && x < b.maxX && z > b.minZ && z < b.maxZ) return true;
  }
  return false;
}

bool CollisionWorld::blocksMove(float x, float z, float dx, float dz, float lookahead) const {
  const float px = x + dx * lookahead;
  const float pz = z + dz * lookahead;
  return collidesXZ(px, pz);
}

FootingResult CollisionWorld::sampleFooting(float x, float z, float prevX, float prevZ, float curY,
                                            float moveX, float moveZ, int prevPlainIndex,
                                            float prevFlatHeight) const {
  FootingResult r{};
  r.eyeY = curY;
  r.plainIndex = prevPlainIndex;

  // Last matching plain wins (OG linked-list order).
  for (int i = 0; i < static_cast<int>(plains_.size()); ++i) {
    const Plain& p = plains_[static_cast<size_t>(i)];
    if (z < p.minZ || z > p.maxZ || x < p.minX || x > p.maxX) continue;

    r.onPlain = true;
    if (p.type == PlainType::Flat) {
      r.eyeY = p.minY;
      r.flatHeightChanged =
          (prevPlainIndex != i) && (std::abs(prevFlatHeight - p.minY) > 1e-4f);
      r.plainIndex = i;
    } else if (p.type == PlainType::ZY) {
      // Robust lerp (cleaner than OG incremental) along Z.
      const float denom = std::max(p.maxZ - p.minZ, 1e-4f);
      const float t = std::clamp((z - p.minZ) / denom, 0.0f, 1.0f);
      r.eyeY = p.minY + (p.maxY - p.minY) * t;
      r.plainIndex = i;
      (void)prevX;
      (void)prevZ;
      (void)moveX;
      (void)moveZ;
    } else if (p.type == PlainType::XY) {
      const float denom = std::max(p.maxX - p.minX, 1e-4f);
      const float t = std::clamp((x - p.minX) / denom, 0.0f, 1.0f);
      r.eyeY = p.minY + (p.maxY - p.minY) * t;
      r.plainIndex = i;
    }
  }
  return r;
}
