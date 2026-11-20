#pragma once

// Only hardware/RTOS/storage are simulated. The app and Unleashed IR codecs are real C.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <infrared.h>
#include "shims/core/common_defines.h"
#include "shims/core/check.h"

#define EXT_PATH(s)     "/tmp/ir-lab-simulation/" s
#define RECORD_GUI      "gui"
#define RECORD_STORAGE  "storage"
#define FuriWaitForever UINT32_MAX
typedef enum {
    FuriStatusOk,
    FuriStatusError
} FuriStatus;
typedef enum {
    FuriMutexTypeNormal
} FuriMutexType;
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyLeft,
    InputKeyRight,
    InputKeyOk,
    InputKeyBack
} InputKey;
typedef enum {
    InputTypePress,
    InputTypeRelease,
    InputTypeShort,
    InputTypeLong,
    InputTypeRepeat
} InputType;
typedef struct {
    InputKey key;
    InputType type;
} InputEvent;
typedef struct {
    unsigned locked;
} FuriMutex;
typedef struct {
    size_t count, size, capacity;
    unsigned char data[512];
} FuriMessageQueue;
typedef struct {
    int unused;
} Gui;
typedef struct Storage {
    int unused;
} Storage;
typedef struct {
    int unused;
} ViewPort;
typedef struct {
    int unused;
} Canvas;
typedef enum {
    FontSecondary,
    FontPrimary
} Font;
typedef enum {
    GuiLayerFullscreen
} GuiLayer;
typedef enum {
    FuriHalInfraredTxPinInternal
} FuriHalInfraredTxPin;
typedef enum {
    FSAM_WRITE
} FS_AccessMode;
typedef enum {
    FSOM_OPEN_APPEND,
    FSOM_CREATE_NEW,
    FSOM_CREATE_ALWAYS
} FS_OpenMode;
typedef enum {
    FSE_OK,
    FSE_NOT_EXIST,
    FSE_EXIST,
    FSE_ERROR
} FS_Error;
typedef struct {
    FILE* fp;
} File;
typedef struct {
    FILE* fp;
} FlipperFormat;
typedef struct {
    bool decoded;
    InfraredMessage message;
    size_t count;
    uint32_t timings[1024];
} InfraredWorkerSignal;
typedef struct {
    bool running, decoding;
    void (*callback)(void*, InfraredWorkerSignal*);
    void* context;
} InfraredWorker;

static uint32_t sim_tick = 100;
static bool sim_sd_fail;
static bool sim_sync_fail;
static bool sim_rename_fail;
static bool sim_remove_fail;
static int sim_meta_rename_fail_after = -1;
static unsigned sim_tx_count;
static IrLabSignal sim_last_tx;
typedef struct {
    char op;
    int x, y, x2, y2, color, font;
    char text[40];
} SimDraw;
static SimDraw sim_draw[64];
static unsigned sim_draw_count;
static int sim_color = 1, sim_font;
static char sim_canvas[6][40];
static unsigned sim_boxes, sim_lines, sim_inversions;
static void (*sim_write_hook)(void);
static void (*sim_tx_hook)(void);

