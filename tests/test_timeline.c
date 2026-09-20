#include "auris_timeline.h"
#include <stdint.h>

#define CHECK(expression) do { if (!(expression)) return 1; } while (0)

int main(void) {
  const auris_timeline_point_t points[] = {
      {900U, 930U, true}, {970U, 1000U, true}, {1010U, 1040U, true},
      {995U, 1045U, false}};
  CHECK(auris_timeline_match(points, 4U, 980U, 1050U, 200U, 100U) == 1U);
  CHECK(auris_timeline_match(points, 4U, 990U, 1050U, 200U, 100U) == 2U);
  CHECK(auris_timeline_match(points, 4U, 700U, 1050U, 200U, 100U) == SIZE_MAX);
  CHECK(auris_timeline_match(points, 4U, 980U, 1300U, 200U, 100U) == SIZE_MAX);
  CHECK(auris_timeline_match(NULL, 0U, 980U, 1050U, 200U, 100U) == SIZE_MAX);
  return 0;
}
