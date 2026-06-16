#pragma once

namespace CorridorKey::Core {

struct RectI {
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
};

struct RectD {
  double x1 = 0.0;
  double y1 = 0.0;
  double x2 = 0.0;
  double y2 = 0.0;
};

constexpr bool operator==(const RectI& lhs, const RectI& rhs) {
  return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 &&
         lhs.y2 == rhs.y2;
}

constexpr bool operator!=(const RectI& lhs, const RectI& rhs) {
  return !(lhs == rhs);
}

int width(const RectI& rect);
int height(const RectI& rect);
bool isEmpty(const RectI& rect);
bool contains(const RectI& rect, int x, int y);

RectI intersect(const RectI& lhs, const RectI& rhs);
RectI clipToBounds(const RectI& renderWindow, const RectI& bounds);

RectD passThroughRegionOfDefinition(const RectD& sourceRegion);
RectD passThroughRegionOfInterest(const RectD& outputRegion);

}  // namespace CorridorKey::Core
