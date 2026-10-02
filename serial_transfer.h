#pragma once
#include <stddef.h>
#include <stdint.h>

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// One shared absolute deadline for the entire transfer, plus a one-second stall limit.
// Clock exposes now_ms()/wait_ms(); the same implementation is exercised by host tests.
template<class Stream, class Clock>
static bool serial_write_all(Stream& stream, Clock& clock, const uint8_t* data,
                             size_t size, int64_t deadline_ms) {
  int64_t progress_at = clock.now_ms();
  while (size) {
    const int64_t now = clock.now_ms();
    if (now >= deadline_ms || now - progress_at >= 1000) return false;
    const size_t chunk = size < 512 ? size : 512;
    const size_t wrote = stream.write(data, chunk);
    if (wrote > chunk) return false;
    if (wrote) { data += wrote; size -= wrote; progress_at = clock.now_ms(); }
    else clock.wait_ms(1);
  }
  return true;
}
