#pragma once
#include <stdint.h>

// Join boxes only when their bounding rectangle costs no more pixels than the
// two existing boxes. This removes duplicate drawing without making sparse,
// diagonal hand updates expand into a large rectangular redraw.
template<class Area>
static void merge_dirty_regions(Area* boxes, int& count) {
  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count;) {
      const Area& a = boxes[i];
      const Area& b = boxes[j];
      Area u = {a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
                a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2};
      const int64_t separate = (int64_t)(a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1)
                             + (int64_t)(b.x2 - b.x1 + 1) * (b.y2 - b.y1 + 1);
      const int64_t united = (int64_t)(u.x2 - u.x1 + 1) * (u.y2 - u.y1 + 1);
      if (united <= separate) {
        boxes[i] = u;
        boxes[j] = boxes[--count];
        j = i + 1; // the enlarged rectangle may now also absorb an earlier box
      } else ++j;
    }
  }
}
