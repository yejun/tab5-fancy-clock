#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

// The Tab5 has ample PSRAM, but its WiFi/SDIO driver needs internal memory.
// Ordinary malloc places small font-cache and widget allocations internally;
// visiting more dates and screens can therefore starve the driver over time.
extern "C" {
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void* lv_malloc_core(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void* lv_realloc_core(void* ptr, size_t size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void lv_free_core(void* ptr) { heap_caps_free(ptr); }

void lv_mem_monitor_core(lv_mem_monitor_t* monitor) {
  memset(monitor, 0, sizeof(*monitor));
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  monitor->total_size = info.total_free_bytes + info.total_allocated_bytes;
  monitor->free_size = info.total_free_bytes;
  monitor->free_biggest_size = info.largest_free_block;
  monitor->free_cnt = info.free_blocks;
  monitor->used_cnt = info.allocated_blocks;
  if (monitor->total_size)
    monitor->used_pct = 100 - (uint64_t)monitor->free_size * 100 / monitor->total_size;
  if (monitor->free_size)
    monitor->frag_pct = 100 - (uint64_t)monitor->free_biggest_size * 100 / monitor->free_size;
}
lv_result_t lv_mem_test_core(void) {
  return heap_caps_check_integrity(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, true)
      ? LV_RESULT_OK : LV_RESULT_INVALID;
}
}