static inline uint32_t furi_get_tick(void) {
    return sim_tick;
}
static inline uint32_t furi_kernel_get_tick_frequency(void) {
    return 1000;
}
static inline uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}
static inline uint32_t furi_hal_rtc_get_timestamp(void) {
    return 1234567890;
}
static inline FuriMutex* furi_mutex_alloc(FuriMutexType type) {
    UNUSED(type);
    return calloc(1, sizeof(FuriMutex));
}
static inline void furi_mutex_free(FuriMutex* mutex) {
    assert(!mutex->locked);
    free(mutex);
}
static inline void furi_mutex_acquire(FuriMutex* mutex, uint32_t timeout) {
    UNUSED(timeout);
    assert(mutex && !mutex->locked);
    mutex->locked = 1;
}
static inline void furi_mutex_release(FuriMutex* mutex) {
    assert(mutex->locked);
    mutex->locked = 0;
}
static inline FuriMessageQueue* furi_message_queue_alloc(size_t capacity, size_t size) {
    FuriMessageQueue* queue = calloc(1, sizeof(*queue));
    assert(capacity * size <= sizeof(queue->data));
    queue->size = size;
    queue->capacity = capacity;
    return queue;
}
static inline void furi_message_queue_free(FuriMessageQueue* queue) {
    free(queue);
}
static inline FuriStatus furi_message_queue_reset(FuriMessageQueue* queue) {
    queue->count = 0;
    return FuriStatusOk;
}
static inline FuriStatus
    furi_message_queue_put(FuriMessageQueue* queue, const void* event, uint32_t timeout) {
    UNUSED(timeout);
    if(queue->count == queue->capacity) return FuriStatusError;
    memcpy(queue->data + queue->count++ * queue->size, event, queue->size);
    return FuriStatusOk;
}
static inline FuriStatus
    furi_message_queue_get(FuriMessageQueue* queue, void* event, uint32_t timeout) {
    UNUSED(timeout);
    if(!queue->count) return FuriStatusError;
    memcpy(event, queue->data, queue->size);
    --queue->count;
    memmove(queue->data, queue->data + queue->size, queue->count * queue->size);
    return FuriStatusOk;
}
static inline void* furi_record_open(const char* name) {
    UNUSED(name);
    return calloc(1, 1);
}
static inline void furi_record_close(const char* name) {
    UNUSED(name);
}
static inline ViewPort* view_port_alloc(void) {
    return calloc(1, sizeof(ViewPort));
}
static inline void view_port_free(ViewPort* view) {
    free(view);
}
static inline void view_port_update(ViewPort* view) {
    UNUSED(view);
}
static inline void view_port_enabled_set(ViewPort* view, bool enabled) {
    UNUSED(view);
    UNUSED(enabled);
}
static inline void
    view_port_draw_callback_set(ViewPort* view, void (*callback)(Canvas*, void*), void* ctx) {
    UNUSED(view);
    UNUSED(callback);
    UNUSED(ctx);
}
static inline void
    view_port_input_callback_set(ViewPort* view, void (*callback)(InputEvent*, void*), void* ctx) {
    UNUSED(view);
    UNUSED(callback);
    UNUSED(ctx);
}
static inline void gui_add_view_port(Gui* gui, ViewPort* view, GuiLayer layer) {
    UNUSED(gui);
    UNUSED(view);
    UNUSED(layer);
}
static inline void gui_remove_view_port(Gui* gui, ViewPort* view) {
    UNUSED(gui);
    UNUSED(view);
}
static inline void canvas_clear(Canvas* canvas) {
    UNUSED(canvas);
    memset(sim_canvas, 0, sizeof(sim_canvas));
    sim_boxes = sim_lines = sim_inversions = 0;
    sim_draw_count = 0;
    sim_color = 1;
    sim_font = 0;
}
static inline void canvas_set_font(Canvas* canvas, Font font) {
    UNUSED(canvas);
    sim_font = font;
}
static inline uint16_t canvas_string_width(Canvas* canvas, const char* text) {
    UNUSED(canvas);
    return strlen(text) * 5;
}
static inline void canvas_invert_color(Canvas* canvas) {
    UNUSED(canvas);
    ++sim_inversions;
    sim_color ^= 1;
}
static inline void canvas_draw_box(Canvas* canvas, int x, int y, size_t w, size_t h) {
    UNUSED(canvas);
    assert(x >= 0 && y >= 0 && x + w <= 128 && y + h <= 64);
    ++sim_boxes;
    assert(sim_draw_count < 64);
    sim_draw[sim_draw_count++] =
        (SimDraw){.op = 'b', .x = x, .y = y, .x2 = w, .y2 = h, .color = sim_color};
}
static inline void canvas_draw_line(Canvas* canvas, int x, int y, int x2, int y2) {
    UNUSED(canvas);
    assert(x >= 0 && y >= 0 && x2 < 128 && y2 < 64);
    ++sim_lines;
    assert(sim_draw_count < 64);
    sim_draw[sim_draw_count++] =
        (SimDraw){.op = 'l', .x = x, .y = y, .x2 = x2, .y2 = y2, .color = sim_color};
}
static inline void canvas_draw_str(Canvas* canvas, int x, int y, const char* text) {
    UNUSED(canvas);
    assert(x >= 0 && y >= 0 && y < 64);
    unsigned row = y == 10 ? 0 : y == 62 ? 5 : (y - 22) / 10 + 1;
    assert(row < 6);
    snprintf(sim_canvas[row], 40, "%s", text);
    assert(sim_draw_count < 64);
    SimDraw* d = &sim_draw[sim_draw_count++];
    *d = (SimDraw){.op = 't', .x = x, .y = y, .color = sim_color, .font = sim_font};
    snprintf(d->text, 40, "%s", text);
}
static inline InfraredWorker* infrared_worker_alloc(void) {
    return calloc(1, sizeof(InfraredWorker));
}
static inline void infrared_worker_free(InfraredWorker* worker) {
    assert(!worker->running);
    free(worker);
}
static inline void infrared_worker_rx_start(InfraredWorker* worker) {
    assert(!worker->running);
    worker->running = true;
}
static inline void infrared_worker_rx_stop(InfraredWorker* worker) {
    assert(worker->running);
    worker->running = false;
}
static inline void infrared_worker_rx_set_received_signal_callback(
    InfraredWorker* worker,
    void (*callback)(void*, InfraredWorkerSignal*),
    void* context) {
    worker->callback = callback;
    worker->context = context;
}
static inline void
    infrared_worker_rx_enable_signal_decoding(InfraredWorker* worker, bool enabled) {
    worker->decoding = enabled;
}
static inline bool infrared_worker_signal_is_decoded(const InfraredWorkerSignal* signal) {
    return signal->decoded;
}
static inline const InfraredMessage*
    infrared_worker_get_decoded_signal(const InfraredWorkerSignal* signal) {
    return &signal->message;
}
static inline void infrared_worker_get_raw_signal(
    const InfraredWorkerSignal* signal,
    const uint32_t** timings,
    size_t* count) {
    *timings = signal->timings;
    *count = signal->count;
}
static inline void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin pin) {
    assert(pin == FuriHalInfraredTxPinInternal);
}
static inline void infrared_send_raw_ext(
    const uint32_t* timings,
    uint32_t count,
    bool mark,
    uint32_t frequency,
    float duty) {
    assert(mark && count <= 1024);
    memset(&sim_last_tx, 0, sizeof(sim_last_tx));
    sim_last_tx.valid = true;
    sim_last_tx.frequency = frequency;
    sim_last_tx.duty_cycle = duty;
    sim_last_tx.count = count;
    memcpy(sim_last_tx.timings, timings, count * sizeof(uint32_t));
    ++sim_tx_count;
    uint64_t duration = 180000;
    for(unsigned i = 0; i < count; ++i)
        duration += timings[i];
    sim_tick += duration / 1000;
    if(sim_tx_hook) sim_tx_hook();
}
static inline bool storage_simply_mkdir(Storage* storage, const char* path) {
    UNUSED(storage);
    return !sim_sd_fail && (mkdir(path, 0700) == 0 || errno == EEXIST);
}
static inline FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    UNUSED(storage);
    if(sim_sd_fail) return FSE_ERROR;
    if(mkdir(path, 0700) == 0) return FSE_OK;
    return errno == EEXIST ? FSE_EXIST : FSE_ERROR;
}
static inline File* storage_file_alloc(Storage* storage) {
    UNUSED(storage);
    return calloc(1, sizeof(File));
}
static inline bool
    storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    UNUSED(access);
    if(sim_sd_fail) return false;
    file->fp = fopen(
        path,
        mode == FSOM_CREATE_NEW    ? "wx" :
        mode == FSOM_CREATE_ALWAYS ? "w" :
                                     "a");
    return file->fp != NULL;
}
static inline size_t storage_file_write(File* file, const void* data, size_t size) {
    if(sim_write_hook) sim_write_hook();
    return sim_sd_fail || !file->fp ? 0 : fwrite(data, 1, size, file->fp);
}
static inline bool storage_file_sync(File* file) {
    return !sim_sync_fail && file->fp && fflush(file->fp) == 0;
}
static inline bool storage_file_close(File* file) {
    bool ok = !file->fp || fclose(file->fp) == 0;
    file->fp = NULL;
    return ok;
}
static inline void storage_file_free(File* file) {
    assert(!file->fp);
    free(file);
}
static inline FS_Error storage_common_stat(Storage* storage, const char* path, void* info) {
    UNUSED(storage);
    UNUSED(info);
    struct stat st;
    if(sim_sd_fail) return FSE_ERROR;
    if(stat(path, &st) == 0) return FSE_OK;
    return errno == ENOENT || errno == ENOTDIR ? FSE_NOT_EXIST : FSE_ERROR;
}
static inline FS_Error storage_common_rename(Storage* storage, const char* from, const char* to) {
    UNUSED(storage);
    if(strstr(to, "/profile.meta") && sim_meta_rename_fail_after >= 0) {
        if(!sim_meta_rename_fail_after) return FSE_ERROR;
        --sim_meta_rename_fail_after;
    }
    return !sim_sd_fail && !sim_rename_fail && rename(from, to) == 0 ? FSE_OK : FSE_ERROR;
}
static inline FS_Error storage_common_remove(Storage* storage, const char* path) {
    UNUSED(storage);
    return !sim_sd_fail && !sim_remove_fail && remove(path) == 0 ? FSE_OK : FSE_ERROR;
}
static inline FlipperFormat* flipper_format_file_alloc(Storage* storage) {
    UNUSED(storage);
    return calloc(1, sizeof(FlipperFormat));
}
static inline bool flipper_format_file_open_new(FlipperFormat* ff, const char* path) {
    if(sim_sd_fail) return false;
    ff->fp = fopen(path, "wx");
    return ff->fp != NULL;
}
static inline bool flipper_format_file_open_existing(FlipperFormat* ff, const char* path) {
    if(sim_sd_fail) return false;
    ff->fp = fopen(path, "r");
    return ff->fp != NULL;
}
static inline bool flipper_format_rewind(FlipperFormat* ff) {
    return !sim_sd_fail && ff->fp && fseek(ff->fp, 0, SEEK_SET) == 0;
}
static inline bool sim_format_read(
    FlipperFormat* ff,
    const char* key,
    void* values,
    uint16_t count,
    bool floating) {
    if(sim_sd_fail || !ff->fp) return false;
    char* line = malloc(16384);
    bool ok = false;
    rewind(ff->fp);
    while(fgets(line, 16384, ff->fp)) {
        size_t length = strlen(key);
        if(strncmp(line, key, length) || line[length] != ':') continue;
        char* p = line + length + 1;
        ok = true;
        for(unsigned n = 0; n < count; ++n) {
            char* end;
            if(floating)
                ((float*)values)[n] = strtof(p, &end);
            else {
                unsigned long value = strtoul(p, &end, 10);
                if(value > UINT32_MAX) ok = false;
                ((uint32_t*)values)[n] = value;
            }
            if(end == p) {
                ok = false;
                break;
            }
            p = end;
        }
        break;
    }
    free(line);
    return ok;
}
static inline bool flipper_format_read_uint32(
    FlipperFormat* ff,
    const char* key,
    uint32_t* values,
    uint16_t count) {
    return sim_format_read(ff, key, values, count, false);
}
static inline bool
    flipper_format_get_value_count(FlipperFormat* ff, const char* key, uint32_t* count) {
    if(sim_sd_fail || !ff->fp) return false;
    char* line = malloc(16384);
    bool ok = false;
    rewind(ff->fp);
    while(fgets(line, 16384, ff->fp)) {
        size_t length = strlen(key);
        if(strncmp(line, key, length) || line[length] != ':') continue;
        char* p = line + length + 1;
        *count = 0;
        while(*p) {
            char* end;
            strtoul(p, &end, 10);
            if(end == p) break;
            ++*count;
            p = end;
        }
        ok = true;
        break;
    }
    free(line);
    return ok;
}
static inline bool
    flipper_format_read_float(FlipperFormat* ff, const char* key, float* values, uint16_t count) {
    return sim_format_read(ff, key, values, count, true);
}
static inline bool
    flipper_format_write_header_cstr(FlipperFormat* ff, const char* type, uint32_t version) {
    return !sim_sd_fail && ff->fp &&
           fprintf(ff->fp, "Filetype: %s\nVersion: %u\n", type, version) > 0;
}
static inline bool
    flipper_format_write_string_cstr(FlipperFormat* ff, const char* key, const char* value) {
    return !sim_sd_fail && ff->fp && fprintf(ff->fp, "%s: %s\n", key, value) > 0;
}
static inline bool flipper_format_write_hex(
    FlipperFormat* ff,
    const char* key,
    const uint8_t* values,
    uint16_t count) {
    if(sim_sd_fail || !ff->fp) return false;
    fprintf(ff->fp, "%s:", key);
    for(unsigned i = 0; i < count; ++i)
        fprintf(ff->fp, " %02X", values[i]);
    return fputc('\n', ff->fp) != EOF;
}
static inline bool flipper_format_write_uint32(
    FlipperFormat* ff,
    const char* key,
    const uint32_t* values,
    uint16_t count) {
    if(sim_sd_fail || !ff->fp) return false;
    fprintf(ff->fp, "%s:", key);
    for(unsigned i = 0; i < count; ++i)
        fprintf(ff->fp, " %u", values[i]);
    return fputc('\n', ff->fp) != EOF;
}
static inline bool flipper_format_write_float(
    FlipperFormat* ff,
    const char* key,
    const float* values,
    uint16_t count) {
    if(sim_sd_fail || !ff->fp) return false;
    fprintf(ff->fp, "%s:", key);
    for(unsigned i = 0; i < count; ++i)
        fprintf(ff->fp, " %.6f", (double)values[i]);
    return fputc('\n', ff->fp) != EOF;
}
static inline bool flipper_format_file_close(FlipperFormat* ff) {
    bool ok = !ff->fp || fclose(ff->fp) == 0;
    ff->fp = NULL;
    return ok;
}
static inline void flipper_format_free(FlipperFormat* ff) {
    assert(!ff->fp);
    free(ff);
}
