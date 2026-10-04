/* LVGL allocator (LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM) that keeps all UI
 * objects in PSRAM. With plain malloc every small widget allocation lands in
 * internal RAM, which Wi-Fi needs for its packet buffers; when that runs short
 * packets get dropped and every request stalls on TCP retransmits. */
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
  LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
  return heap_caps_malloc(size, PSRAM_CAPS);
}

void *lv_realloc_core(void *p, size_t new_size) {
  return heap_caps_realloc(p, new_size, PSRAM_CAPS);
}

void lv_free_core(void *p) {
  heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) {
  return LV_RESULT_OK;
}

#endif
