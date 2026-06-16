#include "core/RenderWindow.h"

#include <algorithm>

namespace CorridorKey::Core {

int width(const RectI& rect) {
  return std::max(0, rect.x2 - rect.x1);
}

int height(const RectI& rect) {
  return std::max(0, rect.y2 - rect.y1);
}

bool isEmpty(const RectI& rect) {
  return width(rect) == 0 || height(rect) == 0;
}

bool contains(const RectI& rect, int x, int y) {
  return x >= rect.x1 && x < rect.x2 && y >= rect.y1 && y < rect.y2;
}

RectI intersect(const RectI& lhs, const RectI& rhs) {
  const RectI result{
      std::max(lhs.x1, rhs.x1),
      std::max(lhs.y1, rhs.y1),
      std::min(lhs.x2, rhs.x2),
      std::min(lhs.y2, rhs.y2),
  };
  if (isEmpty(result)) {
    return RectI{result.x1, result.y1, result.x1, result.y1};
  }
  return result;
}

RectI clipToBounds(const RectI& renderWindow, const RectI& bounds) {
  return intersect(renderWindow, bounds);
}

RectD passThroughRegionOfDefinition(const RectD& sourceRegion) {
  return sourceRegion;
}

RectD passThroughRegionOfInterest(const RectD& outputRegion) {
  return outputRegion;
}

}  // namespace CorridorKey::Core
