#include "core/RenderWindow.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "render window test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool sameRect(const CorridorKey::Core::RectD& actual,
              const CorridorKey::Core::RectD& expected) {
  return std::abs(actual.x1 - expected.x1) < 0.000001 &&
         std::abs(actual.y1 - expected.y1) < 0.000001 &&
         std::abs(actual.x2 - expected.x2) < 0.000001 &&
         std::abs(actual.y2 - expected.y2) < 0.000001;
}

}  // namespace

int main() {
  using namespace CorridorKey::Core;

  bool ok = true;

  const RectI oddBounds{0, 0, 5, 3};
  ok &= expect(width(oddBounds) == 5, "odd-width bounds should keep width");
  ok &= expect(height(oddBounds) == 3, "odd-height bounds should keep height");
  ok &= expect(!isEmpty(oddBounds), "positive bounds should not be empty");

  const RectI partialWindow{-2, 1, 4, 6};
  const RectI clipped = clipToBounds(partialWindow, oddBounds);
  ok &= expect(clipped == RectI{0, 1, 4, 3},
               "partial render window should clip to output bounds");

  const RectI empty = intersect(RectI{0, 0, 1, 1}, RectI{2, 2, 3, 3});
  ok &= expect(isEmpty(empty), "disjoint rectangles should produce an empty rect");

  ok &= expect(contains(RectI{1, 1, 4, 4}, 1, 1),
               "contains should include the lower bound");
  ok &= expect(!contains(RectI{1, 1, 4, 4}, 4, 4),
               "contains should exclude the upper bound");

  const RectD sourceRod{1.25, 2.5, 10.0, 12.75};
  ok &= expect(sameRect(passThroughRegionOfDefinition(sourceRod), sourceRod),
               "passthrough RoD should preserve source RoD");

  const RectD outputRoi{-0.5, 0.25, 3.5, 4.25};
  ok &= expect(sameRect(passThroughRegionOfInterest(outputRoi), outputRoi),
               "passthrough RoI should request the same source region");

  return ok ? 0 : 1;
}
