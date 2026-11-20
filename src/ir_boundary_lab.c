#include "ir_lab_core.h"
#include "ir_lab_protocol.h"
#include "ir_lab_profile.h"

#ifdef IR_LAB_HOST_SIM
#include "tests/host_platform.h"
#else
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <infrared.h>
#include <infrared_worker.h>
#include <infrared_transmit.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAB_ROOT     EXT_PATH("apps_data/ir_boundary_lab")
#define LAB_PROFILES LAB_ROOT "/profiles"
#define LAB_LINES    7
#define LAB_SETS     6

typedef enum {
    LabHomeCapture,
    LabHomeSniff,
    LabHomeProfiles,
    LabHomeFuzzer,
    LabHomeSets,
    LabHomeAnalyze,
    LabHomeSettings,
} LabHomeItem;

typedef enum {
    LabRangeFrom,
    LabRangeTo,
    LabRangeSteps,
    LabRangeStart,
    LabRangePreset,
    LabRangeAnalyze,
    LabRangeDelay,
    LabRangeMore,
} LabRangeItem;

typedef enum {
    LabHome,
    LabCapture,
    LabSeries,
    LabSlots,
    LabSlotAction,
    LabCompare,
    LabCompareAction,
    LabPattern,
    LabHex,
    LabSignal,
    LabMore,
    LabFrames,
    LabRange,
    LabFuzzPresets,
    LabSniff,
    LabSniffLabel,
    LabProfiles,
    LabProfileSignals,
    LabProfileSignal,
    LabProfileMaker,
    LabRangeValue,
    LabFuzzPick,
    LabFuzzBytes,
    LabFuzzByte,
    LabFuzzOptions,
    LabRangeRun,
    LabSend,
    LabResult,
    LabSettings,
    LabCarrier,
    LabHelp,
    LabInfo,
    LabProtocol,
    LabProtocolActions,
    LabProtocolTiming,
    LabProtocolChecks,
    LabProtocolFields,
    LabProtocolField,
    LabMessage,
} LabScreen;

typedef struct {
    uint16_t count;
    uint32_t frequency;
    float duty_cycle;
    uint32_t seed_id;
    uint8_t changes[IR_LAB_MASK_BYTES];
    uint8_t frame, byte;
} LabCaptureSlot;

typedef struct {
    LabCaptureSlot slots[IR_LAB_MAX_CAPTURES];
    IrLabDataView views[IR_LAB_MAX_CAPTURES];
    uint8_t active_slot, compare_frame, compare_byte;
} LabCaptureSet;

static const char* const lab_set_names[LAB_SETS] =
    {"Pressure", "Water temp", "Seat temp", "Custom", "Privacy sound", "Flush"};

static unsigned lab_set_capacity(unsigned series) {
    return series < 4 ? IR_LAB_MAX_CAPTURES : series < LAB_SETS ? 2 : 0;
}

static const char* lab_slot_role(unsigned series, unsigned slot) {
    if(series == 4) return slot == 0 ? "Start" : "Stop";
    if(series == 5) return slot == 0 ? "Big flush" : "Small flush";
    return "Capture";
}

typedef struct {
    Gui* gui;
    Storage* storage;
    ViewPort* view;
    FuriMessageQueue* inputs;
    FuriMutex* mutex;
    InfraredWorker* worker;
    IrLabSignal seed;
    IrLabSignal stop;
    IrLabSignal received;
    IrLabSignal candidate;
    IrLabSignal waveform;
    IrLabMutation mutation;
    IrLabGate gate;
    LabScreen screen;
    LabScreen message_return;
    LabScreen capture_return;
    IrLabBits bits;
    uint8_t changes[IR_LAB_MASK_BYTES];
    uint8_t frame;
    uint8_t byte;
    uint16_t range_index;
    uint8_t range_field;
    int16_t range_edit;
    bool range_edit_negative;
    uint8_t range_nibble;
    uint8_t range_frame;
    uint8_t range_byte;
    uint8_t range_base[IR_LAB_MASK_BYTES];
    bool range_running;
    bool range_pending;
    bool range_simulation;
    bool range_live;
    bool range_repeat;
    IrLabByteRule rules[IR_LAB_MAX_BYTE_RULES];
    uint8_t rule_count, rule_selected;
    bool fuzz_pick_end;
    uint32_t end_seed_id;
    uint8_t fuzz_offset;
    bool fuzz_keep_check;
    IrLabProtocolKind fuzz_preset;
    IrLabProfile profile;
    bool profile_open, capture_sniff;
    LabScreen profile_maker_return;
    bool seed_from_profile;
    uint8_t seed_profile_id, seed_profile_index, seed_profile_action, seed_profile_maker;
    uint32_t seed_profile_file_id;
    uint8_t profile_ids[IR_LAB_MAX_PROFILES], profile_makers[IR_LAB_MAX_PROFILES];
    uint8_t profile_count, profile_selected;
    uint8_t sniff_label, sniff_guess;
    IrLabProtocolAnalysis protocol_analysis;
    IrLabDataView protocol_views[IR_LAB_MAX_CAPTURES];
    uint8_t protocol_view_count, protocol_frame, protocol_byte;
    LabScreen protocol_return;
    bool auto_started, auto_paused, auto_complete, auto_has_sent, auto_waiting;
    bool auto_save_waveform, auto_save_note;
    uint16_t auto_last_index;
    uint32_t auto_sent_count;
    uint8_t auto_last_value;
    uint8_t auto_last_changes[IR_LAB_MASK_BYTES];
    uint32_t auto_last_trial, auto_wait_since, last_emitted_trial;
    char auto_saved_name[32], auto_reason[40];
    bool preview_paused;
    bool preview_complete;
    bool preview_press_consumed;
    bool preview_active; // Atomic: input callback may request an immediate pause.
    bool preview_stop_requested; // Atomic, checked before advancing a candidate.
    uint8_t preview_delay;
    uint32_t preview_last_ms;
    LabCaptureSet sets[LAB_SETS];
    LabCaptureSlot* slots;
    IrLabDataView* views;
    IrLabComparison comparison;
    uint8_t active_slot;
    uint8_t capture_slot;
    uint8_t selected_slot;
    uint8_t series;
    uint8_t compare_frame;
    uint8_t compare_byte;
    bool capture_batch;
    LabScreen drawn_screen;
    uint16_t drawn_row;
    char hex_cells[8][3];
    uint8_t hex_count, hex_selected;
    uint16_t menu_first;
    uint16_t menu_count;
    int8_t highlighted;
    char message[4][40];
    uint16_t row;
    bool running;
    bool rx_running;
    bool rx_done; // Protected by mutex; the worker copies only the first signal.
    bool capture_stop;
    bool storage_ready;
    bool stop_requested; // Access only through atomic operations.
    uint32_t rx_started;
    uint32_t carrier;
    uint32_t capture_id;
    uint32_t seed_id;
    uint32_t stop_id;
    char directory[100];
    char log_path[120];
    char status[32];
    char lines[LAB_LINES][40]; // Drawing reads only these, under mutex.
} IrLabApp;

static uint32_t lab_now_ms(void) {
    return ((uint64_t)furi_get_tick() * 1000) / furi_kernel_get_tick_frequency();
}

static void lab_status(IrLabApp* app, const char* text) {
    snprintf(app->status, sizeof(app->status), "%s", text);
}

static bool lab_log(IrLabApp* app, const char* event, const char* detail) {
    if(!app->storage_ready) return false;
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, app->log_path, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        char line[256];
        int n = snprintf(
            line,
            sizeof(line),
            "%lu,%lu,%lu,%lu,%lu,%s,%u,%u,%ld,%s,%s\n",
            (unsigned long)furi_hal_rtc_get_timestamp(),
            (unsigned long)lab_now_ms(),
            (unsigned long)app->gate.trials,
            (unsigned long)app->seed_id,
            (unsigned long)app->stop_id,
            ir_lab_mode_name(app->mutation.mode),
            app->mutation.index,
            app->mutation.width,
            (long)app->mutation.value,
            event,
            detail);
        ok = n > 0 && (size_t)n < sizeof(line) && storage_file_write(file, line, n) == (size_t)n &&
             storage_file_sync(file);
    }
    bool closed = storage_file_close(file);
    storage_file_free(file);
    ok = ok && closed;
    if(!ok) {
        app->storage_ready = false;
        ir_lab_stop(&app->gate);
        lab_status(app, "SD error: tests locked");
    }
    return ok;
}

static bool lab_storage_init(IrLabApp* app) {
    if(!storage_simply_mkdir(app->storage, EXT_PATH("apps_data")) ||
       !storage_simply_mkdir(app->storage, LAB_ROOT))
        return false;
    // Never reuse a prior run's evidence directory, even if the clock was reset.
    for(unsigned suffix = 0; suffix < 100; ++suffix) {
        snprintf(
            app->directory,
            sizeof(app->directory),
            "%s/%08lx-%08lx-%02u",
            LAB_ROOT,
            (unsigned long)furi_hal_rtc_get_timestamp(),
            (unsigned long)furi_get_tick(),
            suffix);
        if(storage_common_mkdir(app->storage, app->directory) == FSE_OK) break;
        if(suffix == 99) return false;
    }
    snprintf(app->log_path, sizeof(app->log_path), "%s/trials.csv", app->directory);
    File* file = storage_file_alloc(app->storage);
    static const char header[] =
        "rtc_seconds,uptime_ms,trial,seed_id,stop_id,mode,index,width,value,event,detail\n";
    bool ok = storage_file_open(file, app->log_path, FSAM_WRITE, FSOM_CREATE_NEW);
    if(ok) {
        ok = storage_file_write(file, header, sizeof(header) - 1) == sizeof(header) - 1 &&
             storage_file_sync(file);
    }
    bool closed = storage_file_close(file);
    storage_file_free(file);
    return ok && closed;
}

static bool lab_save_signal(IrLabApp* app, const IrLabSignal* signal, const char* name) {
    if(!app->storage_ready || !ir_lab_signal_valid(signal)) return false;
    char path[140];
    snprintf(path, sizeof(path), "%s/%s.ir", app->directory, name);
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    bool ok = flipper_format_file_open_new(ff, path) &&
              flipper_format_write_header_cstr(ff, "IR signals file", 1) &&
              flipper_format_write_string_cstr(ff, "name", name);
    if(signal->decoded) {
        // Flipper .ir hex address/command values are explicitly little endian.
        uint8_t address[4], command[4];
        for(size_t i = 0; i < 4; ++i) {
            address[i] = signal->address >> (8 * i);
            command[i] = signal->command >> (8 * i);
        }
        ok = ok && flipper_format_write_string_cstr(ff, "type", "parsed") &&
             flipper_format_write_string_cstr(
                 ff, "protocol", infrared_get_protocol_name(signal->protocol)) &&
             flipper_format_write_hex(ff, "address", address, 4) &&
             flipper_format_write_hex(ff, "command", command, 4);
    } else {
        ok = ok && flipper_format_write_string_cstr(ff, "type", "raw") &&
             flipper_format_write_uint32(ff, "frequency", &signal->frequency, 1) &&
             flipper_format_write_float(ff, "duty_cycle", &signal->duty_cycle, 1) &&
             flipper_format_write_uint32(ff, "data", signal->timings, signal->count);
    }
    bool closed = flipper_format_file_close(ff);
    flipper_format_free(ff);
    return ok && closed;
}

static void lab_open_send(IrLabApp* app);
static void lab_refresh(IrLabApp* app);
static void lab_auto_pause(IrLabApp* app, const char* reason, bool complete);

static void lab_go(IrLabApp* app, LabScreen screen, uint16_t row) {
    app->screen = screen;
    app->row = row;
}

static void lab_message(
    IrLabApp* app,
    LabScreen back,
    const char* title,
    const char* line1,
    const char* line2,
    const char* line3) {
    snprintf(app->message[0], 40, "%s", title);
    snprintf(app->message[1], 40, "%s", line1);
    snprintf(app->message[2], 40, "%s", line2);
    snprintf(app->message[3], 40, "%s", line3);
    app->message_return = back;
    lab_go(app, LabMessage, 0);
}

static bool lab_candidate(IrLabApp* app) {
    if(app->mutation.mode == IrLabReplay) {
        app->candidate = app->seed;
        return ir_lab_signal_valid(&app->candidate);
    }
    return ir_lab_apply_bits(&app->seed, &app->bits, app->changes, &app->candidate);
}

static unsigned lab_byte_value(const IrLabApp* app, bool edited, unsigned frame, unsigned byte) {
    return ir_lab_byte_value(&app->bits, edited ? app->changes : NULL, frame, byte);
}

static unsigned lab_byte_max(const IrLabApp* app) {
    unsigned width = ir_lab_byte_width(&app->bits, app->frame, app->byte);
    return width ? (1U << width) - 1 : 0;
}

static void lab_set_hex(IrLabApp* app, unsigned value) {
    if(ir_lab_set_byte_linked(&app->bits, app->changes, app->frame, app->byte, value))
        app->mutation = (IrLabMutation){
            .mode = IrLabDataBits,
            .index = app->bits.start[app->frame] + app->byte * 8,
            .width = ir_lab_byte_width(&app->bits, app->frame, app->byte),
            .value = value};
}

static void lab_sets_init(IrLabApp* app) {
    app->slots = app->sets[app->series].slots;
    app->views = app->sets[app->series].views;
}

static void lab_slot_sync(IrLabApp* app) {
    LabCaptureSlot* slot = &app->slots[app->active_slot];
    if(slot->count && slot->seed_id == app->seed_id) {
        memcpy(slot->changes, app->changes, sizeof(slot->changes));
        slot->frame = app->frame;
        slot->byte = app->byte;
    }
}

static bool lab_slot_store(IrLabApp* app, unsigned index, const IrLabSignal* signal, uint32_t id) {
    if(index >= lab_set_capacity(app->series) || !ir_lab_signal_valid(signal) || signal->decoded)
        return false;
    // The immutable seed was saved before this call. Keep only its small index
    // and edits in RAM, so all capture sets can hold maximum-size originals.
    LabCaptureSlot* slot = &app->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->count = signal->count;
    slot->frequency = signal->frequency;
    slot->duty_cycle = signal->duty_cycle;
    slot->seed_id = id;
    return true;
}

static bool lab_slot_read(IrLabApp* app, const LabCaptureSlot* slot, IrLabSignal* signal) {
    if(!slot->count || slot->count > IR_LAB_MAX_TIMINGS) return false;
    char path[140];
    snprintf(path, sizeof(path), "%s/seed-%03lu.ir", app->directory, (unsigned long)slot->seed_id);
    memset(signal, 0, sizeof(*signal));
    signal->valid = true;
    signal->count = slot->count;
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    bool ok = flipper_format_file_open_existing(ff, path) &&
              flipper_format_read_uint32(ff, "frequency", &signal->frequency, 1) &&
              flipper_format_read_float(ff, "duty_cycle", &signal->duty_cycle, 1) &&
              flipper_format_read_uint32(ff, "data", signal->timings, slot->count);
    bool closed = flipper_format_file_close(ff);
    flipper_format_free(ff);
    return ok && closed && ir_lab_signal_valid(signal) && signal->frequency == slot->frequency &&
           signal->duty_cycle == slot->duty_cycle;
}

static void lab_slot_restore(IrLabApp* app, unsigned index) {
    LabCaptureSlot* slot = &app->slots[index];
    app->seed = app->candidate;
    app->seed_id = slot->seed_id;
    app->seed_from_profile = false;
    app->active_slot = index;
    app->frame = slot->frame;
    app->byte = slot->byte;
    memcpy(app->changes, slot->changes, sizeof(app->changes));
    ir_lab_extract_bits(&app->seed, &app->bits);
    app->mutation = (IrLabMutation){.mode = IrLabDataBits};
    app->range_running = app->range_pending = app->range_simulation = app->range_live = false;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
}

static bool lab_slot_load(IrLabApp* app, unsigned index) {
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return false;
    }
    if(index >= lab_set_capacity(app->series) || !app->slots[index].count) return false;
    if(!lab_slot_read(app, &app->slots[index], &app->candidate)) {
        lab_message(
            app, LabSlots, "Cannot read capture", "Check the SD card", "Current signal kept", "");
        return false;
    }
    lab_slot_sync(app);
    lab_slot_restore(app, index);
    return true;
}

static unsigned lab_set_count(const LabCaptureSet* set) {
    unsigned count = 0;
    for(unsigned n = 0; n < IR_LAB_MAX_CAPTURES; ++n)
        count += set->slots[n].count != 0;
    return count;
}

static unsigned lab_slot_count(const IrLabApp* app) {
    return lab_set_count(&app->sets[app->series]);
}

static void lab_set_open(IrLabApp* app, unsigned series) {
    if(series >= LAB_SETS) return;
    if(series == app->series) {
        lab_go(app, LabSlots, app->active_slot);
        return;
    }
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return;
    }
    lab_slot_sync(app);
    LabCaptureSet* old = &app->sets[app->series];
    old->active_slot = app->active_slot;
    old->compare_frame = app->compare_frame;
    old->compare_byte = app->compare_byte;
    LabCaptureSet* next = &app->sets[series];
    bool populated = lab_set_count(next) != 0;
    unsigned selected = next->active_slot;
    if(populated && !next->slots[selected].count) {
        for(selected = 0; selected < lab_set_capacity(series); ++selected)
            if(next->slots[selected].count) break;
    }
    // Read before switching: an SD error must leave the current set intact.
    if(populated && !lab_slot_read(app, &next->slots[selected], &app->candidate)) {
        lab_message(
            app,
            LabSeries,
            "Cannot open capture set",
            "Check the SD card",
            "All sets retained",
            "");
        return;
    }
    app->series = series;
    lab_sets_init(app);
    app->active_slot = app->capture_slot = app->selected_slot = selected;
    app->compare_frame = next->compare_frame;
    app->compare_byte = next->compare_byte;
    app->capture_batch = false;
    if(populated) {
        lab_slot_restore(app, selected);
    } else {
        memset(&app->seed, 0, sizeof(app->seed));
        memset(&app->bits, 0, sizeof(app->bits));
        memset(app->changes, 0, sizeof(app->changes));
        app->seed_id = 0;
        app->frame = app->byte = 0;
        app->mutation = (IrLabMutation){.mode = IrLabDataBits};
        app->range_running = app->range_pending = app->range_simulation = app->range_live = false;
        __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    }
    lab_go(app, LabSlots, selected);
}

static unsigned lab_comparison(IrLabApp* app) {
    return ir_lab_compare(
        app->views, lab_set_capacity(app->series), app->compare_frame, &app->comparison);
}

static void lab_open_compare_frame(IrLabApp* app) {
    unsigned count = lab_comparison(app);
    if(count < 2) {
        lab_message(
            app,
            LabSlots,
            "Need matching captures",
            "Capture at least 2",
            "Same data-frame layout",
            "Unknown formats excluded");
        return;
    }
    app->compare_byte = 0;
    for(unsigned bit = 0; bit < app->comparison.bits; ++bit)
        if(ir_lab_bit_get(app->comparison.varying, bit)) {
            app->compare_byte = bit / 8;
            break;
        }
    lab_go(app, LabCompare, app->active_slot);
}

// Start at the first differing frame, instead of showing a constant preamble.
static void lab_open_compare(IrLabApp* app) {
    unsigned first = IR_LAB_MAX_FRAMES;
    for(unsigned frame = 0; frame < IR_LAB_MAX_FRAMES; ++frame) {
        app->compare_frame = frame;
        if(lab_comparison(app) < 2) continue;
        if(first == IR_LAB_MAX_FRAMES) first = frame;
        if(app->comparison.changing_bits) {
            lab_open_compare_frame(app);
            return;
        }
    }
    app->compare_frame = first == IR_LAB_MAX_FRAMES ? 0 : first;
    lab_open_compare_frame(app);
}

static void lab_compare_pair(const IrLabApp* app, unsigned later, char* text) {
    int16_t delta;
    if(!ir_lab_hex_delta(
           &app->views[later - 1],
           &app->views[later],
           app->compare_frame,
           app->compare_byte,
           &delta)) {
        snprintf(text, 40, "%u-%u: unavailable", later + 1, later);
        return;
    }
    snprintf(
        text,
        40,
        "%u-%u: %02X-%02X=%c%02X",
        later + 1,
        later,
        ir_lab_view_byte(&app->views[later], app->compare_frame, app->compare_byte),
        ir_lab_view_byte(&app->views[later - 1], app->compare_frame, app->compare_byte),
        delta < 0 ? '-' : '+',
        delta < 0 ? -delta : delta);
}

static void lab_received(void* context, InfraredWorkerSignal* received) {
    IrLabApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(!app->rx_done) {
        IrLabSignal* signal = &app->received;
        memset(signal, 0, sizeof(*signal));
        if(!infrared_worker_signal_is_decoded(received)) {
            const uint32_t* timings;
            size_t count;
            infrared_worker_get_raw_signal(received, &timings, &count);
            if(count > 0 && count <= IR_LAB_MAX_TIMINGS) {
                memcpy(signal->timings, timings, count * sizeof(uint32_t));
                signal->count = count;
                signal->frequency = app->carrier;
                signal->duty_cycle = 0.33f;
                signal->valid = true;
            }
        }
        signal->valid = ir_lab_signal_valid(signal);
        app->rx_done = true;
    }
    furi_mutex_release(app->mutex);
}

static void lab_rx_stop(IrLabApp* app) {
    if(app->rx_running) {
        infrared_worker_rx_stop(app->worker);
        app->rx_running = false;
    }
}

static void lab_capture_start(IrLabApp* app, bool stop) {
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return;
    }
    lab_slot_sync(app);
    app->range_live = false;
    app->range_simulation = false;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    app->gate.armed = false;
    app->capture_stop = stop;
    app->capture_sniff = !stop && app->screen == LabSniff;
    app->capture_return = app->screen;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->rx_done = false;
    furi_mutex_release(app->mutex);
    infrared_worker_rx_enable_signal_decoding(app->worker, false);
    lab_go(app, LabCapture, 0);
    app->rx_started = lab_now_ms();
    app->rx_running = true;
    infrared_worker_rx_start(app->worker);
}

static void lab_capture_poll(IrLabApp* app) {
    if(!app->rx_running) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool done = app->rx_done;
    furi_mutex_release(app->mutex);
    if(!done && (uint32_t)(lab_now_ms() - app->rx_started) < 15000) return;
    lab_rx_stop(app);
    if(!done || !app->received.valid) {
        lab_message(
            app,
            app->capture_return,
            done ? "Capture rejected" : "Nothing received",
            done ? "Size or timing limit" : "No signal within 15s",
            "Tap remote once",
            "Original kept");
        return;
    }
    if(app->capture_sniff) {
        IrLabBits* bits = malloc(sizeof(*bits));
        IrLabProtocolAnalysis guess = {0};
        if(bits && ir_lab_extract_bits(&app->received, bits))
            ir_lab_protocol_analyze(&app->received, bits, &guess);
        free(bits);
        app->sniff_guess = guess.kind;
        lab_go(app, LabSniffLabel, app->sniff_label);
        return;
    }
    char name[32];
    ++app->capture_id;
    snprintf(
        name,
        sizeof(name),
        "%s-%03lu",
        app->capture_stop ? "stop" : "seed",
        (unsigned long)app->capture_id);
    if(!lab_save_signal(app, &app->received, name)) {
        app->storage_ready = false;
        ir_lab_stop(&app->gate);
        lab_message(
            app, LabHome, "Save failed", "Check SD card", "Sending is locked", "Original kept");
        return;
    }
    if(app->capture_stop) {
        app->stop = app->received;
        app->stop_id = app->capture_id;
    } else {
        if(!lab_slot_store(app, app->capture_slot, &app->received, app->capture_id)) {
            lab_message(
                app,
                LabSlots,
                "Cannot keep capture",
                "Other captures retained",
                "Try a smaller capture",
                "File saved on SD");
            return;
        }
        app->seed = app->received;
        app->seed_from_profile = false;
        app->seed_id = app->capture_id;
        app->active_slot = app->capture_slot;
        app->frame = app->byte = 0;
        app->range_running = app->range_pending = false;
        memset(app->changes, 0, sizeof(app->changes));
        ir_lab_extract_bits(&app->seed, &app->bits);
        ir_lab_data_view(&app->bits, &app->views[app->active_slot]);
        app->mutation = (IrLabMutation){.mode = IrLabDataBits, .width = 1};
    }
    if(lab_log(app, "capture", name)) {
        if(app->capture_stop) {
            lab_message(
                app,
                LabSettings,
                "STOP saved",
                "Original kept unchanged",
                "Hold Back: STOP + lock",
                "Verify on your bench");
        } else {
            char slot_detail[80];
            snprintf(
                slot_detail,
                sizeof(slot_detail),
                "slot=%u series=%u seed=%lu role=%s",
                app->capture_slot + 1,
                app->series,
                (unsigned long)app->seed_id,
                lab_slot_role(app->series, app->capture_slot));
            if(!lab_log(app, "capture_slot", slot_detail)) {
                lab_message(
                    app,
                    LabSlots,
                    "SD log failed",
                    "Captured data retained",
                    "Live sending locked",
                    "");
                return;
            }
            if(app->capture_batch) {
                unsigned next = app->capture_slot;
                unsigned capacity = lab_set_capacity(app->series);
                for(unsigned n = 1; n <= capacity; ++n)
                    if(!app->slots[(app->capture_slot + n) % capacity].count) {
                        next = (app->capture_slot + n) % capacity;
                        break;
                    }
                lab_go(app, LabSlots, next);
                if(lab_slot_count(app) == capacity) lab_open_compare(app);
            } else
                lab_go(app, LabHex, 0);
            lab_status(app, "Signal saved");
        }
    } else
        lab_message(app, LabHome, "SD error", "Sending is locked", "Check the SD card", "");
    furi_message_queue_reset(app->inputs);
}

static void lab_transmit(const IrLabSignal* signal) {
    furi_check(!signal->decoded && ir_lab_signal_valid(signal));
    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    infrared_send_raw_ext(
        signal->timings, signal->count, true, signal->frequency, signal->duty_cycle);
}

static void lab_emergency_stop(IrLabApp* app) {
    if(app->range_live && app->auto_started) lab_auto_pause(app, "emergency_stop", false);
    app->range_live = false;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    if(app->range_simulation) {
        app->range_simulation = app->range_running = false;
        __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
        __atomic_store_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
        lab_message(
            app,
            LabRange,
            "Simulation stopped",
            "No IR was transmitted",
            "Current hex retained",
            "");
        return;
    }
    app->range_running = app->range_pending = false;
    lab_rx_stop(app);
    ir_lab_stop(&app->gate);
    if(ir_lab_signal_valid(&app->stop)) {
        lab_transmit(&app->stop);
        lab_log(app, "stop_sent", "acceptance_unverified");
        lab_message(
            app,
            LabHome,
            "STOP sent",
            "Sending is locked",
            "For the rest of this run",
            "Response not verified");
    } else {
        lab_log(app, "stop_missing", "tests_locked");
        lab_message(
            app, LabHome, "Run stopped", "Sending is locked", "No optional STOP saved", "");
    }
    furi_message_queue_reset(app->inputs);
}

static void lab_send_trial(IrLabApp* app) {
    if(app->range_simulation) {
        lab_message(
            app, LabRange, "Simulation only", "No automatic IR output", "Use manual send mode", "");
        return;
    }
    if(!app->storage_ready || !ir_lab_can_send(&app->gate, lab_now_ms())) {
        lab_message(
            app,
            LabHex,
            "Cannot send",
            !app->storage_ready                   ? "SD card unavailable" :
            app->gate.locked                      ? "Sending locked this run" :
            app->gate.pending_observation         ? "Record previous response" :
            app->gate.trials >= IR_LAB_MAX_TRIALS ? "Trial counter exhausted" :
                                                    "Wait for 10s countdown",
            "No test signal sent",
            "");
        return;
    }
    if(!lab_candidate(app)) {
        lab_message(
            app, LabHex, "Edit exceeds limits", "Try fewer changed bits", "No signal sent", "");
        return;
    }
    app->waveform = app->candidate;
    if(!ir_lab_reserve_trial(&app->gate, lab_now_ms())) return;
    char name[32], mask[IR_LAB_MASK_BYTES * 2 + 1];
    for(unsigned b = 0; b < IR_LAB_MASK_BYTES; ++b)
        snprintf(mask + b * 2, 3, "%02x", app->mutation.mode == IrLabReplay ? 0 : app->changes[b]);
    snprintf(name, sizeof(name), "trial-%03lu", (unsigned long)app->gate.trials);
    if(!lab_save_signal(app, &app->waveform, name) || !lab_log(app, "changed_bit_mask", mask) ||
       !lab_log(app, "tx_intent", name)) {
        ir_lab_stop(&app->gate);
        app->storage_ready = false;
        lab_message(app, LabHome, "SD error", "No test signal sent", "Sending is locked", "");
        return;
    }
    if(__atomic_load_n(&app->stop_requested, __ATOMIC_RELAXED)) return;
    lab_transmit(&app->waveform);
    app->range_pending = app->range_running;
    app->gate.last_ms = lab_now_ms();
    app->last_emitted_trial = app->gate.trials;
    lab_log(app, "tx_complete", "acceptance_unverified");
    lab_go(app, LabResult, 0);
    furi_message_queue_reset(app->inputs);
}

static void lab_open_send(IrLabApp* app) {
    if(!app->seed.valid) {
        lab_message(
            app, LabHome, "Capture first", "Choose Capture signal", "Press remote button", "");
        return;
    }
    if(!app->storage_ready || app->gate.locked || app->gate.trials >= IR_LAB_MAX_TRIALS) {
        lab_message(
            app,
            LabHex,
            "Cannot send",
            !app->storage_ready ? "SD card unavailable" :
            app->gate.locked    ? "Sending locked this run" :
                                  "Trial counter exhausted",
            "No signal sent",
            "");
        return;
    }
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return;
    }
    if(!app->range_running && app->gate.has_sent &&
       (uint32_t)(lab_now_ms() - app->gate.last_ms) < IR_LAB_COOLDOWN_MS) {
        char wait[32];
        snprintf(
            wait,
            sizeof(wait),
            "Wait %lu seconds",
            (unsigned long)((IR_LAB_COOLDOWN_MS - (uint32_t)(lab_now_ms() - app->gate.last_ms) +
                             999) /
                            1000));
        lab_message(
            app, LabHex, "Wait before next test", wait, "Your edits are kept", "No signal sent");
        return;
    }
    if(!lab_candidate(app)) {
        lab_message(
            app,
            LabHex,
            "Cannot send this edit",
            "Encoding or size limit",
            "Try fewer changed bits",
            "No signal sent");
        return;
    }
    lab_go(app, app->range_running ? LabRangeRun : LabSend, 0);
}

static bool lab_editable(IrLabApp* app) {
    if(app->bits.valid) return true;
    lab_message(
        app,
        LabHex,
        "No editable hex data",
        ir_lab_bits_error(&app->bits),
        "OK menu: replay original",
        "Try another capture");
    return false;
}

static unsigned lab_range_count(const IrLabApp* app) {
    return ir_lab_byte_plan_count(&app->bits, app->rules, app->rule_count);
}

static const char* lab_fuzz_preset_name(const IrLabApp* app) {
    return app->fuzz_preset == IrLabProtocolToto ? "TOTO" :
           app->fuzz_preset == IrLabProtocolInax ? "INAX" :
                                                   "Captured bytes";
}

static bool lab_open_fuzzer_preset(IrLabApp* app, IrLabProtocolKind preset) {
    if(!lab_editable(app)) return false;
    ir_lab_protocol_analyze(&app->seed, &app->bits, &app->protocol_analysis);
    if(preset != IrLabProtocolUnknown &&
       ((preset != IrLabProtocolToto && preset != IrLabProtocolInax) ||
        app->protocol_analysis.kind != preset)) {
        lab_message(
            app,
            LabFuzzPresets,
            "Preset does not fit",
            "Capture a matching signal",
            "Current rules kept",
            "No IR sent");
        return false;
    }
    app->fuzz_preset = preset;
    app->fuzz_offset = preset == IrLabProtocolToto ? 15 : 0;
    app->fuzz_keep_check =
        preset != IrLabProtocolUnknown && app->protocol_analysis.checks_tested &&
        app->protocol_analysis.checks_passed == app->protocol_analysis.checks_tested;
    unsigned fixed_bits = preset == IrLabProtocolInax ? 8 : app->fuzz_offset;
    if(fixed_bits) {
        // Payload mode retains the captured prefix, including after earlier raw edits.
        for(unsigned frame = 0; frame < app->bits.frames; ++frame)
            for(unsigned bit = 0; bit < fixed_bits; ++bit) {
                unsigned index = app->bits.start[frame] + bit;
                app->changes[index / 8] &= (uint8_t) ~(1U << (index % 8));
            }
    }
    app->range_repeat = false;
    app->end_seed_id = 0;
    app->rule_count = app->rule_selected = 0;
    for(unsigned frame = 0; frame < app->bits.frames; ++frame) {
        for(unsigned byte = 0; ir_lab_window_width(&app->bits, frame, app->fuzz_offset, byte);
            ++byte) {
            if(preset == IrLabProtocolInax && byte == 0) continue;
            bool alias = false;
            for(unsigned n = 0; n < app->rule_count; ++n)
                alias |= ir_lab_byte_targets_overlap(
                    &app->bits, frame, byte, app->rules[n].frame, app->rules[n].byte);
            if(alias) continue;
            if(app->rule_count >= IR_LAB_MAX_BYTE_RULES) return false;
            unsigned value =
                ir_lab_window_value(&app->bits, app->changes, frame, app->fuzz_offset, byte);
            unsigned n = app->rule_count++;
            app->rules[n] = (IrLabByteRule){
                .frame = frame,
                .byte = byte,
                .from = value,
                .to = value,
                .bit_offset = app->fuzz_offset};
            if(frame == app->frame && byte == app->byte) app->rule_selected = n;
        }
    }
    lab_go(app, LabRange, LabRangeFrom);
    return true;
}

static void lab_open_fuzzer_mode(IrLabApp* app, unsigned offset) {
    lab_open_fuzzer_preset(app, offset == 15 ? IrLabProtocolToto : IrLabProtocolUnknown);
}

static void lab_use_protocol_preset(IrLabApp* app) {
    IrLabProtocolKind kind = app->protocol_analysis.kind;
    lab_open_fuzzer_preset(
        app, kind == IrLabProtocolToto || kind == IrLabProtocolInax ? kind : IrLabProtocolUnknown);
}

static void lab_open_fuzzer(IrLabApp* app) {
    lab_open_fuzzer_mode(app, 0);
}

static void lab_profile_name(char* text, size_t size, unsigned maker, unsigned id) {
    snprintf(text, size, "%s %03u", ir_lab_maker_name(maker), id);
}

static bool
    lab_set_profile_source(void* context, unsigned index, IrLabSignal* signal, unsigned* action) {
    IrLabApp* app = context;
    for(unsigned slot = 0; slot < lab_set_capacity(app->series); ++slot) {
        if(!app->slots[slot].count) continue;
        if(index) {
            --index;
            continue;
        }
        *action = app->series == 0 ? IrLabActionPressure :
                  app->series == 1 ? IrLabActionWaterTemp :
                  app->series == 2 ? IrLabActionSeatTemp :
                  app->series == 4 ? (slot == 0 ? IrLabActionMusicStart : IrLabActionMusicStop) :
                  app->series == 5 ? (slot == 0 ? IrLabActionFlushBig : IrLabActionFlushSmall) :
                                     IrLabActionOther;
        return lab_slot_read(app, &app->slots[slot], signal);
    }
    return false;
}

static void lab_set_profile_begin(IrLabApp* app) {
    if(!lab_slot_count(app)) {
        lab_message(app, app->screen, "No captures to save", "Capture a signal first", "", "");
        return;
    }
    app->profile_maker_return = app->screen;
    lab_go(app, LabProfileMaker, IrLabMakerUnknown);
}

static void lab_set_profile_save(IrLabApp* app, unsigned maker) {
    IrLabProfile* saved = malloc(sizeof(*saved));
    bool ok = saved && ir_lab_profile_import(
                           app->storage,
                           LAB_PROFILES,
                           lab_slot_count(app),
                           maker,
                           lab_set_profile_source,
                           app,
                           saved);
    if(ok) {
        char title[40], count[40];
        snprintf(title, sizeof(title), "Saved %s %03u", ir_lab_maker_name(maker), saved->id);
        snprintf(count, sizeof(count), "%u signals on SD", saved->count);
        lab_message(app, app->profile_maker_return, title, count, "Load profile to reuse", "");
    } else
        lab_message(
            app, LabProfileMaker, "Profile save failed", "Captures kept", "Check SD and retry", "");
    free(saved);
}

static void lab_sniff_new(IrLabApp* app) {
    memset(&app->profile, 0, sizeof(app->profile));
    app->profile_open = true;
    lab_go(app, LabSniff, 0);
}

static void lab_sniff_save(IrLabApp* app, unsigned action) {
    if(!app->profile.id && !ir_lab_profile_create(app->storage, LAB_PROFILES, &app->profile)) {
        lab_message(
            app,
            LabSniffLabel,
            "Cannot create profile",
            "Check SD / profile limit",
            "Capture kept for retry",
            "No IR sent");
        return;
    }
    if(!ir_lab_profile_append(app->storage, LAB_PROFILES, &app->profile, &app->received, action)) {
        lab_message(
            app,
            LabSniffLabel,
            "Profile save failed",
            "Capture kept for retry",
            "Earlier signals kept",
            "Check SD card");
        return;
    }
    app->sniff_label = action;
    lab_go(app, LabSniff, 0);
}

static void lab_profiles_open(IrLabApp* app) {
    app->profile_count =
        ir_lab_profile_list(app->storage, LAB_PROFILES, app->profile_ids, app->profile_makers);
    if(app->profile_count)
        lab_go(app, LabProfiles, 0);
    else
        lab_message(
            app,
            LabHome,
            "No saved profiles",
            "Sniff mode creates one",
            "Check SD if expected",
            "");
}

static void lab_profile_load(IrLabApp* app, unsigned id) {
    if(!ir_lab_profile_load(app->storage, LAB_PROFILES, id, &app->profile)) {
        lab_message(
            app,
            LabProfiles,
            "Cannot load profile",
            "Missing or damaged signal",
            "Current profile kept",
            "Check SD card");
        return;
    }
    app->profile_open = true;
    lab_go(app, LabProfileSignals, 0);
}

static void lab_profile_dedup(IrLabApp* app) {
    if(!app->profile.count) {
        lab_message(app, LabSniff, "No saved signals", "Capture a button first", "", "");
        return;
    }
    unsigned removed = 0;
    bool pending = false;
    bool ok =
        ir_lab_profile_deduplicate(app->storage, LAB_PROFILES, &app->profile, &removed, &pending);
    if(!ok) {
        lab_message(
            app,
            LabSniff,
            "Cleanup incomplete",
            "Saved signals kept",
            pending ? "SD cleanup pending" : "Check profile / SD",
            "Try again");
        return;
    }
    if(app->seed_from_profile && app->seed_profile_id == app->profile.id)
        for(unsigned i = 0; i < app->profile.count; ++i)
            if(app->profile.entries[i].file_id == app->seed_profile_file_id)
                app->profile_selected = app->seed_profile_index = i;
    char title[40];
    snprintf(title, sizeof(title), "Removed %u duplicate%s", removed, removed == 1 ? "" : "s");
    lab_message(
        app,
        LabSniff,
        removed ? title : "No duplicates found",
        "Kept first match",
        pending ? "SD cleanup: retry" : "Profile saved on SD",
        "Other signals kept");
}

static bool lab_profile_activate(IrLabApp* app, unsigned index) {
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return false;
    }
    if(!ir_lab_profile_read(app->storage, LAB_PROFILES, &app->profile, index, &app->candidate)) {
        lab_message(
            app,
            LabProfileSignals,
            "Cannot read signal",
            "Current signal kept",
            "Check profile / SD",
            "");
        return false;
    }
    if(app->capture_id == UINT32_MAX) return false;
    char name[32];
    snprintf(name, sizeof(name), "seed-%03lu", (unsigned long)++app->capture_id);
    if(!lab_save_signal(app, &app->candidate, name)) {
        lab_message(
            app,
            LabProfileSignals,
            "Cannot prepare signal",
            "Current signal kept",
            "Check session SD",
            "");
        return false;
    }
    lab_slot_sync(app);
    // Reuse the normal current-signal path; the saved profile remains immutable.
    app->sets[app->series].active_slot = app->active_slot;
    app->sets[app->series].compare_frame = app->compare_frame;
    app->sets[app->series].compare_byte = app->compare_byte;
    app->series = 3;
    lab_sets_init(app);
    // Loaded profiles never replace a slot in the comparison banks.
    app->active_slot = app->sets[3].active_slot;
    app->seed = app->candidate;
    app->seed_id = app->capture_id;
    app->frame = app->byte = 0;
    memset(app->changes, 0, sizeof(app->changes));
    ir_lab_extract_bits(&app->seed, &app->bits);
    app->mutation = (IrLabMutation){.mode = IrLabDataBits};
    app->range_running = app->range_pending = app->range_simulation = app->range_live = false;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    app->profile_selected = index;
    app->seed_from_profile = true;
    app->seed_profile_id = app->profile.id;
    app->seed_profile_index = index;
    app->seed_profile_action = app->profile.entries[index].action;
    app->seed_profile_maker = app->profile.maker;
    app->seed_profile_file_id = app->profile.entries[index].file_id;
    lab_go(app, LabProfileSignal, 0);
    return true;
}

static uint8_t
    lab_fuzz_value(const IrLabApp* app, const uint8_t* changes, unsigned frame, unsigned byte) {
    return ir_lab_window_value(&app->bits, changes, frame, app->fuzz_offset, byte);
}

static void lab_open_protocol(IrLabApp* app) {
    app->protocol_return = app->screen == LabRange ? LabRange : LabHex;
    if(!ir_lab_protocol_analyze(&app->seed, &app->bits, &app->protocol_analysis)) {
        lab_message(
            app,
            app->protocol_return,
            "Analysis unavailable",
            "Need a complete raw frame",
            "Original replay retained",
            "Try repeated captures");
        return;
    }
    app->protocol_frame = app->frame;
    app->protocol_byte = 0;
    app->protocol_view_count = 0;
    for(unsigned n = 0; n < lab_set_capacity(app->series); ++n) {
        if(!app->views[n].valid || !ir_lab_view_width(&app->views[n], app->protocol_frame, 0))
            continue;
        if(app->protocol_analysis.payload_offset) {
            bool prefix_ok = true;
            for(unsigned f = 0; f < app->views[n].frames; ++f) {
                unsigned prefix = 0;
                for(unsigned bit = 0; bit < 15; ++bit)
                    prefix |=
                        (unsigned)ir_lab_bit_get(app->views[n].data, app->views[n].start[f] + bit)
                        << bit;
                prefix_ok &= app->views[n].length[f] == 39 && prefix == 0x0802;
            }
            if(!prefix_ok) continue;
        }
        if(ir_lab_protocol_view(
               &app->views[n],
               app->protocol_analysis.payload_offset,
               &app->protocol_views[app->protocol_view_count]))
            ++app->protocol_view_count;
    }
    lab_go(app, LabProtocol, 0);
}

static bool lab_protocol_save(IrLabApp* app) {
    char name[40], path[160], note[900];
    snprintf(name, sizeof(name), "protocol-%03lu", (unsigned long)++app->capture_id);
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, name);
    const IrLabProtocolAnalysis* a = &app->protocol_analysis;
    int n = snprintf(
        note,
        sizeof(note),
        "{\n  \"seed_id\":%lu,\n  \"candidate\":\"%s\",\n  \"basis\":\"timing/layout, receiver unverified\",\n"
        "  \"frame_cells\":%u,\n  \"frames\":%u,\n  \"unique_frames\":%u,\n  \"payload_offset_bits\":%u,\n"
        "  \"header_mark_us\":%lu,\n  \"header_space_us\":%lu,\n  \"short_us\":%lu,\n  \"long_us\":%lu,\n"
        "  \"check\":\"%s\",\n  \"check_passed\":%u,\n  \"check_tested\":%u,\n  \"carrier_basis\":\"assumed capture setting\"\n}\n",
        (unsigned long)app->seed_id,
        ir_lab_protocol_name(a->kind),
        a->frame_bits,
        a->frames,
        a->unique_frames,
        a->payload_offset,
        (unsigned long)a->header_mark,
        (unsigned long)a->header_space,
        (unsigned long)a->short_symbol,
        (unsigned long)a->long_symbol,
        ir_lab_protocol_check(a->kind),
        a->checks_passed,
        a->checks_tested);
    File* file = storage_file_alloc(app->storage);
    bool ok = app->storage_ready && n > 0 && (size_t)n < sizeof(note) &&
              storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW);
    ok = ok && storage_file_write(file, note, n) == (size_t)n && storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    ok = ok && closed;
    if(ok)
        lab_message(
            app, LabProtocolActions, "Analysis saved", name, "Candidate, not confirmation", "");
    else {
        app->storage_ready = false;
        ir_lab_stop(&app->gate);
        lab_message(
            app, LabProtocolActions, "Analysis save failed", "Check SD card", "Sending locked", "");
    }
    return ok;
}

static void lab_protocol_matches(IrLabApp* app) {
    IrLabDataView original;
    ir_lab_data_view(&app->bits, &original);
    unsigned scores[3] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
    char rows[3][40] = {
        "No same-layout reference", "Capture more known signals", "Data match; not acceptance"};
    for(unsigned set = 0; set < LAB_SETS; ++set)
        for(unsigned slot = 0; slot < lab_set_capacity(set); ++slot) {
            if(app->sets[set].slots[slot].seed_id == app->seed_id) continue;
            unsigned distance = ir_lab_protocol_distance(&original, &app->sets[set].views[slot]);
            for(unsigned rank = 0; rank < 3; ++rank)
                if(distance < scores[rank]) {
                    for(unsigned k = 2; k > rank; --k) {
                        scores[k] = scores[k - 1];
                        memcpy(rows[k], rows[k - 1], 40);
                    }
                    scores[rank] = distance;
                    snprintf(
                        rows[rank],
                        40,
                        "%.13s %hhu: %hu bits",
                        lab_set_names[set],
                        (unsigned char)(slot + 1),
                        (unsigned short)distance);
                    break;
                }
        }
    lab_message(app, LabProtocolActions, "Other saved data matches", rows[0], rows[1], rows[2]);
}

static unsigned lab_protocol_field_values(const IrLabApp* app, unsigned byte, uint8_t* values) {
    unsigned count = 0;
    for(unsigned n = 0; n < app->protocol_view_count; ++n)
        if(ir_lab_view_width(&app->protocol_views[n], app->protocol_frame, byte))
            values[count++] = ir_lab_view_byte(&app->protocol_views[n], app->protocol_frame, byte);
    return count;
}

static void lab_protocol_field_rule(const IrLabApp* app, unsigned byte, char* text) {
    uint8_t values[5];
    unsigned count = lab_protocol_field_values(app, byte, values);
    if(count < 3) {
        snprintf(text, 40, "Need 3+ controlled captures");
        return;
    }
    unsigned distinct = 0;
    for(unsigned n = 0; n < count; ++n) {
        bool seen = false;
        for(unsigned k = 0; k < n; ++k)
            seen |= values[n] == values[k];
        distinct += !seen;
    }
    if(distinct < 3) {
        snprintf(text, 40, "Need 3+ distinct values");
        return;
    }
    unsigned bytes =
        (app->protocol_analysis.frame_bits - app->protocol_analysis.payload_offset + 7) / 8;
    for(unsigned other = 0; other < bytes; ++other) {
        if(other == byte ||
           ir_lab_view_width(&app->protocol_views[0], app->protocol_frame, other) != 8)
            continue;
        uint8_t rhs[5];
        if(lab_protocol_field_values(app, other, rhs) != count) continue;
        bool sum = true, exclusive = true;
        unsigned csum = (values[0] + rhs[0]) & 255, cxor = values[0] ^ rhs[0];
        for(unsigned n = 1; n < count; ++n) {
            sum &= ((values[n] + rhs[n]) & 255) == csum;
            exclusive &= (values[n] ^ rhs[n]) == cxor;
        }
        if(sum || exclusive) {
            snprintf(
                text,
                40,
                "B%u %s B%u = %02X ?",
                byte + 1,
                sum ? "+" : "XOR",
                other + 1,
                sum ? csum : cxor);
            return;
        }
    }
    snprintf(text, 40, "No pair relation found");
}

static void lab_protocol_apply_steps(IrLabApp* app) {
    unsigned frame = app->protocol_frame, byte = app->protocol_byte;
    lab_use_protocol_preset(app);
    if(app->protocol_view_count >= 3) {
        for(unsigned n = 0; n < app->rule_count; ++n) {
            IrLabByteRule* r = &app->rules[n];
            int16_t step = 0;
            if(r->frame != frame || (app->fuzz_keep_check && r->byte == 2)) continue;
            if(ir_lab_hex_sequence(
                   app->protocol_views, app->protocol_view_count, frame, r->byte, &step) &&
               step) {
                r->from = ir_lab_view_byte(&app->protocol_views[0], frame, r->byte);
                r->to = ir_lab_view_byte(
                    &app->protocol_views[app->protocol_view_count - 1], frame, r->byte);
                r->step = step;
            }
        }
    }
    for(unsigned n = 0; n < app->rule_count; ++n)
        if(app->rules[n].frame == frame && app->rules[n].byte == byte) app->rule_selected = n;
}

// Read the end capture without replacing the start waveform or its saved edits.
static bool lab_fuzz_end_capture(IrLabApp* app, unsigned slot) {
    IrLabBits* end = malloc(sizeof(*end));
    if(!end) return false;
    bool ok = lab_slot_read(app, &app->slots[slot], &app->candidate) &&
              ir_lab_extract_bits(&app->candidate, end);
    ok = ok && end->frames == app->bits.frames && end->pulse_width == app->bits.pulse_width;
    for(unsigned f = 0; ok && f < app->bits.frames; ++f)
        ok = end->length[f] == app->bits.length[f];
    if(ok && app->fuzz_preset != IrLabProtocolUnknown) {
        IrLabProtocolAnalysis model;
        ok = ir_lab_protocol_analyze(&app->candidate, end, &model) &&
             model.kind == app->fuzz_preset;
        if(ok && app->fuzz_preset == IrLabProtocolInax)
            for(unsigned f = 0; ok && f < end->frames; ++f)
                ok = ir_lab_byte_value(end, NULL, f, 0) ==
                     ir_lab_byte_value(&app->bits, NULL, f, 0);
    }
    // Identical start repeat frames share edits, so their target bytes must agree.
    for(unsigned n = 0; ok && n < app->rule_count; ++n) {
        const IrLabByteRule* rule = &app->rules[n];
        unsigned value = ir_lab_window_value(end, NULL, rule->frame, rule->bit_offset, rule->byte);
        for(unsigned f = 0; ok && f < app->bits.frames; ++f)
            if(ir_lab_byte_width(&app->bits, f, rule->byte) &&
               ir_lab_byte_targets_overlap(&app->bits, rule->frame, rule->byte, f, rule->byte))
                ok = ir_lab_window_value(end, NULL, f, rule->bit_offset, rule->byte) == value;
    }
    if(ok) {
        for(unsigned n = 0; n < app->rule_count; ++n) {
            IrLabByteRule* rule = &app->rules[n];
            rule->to = ir_lab_window_value(end, NULL, rule->frame, rule->bit_offset, rule->byte);
            rule->step = rule->to > rule->from ? 1 : rule->to < rule->from ? -1 : 0;
            if(app->fuzz_keep_check && rule->byte == 2) {
                rule->to = rule->from;
                rule->step = 0;
            }
        }
        app->end_seed_id = app->slots[slot].seed_id;
    }
    free(end);
    if(!ok)
        lab_message(
            app,
            LabFuzzPick,
            "Cannot use end signal",
            "Need matching data layout",
            "Start and steps kept",
            "Check capture / SD card");
    return ok;
}

static unsigned lab_fuzz_pick_slot(const IrLabApp* app, unsigned row) {
    // First row is the manual editor; only populated captures follow it.
    for(unsigned slot = 0; slot < lab_set_capacity(app->series); ++slot)
        if(app->slots[slot].count && --row == 0) return slot;
    return lab_set_capacity(app->series);
}

static void lab_fuzz_set_edit(IrLabApp* app) {
    IrLabByteRule* rule = &app->rules[app->rule_selected];
    uint8_t old_from = rule->from, old_to = rule->to;
    unsigned maximum =
        (1U << ir_lab_window_width(&app->bits, rule->frame, rule->bit_offset, rule->byte)) - 1;
    if(app->range_field == 0)
        rule->from = app->range_edit;
    else if(app->range_field == 1)
        rule->to = app->range_edit;
    else {
        rule->step = app->range_edit_negative ? -app->range_edit : app->range_edit;
        // Entering a step on a fixed byte opens its full remaining range.
        if(rule->step && rule->from == rule->to) rule->to = rule->step < 0 ? 0 : maximum;
        if(!rule->step) rule->to = rule->from;
    }
    if(app->range_field < 2 && !rule->step && rule->from != rule->to)
        rule->step = rule->from < rule->to ? 1 : -1;
    if(rule->from != old_from || rule->to != old_to) app->end_seed_id = 0;
    lab_go(app, LabFuzzByte, app->range_field);
}

static bool lab_range_apply(IrLabApp* app) {
    uint8_t next[IR_LAB_MASK_BYTES];
    if(!ir_lab_byte_plan_apply(
           &app->bits, app->rules, app->rule_count, app->range_base, app->range_index, next))
        return false;
    if(app->fuzz_keep_check &&
       !(app->fuzz_preset == IrLabProtocolToto ? ir_lab_protocol_repair_toto(&app->bits, next) :
                                                 ir_lab_protocol_repair_inax(&app->bits, next)))
        return false;
    memcpy(app->changes, next, sizeof(next));
    app->frame = app->range_frame;
    app->byte = (app->fuzz_offset + app->range_byte * 8) / 8;
    app->mutation = (IrLabMutation){
        .mode = IrLabDataBits,
        .index = app->bits.start[app->frame] + app->fuzz_offset + app->range_byte * 8,
        .width =
            ir_lab_window_width(&app->bits, app->range_frame, app->fuzz_offset, app->range_byte),
        .value = lab_fuzz_value(app, app->changes, app->range_frame, app->range_byte)};
    return true;
}

static void lab_invert_command(IrLabApp* app) {
    if(app->fuzz_preset != IrLabProtocolInax || !app->protocol_analysis.checks_tested ||
       app->protocol_analysis.checks_passed != app->protocol_analysis.checks_tested) {
        lab_message(
            app,
            LabFuzzOptions,
            "Need an inverse pair",
            "Choose matching INAX preset",
            "This edits bytes only",
            "No IR sent");
        return;
    }
    // Complementing an 8-bit range also reverses its signed step. This is a
    // byte transformation, not a claim that two physical actions are opposites.
    for(unsigned n = 0; n < app->rule_count; ++n) {
        IrLabByteRule* rule = &app->rules[n];
        if(rule->byte == 1) {
            rule->from ^= 255;
            rule->to ^= 255;
            rule->step = -rule->step;
        } else if(rule->byte == 2) {
            for(unsigned c = 0; c < app->rule_count; ++c)
                if(app->rules[c].frame == rule->frame && app->rules[c].byte == 1)
                    rule->from = app->rules[c].from ^ 255;
            rule->to = rule->from;
            rule->step = 0;
        }
    }
    app->fuzz_keep_check = true;
    app->end_seed_id = 0;
    lab_go(app, LabRange, LabRangeSteps);
}

static bool lab_fuzz_validate(IrLabApp* app) {
    if(lab_range_count(app)) return true;
    for(unsigned n = 0; n < app->rule_count; ++n) {
        const IrLabByteRule* r = &app->rules[n];
        int distance = (int)r->to - r->from;
        if((!r->step && distance) || (distance < 0 && r->step > 0) ||
           (distance > 0 && r->step < 0)) {
            char line[40];
            snprintf(
                line,
                sizeof(line),
                "F%u Byte %u: %02X > %02X",
                r->frame + 1,
                r->byte + 1,
                r->from,
                r->to);
            app->rule_selected = n;
            lab_message(
                app,
                LabFuzzByte,
                "Check byte step",
                line,
                "Step must point to End",
                "00 keeps Start fixed");
            return false;
        }
    }
    lab_message(
        app, LabRange, "Invalid byte steps", "Check Start / End / Step", "No signal sent", "");
    return false;
}

// Track the first changing byte for the general mutation log and live focus.
static void lab_fuzz_focus(IrLabApp* app) {
    unsigned selected = app->rule_selected;
    for(unsigned n = 0; n < app->rule_count; ++n)
        if(app->rules[n].step) {
            selected = n;
            break;
        }
    app->frame = app->range_frame = app->rules[selected].frame;
    app->range_byte = app->rules[selected].byte;
    app->byte = (app->fuzz_offset + app->range_byte * 8) / 8;
}

static void lab_start_range(IrLabApp* app) {
    app->range_live = false;
    app->range_simulation = false;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    unsigned count = lab_range_count(app);
    if(!count) {
        lab_fuzz_validate(app);
        return;
    }
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return;
    }
    lab_fuzz_focus(app);
    memcpy(app->range_base, app->changes, sizeof(app->changes));
    app->range_running = true;
    app->range_pending = false;
    app->range_index = 0;
    if(!lab_range_apply(app)) {
        app->range_running = app->range_live = false;
        lab_message(
            app,
            LabRange,
            "Cannot prepare candidate",
            "Check layout / integrity",
            "No signal sent",
            "");
        return;
    }
    lab_open_send(app);
}

static void lab_start_preview(IrLabApp* app) {
    app->range_live = false;
    if(!lab_fuzz_validate(app)) return;
    lab_fuzz_focus(app);
    memcpy(app->range_base, app->changes, sizeof(app->changes));
    app->range_index = 0;
    app->range_running = app->range_simulation = true;
    app->range_pending = app->preview_paused = app->preview_complete =
        app->preview_press_consumed = false;
    if(app->preview_delay != 2) app->preview_delay = 1;
    app->preview_last_ms = lab_now_ms();
    __atomic_store_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
    __atomic_store_n(&app->preview_active, true, __ATOMIC_RELAXED);
    if(!lab_range_apply(app)) {
        app->range_running = app->range_simulation = false;
        __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
        lab_message(
            app,
            LabRange,
            "Cannot prepare candidate",
            "Check layout / integrity",
            "No signal sent",
            "");
        return;
    }
    lab_go(app, LabRangeRun, 0);
}

// This timer only prepares/display candidates. It has no path to any transmitter.
static void lab_preview_poll(IrLabApp* app) {
    if(!app->range_simulation || app->screen != LabRangeRun || app->preview_paused) return;
    if(__atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED)) {
        app->preview_paused = app->preview_press_consumed = true;
        __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
        return;
    }
    if((uint32_t)(lab_now_ms() - app->preview_last_ms) < app->preview_delay * 1000U) return;
    app->preview_last_ms = lab_now_ms();
    if(app->range_index + 1U < lab_range_count(app)) {
        ++app->range_index;
        lab_range_apply(app);
    } else if(app->range_repeat) {
        app->range_index = 0;
        lab_range_apply(app);
    } else {
        app->preview_complete = app->preview_paused = true;
        __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    }
}

static bool lab_auto_snapshot_note(IrLabApp* app, const char* name, const char* reason) {
    char path[160], chunk[900];
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, name);
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW);
    int n = snprintf(
        chunk,
        sizeof(chunk),
        "{\n  \"waveform\": \"%s.ir\",\n  \"seed_id\": %lu,\n  \"end_seed_id\": %lu,\n"
        "  \"capture_slot\": %u,\n  \"series_name\": \"%s\",\n  \"slot_role\": \"%s\",\n"
        "  \"value_hex\": \"%02X\",\n  \"last_emitted_trial\": %lu,\n  \"range_position\": %u,\n"
        "  \"emitted_count\": %lu,\n  \"candidate_count\": %u,\n  \"delay_ms\": %u,\n"
        "  \"repeat_range\": %s,\n  \"pause_trigger\": \"%s\",\n"
        "  \"receiver_response\": \"not_recorded\",\n  \"byte_steps\": [\n",
        name,
        (unsigned long)app->seed_id,
        (unsigned long)app->end_seed_id,
        app->active_slot + 1,
        lab_set_names[app->series],
        lab_slot_role(app->series, app->active_slot),
        app->auto_last_value,
        (unsigned long)app->auto_last_trial,
        app->auto_last_index + 1,
        (unsigned long)app->auto_sent_count,
        lab_range_count(app),
        app->preview_delay * 1000,
        app->range_repeat ? "true" : "false",
        reason);
    ok = ok && n > 0 && (size_t)n < sizeof(chunk) &&
         storage_file_write(file, chunk, n) == (size_t)n;
    for(unsigned r = 0; ok && r < app->rule_count; ++r) {
        const IrLabByteRule* rule = &app->rules[r];
        n = snprintf(
            chunk,
            sizeof(chunk),
            "    {\"frame\":%u,\"byte\":%u,\"start_hex\":\"%02X\",\"end_hex\":\"%02X\","
            "\"derived\":%s,\"bit_offset\":%u,\"step_hex\":\"%c%02X\",\"value_hex\":\"%02X\"}%s\n",
            rule->frame + 1,
            rule->byte + 1,
            rule->from,
            rule->to,
            app->fuzz_keep_check && rule->byte == 2 ? "true" : "false",
            rule->bit_offset,
            rule->step < 0 ? '-' : '+',
            rule->step < 0 ? -rule->step : rule->step,
            ir_lab_window_value(
                &app->bits, app->auto_last_changes, rule->frame, rule->bit_offset, rule->byte),
            r + 1 < app->rule_count ? "," : "");
        ok = n > 0 && (size_t)n < sizeof(chunk) && storage_file_write(file, chunk, n) == (size_t)n;
    }
    n = snprintf(
        chunk,
        sizeof(chunk),
        "  ],\n  \"profile_id\":%u,\n  \"profile_signal\":%lu,\n  \"profile_action\":\"%s\",\n"
        "  \"profile_maker\":\"%s\",\n"
        "  \"payload_offset_bits\":%u,\n  \"preset\":\"%s\",\n  \"auto_integrity\":\"%s\"\n}\n",
        app->seed_from_profile ? app->seed_profile_id : 0,
        (unsigned long)(app->seed_from_profile ? app->seed_profile_file_id : 0),
        app->seed_from_profile ? ir_lab_action_name(app->seed_profile_action) : "",
        app->seed_from_profile ? ir_lab_maker_name(app->seed_profile_maker) : "",
        app->fuzz_offset,
        lab_fuzz_preset_name(app),
        !app->fuzz_keep_check                 ? "off" :
        app->fuzz_preset == IrLabProtocolToto ? "TOTO XOR8" :
                                                "INAX inverse8");
    ok = ok && n > 0 && (size_t)n < sizeof(chunk) &&
         storage_file_write(file, chunk, n) == (size_t)n && storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    return ok && closed;
}

static void lab_auto_pause(IrLabApp* app, const char* reason, bool complete) {
    if(app->auto_paused) return;
    app->auto_paused = true;
    app->auto_complete = complete || (!app->range_repeat && app->auto_has_sent &&
                                      app->auto_last_index + 1U >= lab_range_count(app));
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    snprintf(app->auto_reason, sizeof(app->auto_reason), "%s", reason);
    if(!app->auto_has_sent) {
        memcpy(app->changes, app->range_base, sizeof(app->changes));
        app->auto_save_waveform = app->auto_save_note = false;
        return;
    }
    // waveform is committed only immediately before TX, never during preparation.
    // The selected value/mask are restored to the last completed emission.
    app->range_index = app->auto_last_index;
    app->frame = app->range_frame;
    app->byte = (app->fuzz_offset + app->range_byte * 8) / 8;
    memcpy(app->changes, app->auto_last_changes, sizeof(app->changes));
    app->mutation = (IrLabMutation){
        .mode = IrLabDataBits,
        .index = app->bits.start[app->range_frame] + app->fuzz_offset + app->range_byte * 8,
        .width =
            ir_lab_window_width(&app->bits, app->range_frame, app->fuzz_offset, app->range_byte),
        .value = app->auto_last_value};
    snprintf(
        app->auto_saved_name,
        sizeof(app->auto_saved_name),
        "paused-%03lu",
        (unsigned long)++app->capture_id);
    app->auto_save_waveform = lab_save_signal(app, &app->waveform, app->auto_saved_name);
    app->auto_save_note = app->auto_save_waveform &&
                          lab_auto_snapshot_note(app, app->auto_saved_name, reason);
    if(!app->auto_save_note) {
        app->storage_ready = false;
        ir_lab_stop(&app->gate);
    } else if(!lab_log(app, "range_snapshot", app->auto_saved_name)) {
        // The saved files remain valid; logging failure still locks further TX.
        ir_lab_stop(&app->gate);
    } else if(app->gate.pending_observation) {
        // A manual pause is retained without asserting that a beep occurred.
        // It must not leave the compare -> fuzzer workflow behind a result gate.
        if(lab_log(app, "observation_unrecorded", "auto_range_paused"))
            app->gate.pending_observation = false;
    }
}

static bool lab_auto_pause_requested(const IrLabApp* app) {
    return __atomic_load_n(&app->preview_stop_requested, __ATOMIC_RELAXED) ||
           __atomic_load_n(&app->stop_requested, __ATOMIC_RELAXED);
}

static void lab_auto_cancel_prepared(IrLabApp* app, const char* name) {
    app->gate.pending_observation = false; // This reserved attempt never emitted.
    lab_log(app, "tx_cancelled_before_emit", name);
    app->preview_press_consumed =
        __atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
    lab_auto_pause(app, "user_pause_before_emit", false);
}

static void lab_auto_poll(IrLabApp* app) {
    if(!app->range_live || app->screen != LabRangeRun || !app->auto_started || app->auto_paused)
        return;
    if(lab_auto_pause_requested(app)) {
        app->preview_press_consumed =
            __atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
        lab_auto_pause(app, "user_button", false);
        return;
    }
    uint32_t delay = app->preview_delay * 1000U;
    if(app->auto_waiting && (uint32_t)(lab_now_ms() - app->auto_wait_since) < delay) return;
    unsigned count = lab_range_count(app);
    if(!count) {
        lab_auto_pause(app, "invalid_byte_steps", false);
        return;
    }
    if(!app->range_repeat && app->auto_has_sent && app->auto_last_index + 1U >= count) {
        lab_auto_pause(app, "range_complete", true);
        return;
    }
    if(!app->storage_ready || app->gate.locked || app->gate.trials >= IR_LAB_MAX_TRIALS) {
        lab_auto_pause(app, "storage_lock_or_counter", false);
        return;
    }
    if(app->gate.pending_observation) {
        // Continuing a supervised range does not imply any observed response.
        if(!app->auto_has_sent || !lab_log(app, "observation_unrecorded", "next_auto_step")) {
            lab_auto_pause(app, "pending_response_or_log_error", false);
            return;
        }
        app->gate.pending_observation = false;
    }
    if(lab_auto_pause_requested(app)) {
        app->preview_press_consumed =
            __atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
        lab_auto_pause(app, "user_button", false);
        return;
    }
    if(!ir_lab_can_send_range(&app->gate, lab_now_ms(), delay)) return;
    app->range_index = app->auto_has_sent ? (app->auto_last_index + 1U) % count : 0;
    if(!lab_range_apply(app) || !lab_candidate(app)) {
        lab_auto_pause(app, "waveform_size_limit", false);
        return;
    }
    if(lab_auto_pause_requested(app)) {
        app->preview_press_consumed =
            __atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
        lab_auto_pause(app, "user_button", false);
        return;
    }
    if(!ir_lab_reserve_range_trial(&app->gate, lab_now_ms(), delay)) return;
    char name[32], mask[IR_LAB_MASK_BYTES * 2 + 1];
    snprintf(name, sizeof(name), "trial-%03lu", (unsigned long)app->gate.trials);
    for(unsigned b = 0; b < IR_LAB_MASK_BYTES; ++b)
        snprintf(mask + b * 2, 3, "%02x", app->changes[b]);
    if(!lab_save_signal(app, &app->candidate, name) || !lab_log(app, "changed_bit_mask", mask) ||
       !lab_log(app, "tx_intent", name)) {
        app->gate.pending_observation = false;
        app->storage_ready = false;
        ir_lab_stop(&app->gate);
        lab_auto_pause(app, "storage_failure_before_emit", false);
        return;
    }
    if(lab_auto_pause_requested(app)) {
        lab_auto_cancel_prepared(app, name);
        return;
    }
    app->waveform = app->candidate;
    lab_transmit(&app->waveform);
    // Start the listening delay after the captured block has finished emitting.
    app->gate.last_ms = lab_now_ms();
    app->last_emitted_trial = app->auto_last_trial = app->gate.trials;
    app->auto_last_index = app->range_index;
    app->auto_last_value = lab_fuzz_value(app, app->changes, app->range_frame, app->range_byte);
    memcpy(app->auto_last_changes, app->changes, sizeof(app->changes));
    app->auto_has_sent = app->auto_waiting = true;
    ++app->auto_sent_count;
    app->auto_wait_since = lab_now_ms();
    if(!lab_log(app, "tx_complete", "auto_range_acceptance_unverified")) {
        lab_auto_pause(app, "storage_failure_after_emit", false);
        return;
    }
    if(lab_auto_pause_requested(app)) {
        app->preview_press_consumed =
            __atomic_exchange_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
        lab_auto_pause(app, "user_button", false);
    }
}

static void lab_start_auto(IrLabApp* app) {
    unsigned count = lab_range_count(app);
    if(!count) {
        lab_fuzz_validate(app);
        return;
    }
    if(app->gate.pending_observation) {
        lab_go(app, LabResult, 0);
        return;
    }
    app->range_simulation = false;
    app->range_running = app->range_live = true;
    app->range_pending = false;
    app->auto_started = app->auto_paused = app->auto_complete = app->auto_has_sent = false;
    app->auto_waiting = app->auto_save_waveform = app->auto_save_note = false;
    app->auto_sent_count = app->auto_last_index = app->range_index = 0;
    app->auto_saved_name[0] = app->auto_reason[0] = 0;
    app->preview_press_consumed = false;
    if(app->preview_delay != 2) app->preview_delay = 1;
    __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
    __atomic_store_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
    lab_fuzz_focus(app);
    memcpy(app->range_base, app->changes, sizeof(app->changes));
    if(!lab_range_apply(app)) {
        app->range_running = app->range_live = false;
        lab_message(
            app,
            LabRange,
            "Cannot prepare candidate",
            "Check layout / integrity",
            "No signal sent",
            "");
        return;
    }
    lab_open_send(app);
}

static void lab_auto_begin(IrLabApp* app) {
    if(!app->storage_ready || app->gate.locked) {
        lab_auto_pause(app, "storage_or_lock", false);
        return;
    }
    if(!lab_log(app, "armed", "supervised_auto_range_user_start")) {
        lab_auto_pause(app, "arming_log_failure", false);
        return;
    }
    app->gate.armed = true;
    app->auto_started = true;
    __atomic_store_n(&app->preview_active, true, __ATOMIC_RELAXED);
    lab_auto_poll(app);
}

static void lab_auto_resume(IrLabApp* app) {
    if(app->auto_complete || app->gate.locked || app->gate.trials >= IR_LAB_MAX_TRIALS ||
       !app->storage_ready || strcmp(app->auto_reason, "waveform_size_limit") == 0)
        return;
    if(app->gate.pending_observation) {
        if(!lab_log(app, "observation_unrecorded", "user_resumes_auto_range")) return;
        app->gate.pending_observation = false;
    }
    if(!lab_log(app, "range_resume", "user_hold_ok")) return;
    app->auto_paused = false;
    app->auto_started = true;
    app->auto_waiting = true;
    app->auto_wait_since = lab_now_ms();
    app->gate.armed = true;
    app->preview_press_consumed = false;
    __atomic_store_n(&app->preview_stop_requested, false, __ATOMIC_RELAXED);
    __atomic_store_n(&app->preview_active, true, __ATOMIC_RELAXED);
}

static unsigned lab_menu_count(const IrLabApp* app) {
    switch(app->screen) {
    case LabHome:
        return 7;
    case LabSniff:
        return 5;
    case LabProfileMaker:
        return IrLabMakerCount;
    case LabSniffLabel:
        return IrLabActionCount;
    case LabProfiles:
        return app->profile_count;
    case LabProfileSignals:
        return app->profile.count;
    case LabProfileSignal:
        return 4;
    case LabSeries:
        return LAB_SETS;
    case LabSlots:
        return lab_set_capacity(app->series) + 1;
    case LabSlotAction:
        return 5;
    case LabCompare:
        return lab_set_capacity(app->series);
    case LabCompareAction:
        return 6;
    case LabSignal:
        return 5;
    case LabMore:
        return 6;
    case LabFrames:
        return app->bits.valid ? app->bits.frames : 0;
    case LabRange:
        return 8;
    case LabFuzzPresets:
        return 3;
    case LabFuzzPick:
        return lab_slot_count(app) + 1;
    case LabFuzzBytes:
        return app->rule_count;
    case LabFuzzByte:
        return 4;
    case LabFuzzOptions:
        return 5;
    case LabProtocolActions:
        return 6;
    case LabProtocolFields:
        return (app->protocol_analysis.frame_bits - app->protocol_analysis.payload_offset + 7) / 8;
    case LabResult:
        return 4;
    case LabSettings:
        return 4;
    case LabCarrier:
        return 41;
    default:
        return 0;
    }
}

static void lab_menu_item(const IrLabApp* app, unsigned index, char* text) {
    static const char* const home[] = {
        "Capture signal",
        "Sniff mode",
        "Load profile",
        "Fuzzer",
        "Capture sets / diff",
        "Analyze protocol",
        "Settings / help"};
    static const char* const signal[] = {
        "Send to fuzzer", "Analyze protocol", "Replay edited hex", "Replay original", "More"};
    static const char* const more[] = {
        "Select data frame",
        "Restore this byte",
        "Restore all bytes",
        "Save edited .ir",
        "Capture another signal",
        "Signal details"};
    static const char* const result[] = {
        "No response", "Beep heard", "Action observed", "Unexpected: STOP"};
    switch(app->screen) {
    case LabHome:
        snprintf(text, 40, "%s", home[index]);
        break;
    case LabSniff: {
        static const char* const actions[] = {
            "Capture next signal",
            "Saved signals",
            "Save & close",
            "New profile",
            "Remove duplicates"};
        snprintf(text, 40, "%s", actions[index]);
        break;
    }
    case LabSniffLabel:
        snprintf(text, 40, "%s", ir_lab_action_name(index));
        break;
    case LabProfiles:
        lab_profile_name(text, 40, app->profile_makers[index], app->profile_ids[index]);
        break;
    case LabProfileMaker:
        snprintf(text, 40, "%s", ir_lab_maker_name(index));
        break;
    case LabProfileSignals:
        snprintf(
            text, 40, "%u: %s", index + 1, ir_lab_action_name(app->profile.entries[index].action));
        break;
    case LabProfileSignal: {
        static const char* const actions[] = {
            "Send to fuzzer", "Analyze protocol", "Replay original", "View hex"};
        snprintf(text, 40, "%s", actions[index]);
        break;
    }
    case LabSeries:
        snprintf(
            text,
            40,
            "%s %u/%u",
            lab_set_names[index],
            lab_set_count(&app->sets[index]),
            lab_set_capacity(index));
        break;
    case LabSlots:
        if(index == lab_set_capacity(app->series)) {
            snprintf(text, 40, "Save profile");
            break;
        }
        if(lab_set_capacity(app->series) == 2) {
            snprintf(
                text,
                40,
                "%s: %s",
                lab_slot_role(app->series, index),
                app->slots[index].count ? "saved" : "capture");
            break;
        }
        if(!app->slots[index].count)
            snprintf(text, 40, "%u: [empty] OK capture", index + 1);
        else if(app->views[index].valid) {
            unsigned offset = snprintf(text, 40, "%u: ", index + 1);
            for(unsigned b = 0; b < 4 && ir_lab_view_width(&app->views[index], 0, b); ++b)
                offset += snprintf(
                    text + offset,
                    40 - offset,
                    "%02X ",
                    ir_lab_view_byte(&app->views[index], 0, b));
        } else
            snprintf(text, 40, "%u: raw / hex unavailable", index + 1);
        break;
    case LabSlotAction:
        snprintf(
            text,
            40,
            "%s",
            index == 0 ? "Edit selected signal" :
            index == 1 ? "Replay original" :
            index == 2 ? "Recapture this slot" :
            index == 3 ? "Send to fuzzer" :
                         "Analyze protocol");
        break;
    case LabCompare:
        if(!ir_lab_view_width(&app->views[index], app->compare_frame, app->compare_byte))
            snprintf(text, 40, "%u: data unavailable", index + 1);
        else if(index == 0)
            snprintf(
                text,
                40,
                "1: %02X (baseline)",
                ir_lab_view_byte(&app->views[0], app->compare_frame, app->compare_byte));
        else
            lab_compare_pair(app, index, text);
        break;
    case LabCompareAction:
        if(index == 0)
            snprintf(text, 40, "Send %u to fuzzer", app->selected_slot + 1);
        else if(index < 3)
            snprintf(
                text,
                40,
                "%s %u",
                index == 1 ? "Edit capture" : "Replay original",
                app->selected_slot + 1);
        else if(index == 5)
            snprintf(text, 40, "Save profile");
        else
            snprintf(text, 40, "%s", index == 3 ? "Next data frame" : "Hex step details");
        break;
    case LabSignal:
        snprintf(text, 40, "%s", signal[index]);
        break;
    case LabMore:
        snprintf(text, 40, "%s", more[index]);
        break;
    case LabFrames:
        snprintf(text, 40, "Frame %u: %u data bits", index + 1, app->bits.length[index]);
        break;
    case LabRange:
        if(index == LabRangeFrom)
            snprintf(text, 40, "Start signal: #%lu", (unsigned long)app->seed_id);
        else if(index == LabRangeTo) {
            if(app->end_seed_id)
                snprintf(text, 40, "End signal: #%lu", (unsigned long)app->end_seed_id);
            else
                snprintf(text, 40, "End signal: edit hex");
        } else if(index == LabRangeSteps)
            snprintf(text, 40, "Byte steps (+ / -)");
        else if(index == LabRangeStart)
            snprintf(text, 40, "Start fuzzing (%u)", lab_range_count(app));
        else if(index == LabRangePreset)
            snprintf(
                text,
                40,
                "Preset: %s",
                app->fuzz_preset == IrLabProtocolUnknown ? "Manual" : lab_fuzz_preset_name(app));
        else if(index == LabRangeAnalyze)
            snprintf(text, 40, "Analyze protocol");
        else if(index == LabRangeDelay)
            snprintf(text, 40, "Delay: %u sec", app->preview_delay == 2 ? 2 : 1);
        else
            snprintf(text, 40, "More options");
        break;
    case LabFuzzPresets: {
        static const char* const presets[] = {"Captured bytes", "TOTO", "INAX"};
        snprintf(text, 40, "%s", presets[index]);
        break;
    }
    case LabFuzzPick: {
        if(!index) {
            snprintf(text, 40, "Edit %s hex", app->fuzz_pick_end ? "End" : "Start");
            break;
        }
        unsigned slot = lab_fuzz_pick_slot(app, index);
        snprintf(
            text,
            40,
            "#%lu: %02X %02X %02X %02X",
            (unsigned long)app->slots[slot].seed_id,
            ir_lab_view_byte(&app->views[slot], 0, 0),
            ir_lab_view_byte(&app->views[slot], 0, 1),
            ir_lab_view_byte(&app->views[slot], 0, 2),
            ir_lab_view_byte(&app->views[slot], 0, 3));
        break;
    }
    case LabFuzzBytes: {
        const IrLabByteRule* r = &app->rules[index];
        if(app->fuzz_keep_check && r->byte == 2) {
            snprintf(
                text,
                40,
                "F%u B3: %s",
                r->frame + 1,
                app->fuzz_preset == IrLabProtocolToto ? "automatic XOR" : "auto inverse");
            break;
        }
        if(app->bits.frames == 1)
            snprintf(
                text,
                40,
                "Byte %u %02X>%02X %c%02X",
                r->byte + 1,
                r->from,
                r->to,
                r->step < 0 ? '-' : '+',
                r->step < 0 ? -r->step : r->step);
        else
            snprintf(
                text,
                40,
                "F%u B%u %02X>%02X %c%02X",
                r->frame + 1,
                r->byte + 1,
                r->from,
                r->to,
                r->step < 0 ? '-' : '+',
                r->step < 0 ? -r->step : r->step);
        break;
    }
    case LabFuzzByte: {
        const IrLabByteRule* r = &app->rules[app->rule_selected];
        if(index < 2)
            snprintf(
                text, 40, "%s: %02X", index == 0 ? "Start" : "End", index == 0 ? r->from : r->to);
        else if(index == 2)
            snprintf(
                text,
                40,
                "Step: %c%02X%s",
                r->step < 0 ? '-' : '+',
                r->step < 0 ? -r->step : r->step,
                r->step ? "" : " (hold)");
        else
            snprintf(text, 40, "Done");
        break;
    }
    case LabFuzzOptions:
        if(index == 0)
            snprintf(text, 40, "Repeat: %s", app->range_repeat ? "until paused" : "off");
        else if(index < 3)
            snprintf(text, 40, "%s", index == 1 ? "Preview (NO IR)" : "Send one at a time");
        else if(index == 4)
            snprintf(text, 40, "Invert command");
        else
            snprintf(
                text,
                40,
                "%s: %s",
                app->fuzz_preset == IrLabProtocolToto ? "TOTO XOR" :
                app->fuzz_preset == IrLabProtocolInax ? "Keep inverse pair" :
                                                        "Auto check",
                app->fuzz_keep_check ? "on" : "off");
        break;
    case LabProtocolActions: {
        static const char* const labels[] = {
            "Use layout in fuzzer",
            "Timing / framing",
            "Integrity checks",
            "Compare fields / steps",
            "Match saved signals",
            "Save analysis report"};
        snprintf(text, 40, "%s", labels[index]);
        break;
    }
    case LabProtocolFields: {
        uint8_t values[5];
        unsigned count = lab_protocol_field_values(app, index, values);
        uint8_t mask = 0;
        for(unsigned n = 1; n < count; ++n)
            mask |= values[n] ^ values[0];
        int16_t step = 0;
        bool seq =
            count >= 3 &&
            ir_lab_hex_sequence(
                app->protocol_views, app->protocol_view_count, app->protocol_frame, index, &step);
        if(seq && step)
            snprintf(
                text,
                40,
                "B%u: step %c%02X ?",
                index + 1,
                step < 0 ? '-' : '+',
                step < 0 ? -step : step);
        else
            snprintf(
                text,
                40,
                "B%u: %s %02X",
                index + 1,
                !mask ? "fixed" : "vary mask",
                count ? !mask ? values[0] : mask : 0);
        break;
    }
    case LabResult:
        snprintf(text, 40, "%s", result[index]);
        break;
    case LabSettings:
        if(index == 0)
            snprintf(text, 40, "STOP signal (optional)");
        else if(index == 1)
            snprintf(text, 40, "Carrier: %lu kHz", (unsigned long)(app->carrier / 1000));
        else
            snprintf(text, 40, "%s", index == 2 ? "Session status" : "How to use");
        break;
    case LabCarrier:
        snprintf(
            text,
            40,
            "%u kHz%s",
            index + 20,
            app->carrier == (index + 20) * 1000 ? " (selected)" : "");
        break;
    default:
        text[0] = '\0';
        break;
    }
}

static void lab_refresh(IrLabApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    memset(app->lines, 0, sizeof(app->lines));
    app->drawn_screen = app->screen;
    app->drawn_row = app->row;
    app->hex_count = 0;
    app->highlighted = -1;
    app->menu_first = 0;
    unsigned count = lab_menu_count(app);
    app->menu_count = count;
    if(count) {
        if(app->row >= count) app->row = count - 1;
        unsigned visible = app->screen == LabSlots || app->screen == LabCompare ? 5 : 4;
        app->menu_first = app->row < visible ? 0 : app->row - visible + 1;
        for(unsigned i = 0; i < visible && app->menu_first + i < count; ++i)
            lab_menu_item(app, app->menu_first + i, app->lines[i + 1]);
        app->highlighted = app->row - app->menu_first + 1;
        snprintf(app->lines[6], 40, "OK: select  Back: return");
    }
    switch(app->screen) {
    case LabProtocol:
        snprintf(app->lines[0], 40, "%s", ir_lab_protocol_name(app->protocol_analysis.kind));
        snprintf(
            app->lines[1],
            40,
            "%u cells / %u frames",
            app->protocol_analysis.frame_bits,
            app->protocol_analysis.frames);
        snprintf(
            app->lines[2], 40, "%u unique frame patterns", app->protocol_analysis.unique_frames);
        if(app->protocol_analysis.payload_offset)
            snprintf(app->lines[3], 40, "15 prefix + 24 payload");
        else
            snprintf(app->lines[3], 40, "Candidate; not device ID");
        if(app->protocol_analysis.checks_tested)
            snprintf(
                app->lines[4],
                40,
                "Check: %u/%u frames",
                app->protocol_analysis.checks_passed,
                app->protocol_analysis.checks_tested);
        else
            snprintf(app->lines[4], 40, "No check established");
        snprintf(app->lines[6], 40, "OK actions  Back return");
        break;
    case LabProtocolActions:
        snprintf(app->lines[0], 40, "Protocol actions");
        break;
    case LabProtocolTiming:
        snprintf(app->lines[0], 40, "Timing model (us)");
        snprintf(
            app->lines[1],
            40,
            "Header %lu / %lu",
            (unsigned long)app->protocol_analysis.header_mark,
            (unsigned long)app->protocol_analysis.header_space);
        snprintf(
            app->lines[2],
            40,
            "Short %lu Long %lu",
            (unsigned long)app->protocol_analysis.short_symbol,
            (unsigned long)app->protocol_analysis.long_symbol);
        snprintf(
            app->lines[3],
            40,
            "%s",
            app->bits.pulse_width ? "Pulse-width data marks" : "Pulse-distance spaces");
        snprintf(app->lines[4], 40, "Carrier: assumed setting");
        snprintf(app->lines[6], 40, "OK / Back: actions");
        break;
    case LabProtocolChecks:
        snprintf(app->lines[0], 40, "Integrity candidate");
        snprintf(app->lines[1], 40, "%s", ir_lab_protocol_check(app->protocol_analysis.kind));
        snprintf(
            app->lines[2],
            40,
            "Pass %u / %u frames",
            app->protocol_analysis.checks_passed,
            app->protocol_analysis.checks_tested);
        snprintf(
            app->lines[3],
            40,
            "%s",
            app->protocol_analysis.kind == IrLabProtocolToto ? "Published TOTO XOR model" :
                                                               "Observed/corpus hypothesis");
        snprintf(app->lines[4], 40, "Field meaning unverified");
        snprintf(app->lines[6], 40, "OK / Back: actions");
        break;
    case LabProtocolFields:
        snprintf(
            app->lines[0],
            40,
            "F%u: %u capture samples",
            app->protocol_frame + 1,
            app->protocol_view_count);
        snprintf(app->lines[6], 40, "L/R frame  OK byte");
        break;
    case LabProtocolField: {
        uint8_t values[5];
        unsigned count = lab_protocol_field_values(app, app->protocol_byte, values);
        snprintf(
            app->lines[0],
            40,
            "F%u B%u candidates",
            app->protocol_frame + 1,
            app->protocol_byte + 1);
        unsigned used = 0;
        for(unsigned n = 0; n < count; ++n)
            used += snprintf(app->lines[1] + used, 40 - used, "%02X ", values[n]);
        int16_t step = 0;
        if(count >= 3 && ir_lab_hex_sequence(
                             app->protocol_views,
                             app->protocol_view_count,
                             app->protocol_frame,
                             app->protocol_byte,
                             &step))
            snprintf(
                app->lines[2],
                40,
                "Observed step: %c%02X",
                step < 0 ? '-' : '+',
                step < 0 ? -step : step);
        else
            snprintf(app->lines[2], 40, "No fixed signed step");
        lab_protocol_field_rule(app, app->protocol_byte, app->lines[3]);
        snprintf(app->lines[4], 40, "Meaning needs labeled data");
        snprintf(app->lines[6], 40, "OK: use steps in fuzzer");
        break;
    }
    case LabHome:
        snprintf(app->lines[0], 40, "IR Fuzz Compare 0.17");
        break;
    case LabProfileMaker:
        snprintf(app->lines[0], 40, "Who is the maker?");
        if(app->profile_maker_return == LabSniff)
            snprintf(app->lines[6], 40, "Label only  Back cancel");
        else
            snprintf(app->lines[6], 40, "%u captures; Back cancel", lab_slot_count(app));
        break;
    case LabSniff:
        snprintf(app->lines[0], 40, "Sniff: %u saved", app->profile.count);
        if(app->profile.id)
            lab_profile_name(app->lines[6], 40, app->profile.maker, app->profile.id);
        else
            snprintf(app->lines[6], 40, "Capture then label");
        break;
    case LabSniffLabel:
        snprintf(app->lines[0], 40, "What is this?");
        snprintf(app->lines[6], 40, "Guess: %s", ir_lab_protocol_name(app->sniff_guess));
        break;
    case LabProfiles:
        snprintf(app->lines[0], 40, "Load profile");
        snprintf(app->lines[6], 40, "OK load  Back home");
        break;
    case LabProfileSignals:
        lab_profile_name(app->lines[0], 40, app->profile.maker, app->profile.id);
        snprintf(app->lines[6], 40, "OK select  Back profile");
        break;
    case LabProfileSignal:
        snprintf(
            app->lines[0],
            40,
            "%s",
            ir_lab_action_name(app->profile.entries[app->profile_selected].action));
        snprintf(app->lines[6], 40, "Original kept on SD");
        break;
    case LabSeries:
        snprintf(app->lines[0], 40, "Capture sets");
        snprintf(app->lines[6], 40, "Choose a capture set");
        break;
    case LabSlots: {
        snprintf(
            app->lines[0],
            40,
            "%s %u/%u",
            lab_set_names[app->series],
            lab_slot_count(app),
            lab_set_capacity(app->series));
        snprintf(
            app->lines[6],
            40,
            "%s",
            app->row == lab_set_capacity(app->series) ? "OK: save profile" :
                                                        "OK: slot  Right: compare");
        break;
    }
    case LabSlotAction:
        if(lab_set_capacity(app->series) == 2)
            snprintf(app->lines[0], 40, "%s", lab_slot_role(app->series, app->selected_slot));
        else
            snprintf(app->lines[0], 40, "Capture %u", app->selected_slot + 1);
        break;
    case LabCompare:
        snprintf(
            app->lines[0],
            40,
            "Steps F%u / Byte %u",
            app->compare_frame + 1,
            app->compare_byte + 1);
        if(lab_set_capacity(app->series) == 2)
            snprintf(
                app->lines[3],
                40,
                "%s / %s",
                lab_slot_role(app->series, 0),
                lab_slot_role(app->series, 1));
        snprintf(app->lines[6], 40, "L/R byte  OK actions");
        break;
    case LabCompareAction:
        snprintf(app->lines[0], 40, "Capture %u actions", app->selected_slot + 1);
        break;
    case LabPattern:
        snprintf(
            app->lines[0], 40, "Steps F%u / B%u", app->compare_frame + 1, app->compare_byte + 1);
        for(unsigned n = 1; n < lab_set_capacity(app->series); ++n)
            lab_compare_pair(app, n, app->lines[n]);
        snprintf(app->lines[6], 40, "L/R byte  OK compare");
        break;
    case LabHex:
        if(app->bits.valid) {
            snprintf(app->lines[0], 40, "Hex F%u / Byte %u", app->frame + 1, app->byte + 1);
            snprintf(
                app->lines[1],
                40,
                "Original %02X > Now %02X",
                lab_byte_value(app, false, app->frame, app->byte),
                lab_byte_value(app, true, app->frame, app->byte));
            unsigned first = (app->byte / 8) * 8;
            app->hex_selected = app->byte % 8;
            for(unsigned line = 0; line < 2; ++line) {
                unsigned offset = 0;
                for(unsigned column = 0; column < 4; ++column) {
                    unsigned byte = first + line * 4 + column;
                    if(!ir_lab_byte_width(&app->bits, app->frame, byte)) break;
                    snprintf(
                        app->hex_cells[app->hex_count++],
                        3,
                        "%02X",
                        lab_byte_value(app, true, app->frame, byte));
                    offset += snprintf(
                        app->lines[line + 2] + offset,
                        40 - offset,
                        "%02X%s",
                        lab_byte_value(app, true, app->frame, byte),
                        column < 3 ? " " : "");
                }
            }
            if(ir_lab_byte_width(&app->bits, app->frame, app->byte) < 8)
                snprintf(app->lines[4], 40, "+/- 01 (max %02X)", lab_byte_max(app));
            else
                snprintf(app->lines[4], 40, "Up/Down: hex +1 / -1");
            snprintf(app->lines[6], 40, "L/R: byte  OK: menu");
        } else {
            snprintf(app->lines[0], 40, "Signal captured");
            snprintf(app->lines[1], 40, "Hex unavailable");
            snprintf(app->lines[2], 40, "%s", ir_lab_bits_error(&app->bits));
            snprintf(app->lines[3], 40, "Original replay available");
            snprintf(app->lines[6], 40, "OK: menu  Back: home");
        }
        break;
    case LabSignal:
        snprintf(app->lines[0], 40, "Signal actions");
        break;
    case LabMore:
        snprintf(app->lines[0], 40, "More signal options");
        break;
    case LabFrames:
        snprintf(app->lines[0], 40, "Choose a data frame");
        break;
    case LabRange:
        if(app->fuzz_preset == IrLabProtocolUnknown)
            snprintf(app->lines[0], 40, "Fuzzer");
        else
            snprintf(app->lines[0], 40, "Fuzzer: %s", lab_fuzz_preset_name(app));
        snprintf(app->lines[6], 40, "OK select  Back hex");
        break;
    case LabFuzzPresets:
        snprintf(app->lines[0], 40, "Choose preset");
        snprintf(app->lines[6], 40, "Use matching capture");
        break;
    case LabFuzzPick:
        snprintf(
            app->lines[0],
            40,
            "%s: %s",
            app->fuzz_pick_end ? "End" : "Start",
            lab_set_names[app->series]);
        snprintf(app->lines[6], 40, "OK select  Back fuzzer");
        break;
    case LabFuzzBytes:
        snprintf(
            app->lines[0],
            40,
            "%s",
            app->range_field < 2 ? (app->range_field ? "End signal bytes" : "Start signal bytes") :
                                   "Byte steps (hex)");
        snprintf(app->lines[6], 40, "OK edit  Back fuzzer");
        break;
    case LabFuzzByte:
        snprintf(
            app->lines[0],
            40,
            "F%u Byte %u",
            app->rules[app->rule_selected].frame + 1,
            app->rules[app->rule_selected].byte + 1);
        snprintf(app->lines[6], 40, "OK edit  Back bytes");
        break;
    case LabFuzzOptions:
        snprintf(app->lines[0], 40, "Fuzzer options");
        break;
    case LabRangeValue:
        snprintf(
            app->lines[0],
            40,
            "Byte %u: %s hex",
            app->rules[app->rule_selected].byte + 1,
            app->range_field == 0 ? "Start" :
            app->range_field == 1 ? "End" :
                                    "Step");
        if(app->range_field == 2)
            snprintf(
                app->lines[1], 40, "%c%02X", app->range_edit_negative ? '-' : '+', app->range_edit);
        else
            snprintf(app->lines[1], 40, "%02X", app->range_edit);
        snprintf(
            app->lines[2],
            40,
            "%s",
            app->range_nibble == 2 ? "Editing sign (+ / -)" :
            app->range_nibble == 1 ? "Editing high hex digit" :
                                     "Editing low hex digit");
        snprintf(app->lines[3], 40, "Up/Down: change");
        snprintf(app->lines[4], 40, "Left/Right: select digit");
        snprintf(app->lines[6], 40, "OK save  Back cancel");
        break;
    case LabRangeRun: {
        if(app->range_live && !app->auto_started && !app->auto_paused) {
            snprintf(app->lines[0], 40, "Ready to fuzz");
            snprintf(
                app->lines[1],
                40,
                "%u signals / %us delay",
                lab_range_count(app),
                app->preview_delay);
            snprintf(app->lines[2], 40, "All byte steps together");
            snprintf(app->lines[3], 40, "Hold OK: start sending");
            snprintf(app->lines[4], 40, "Then OK: pause + save");
            snprintf(app->lines[6], 40, "Back: fuzzer settings");
        } else if(app->range_live && app->auto_paused) {
            snprintf(
                app->lines[0],
                40,
                "%s: %lu sent",
                app->auto_complete ? "Done" : "Paused",
                (unsigned long)app->auto_sent_count);
            snprintf(
                app->lines[1],
                40,
                "%s",
                app->auto_has_sent ? "Last sent signal kept" : "No signal sent");
            snprintf(
                app->lines[2],
                40,
                "%s",
                app->auto_save_note ? app->auto_saved_name : "Check SD / save status");
            snprintf(
                app->lines[3],
                40,
                "%s",
                app->auto_save_note ? "Saved .ir + byte steps" : app->auto_reason);
            snprintf(
                app->lines[4],
                40,
                "%s",
                app->gate.locked   ? "Sending locked" :
                app->auto_complete ? "Range complete" :
                                     "Hold OK: resume next");
            snprintf(app->lines[6], 40, "Back: edit last signal");
        } else if(app->range_live) {
            snprintf(
                app->lines[0],
                40,
                "IR sent: %lu%s",
                (unsigned long)app->auto_sent_count,
                app->range_repeat ? " loop" : "");
            unsigned offset = 0;
            for(unsigned byte = 0;
                byte < 7 &&
                ir_lab_window_width(&app->bits, app->range_frame, app->fuzz_offset, byte);
                ++byte)
                offset += snprintf(
                    app->lines[1] + offset,
                    40 - offset,
                    "%02X ",
                    lab_fuzz_value(app, app->changes, app->range_frame, byte));
            uint32_t elapsed =
                app->auto_waiting ? (uint32_t)(lab_now_ms() - app->auto_wait_since) : 0;
            uint32_t gap = app->preview_delay * 1000U;
            snprintf(
                app->lines[2],
                40,
                "Next in %lu.%lu sec",
                (unsigned long)(elapsed < gap ? (gap - elapsed) / 1000 : 0),
                (unsigned long)(elapsed < gap ? ((gap - elapsed) % 1000) / 100 : 0));
            snprintf(app->lines[3], 40, "Listen for a beep");
            snprintf(app->lines[4], 40, "OK: pause + save");
            snprintf(app->lines[6], 40, "Back: stop + save");
        } else if(app->range_simulation) {
            snprintf(
                app->lines[0], 40, "NO IR: %u/%u", app->range_index + 1, lab_range_count(app));
            snprintf(
                app->lines[1],
                40,
                "%s",
                app->preview_complete ? "Done" :
                app->preview_paused   ? "Paused" :
                                        "Previewing byte steps");
            snprintf(app->lines[2], 40, "No IR is transmitted");
            snprintf(app->lines[3], 40, "OK: freeze this signal");
            snprintf(app->lines[6], 40, "Back: fuzzer");
        } else {
            snprintf(
                app->lines[0], 40, "Signal %u/%u", app->range_index + 1, lab_range_count(app));
            snprintf(app->lines[1], 40, "All byte steps together");
            snprintf(app->lines[2], 40, "One signal per hold");
            snprintf(app->lines[3], 40, "Hold OK: send signal");
            snprintf(app->lines[6], 40, "Back: fuzzer");
        }
        break;
    }
    case LabResult:
        snprintf(
            app->lines[0],
            40,
            "IR sent #%lu",
            (unsigned long)(app->last_emitted_trial ? app->last_emitted_trial : app->gate.trials));
        break;
    case LabSettings:
        snprintf(app->lines[0], 40, "Settings / help");
        break;
    case LabCarrier:
        snprintf(app->lines[0], 40, "Assumed raw carrier");
        break;
    case LabCapture:
        if(app->capture_batch && !app->capture_stop) {
            if(lab_set_capacity(app->series) == 2)
                snprintf(
                    app->lines[0], 40, "Capture %s", lab_slot_role(app->series, app->capture_slot));
            else
                snprintf(
                    app->lines[0],
                    40,
                    "Capture slot %u/%u",
                    app->capture_slot + 1,
                    lab_set_capacity(app->series));
        } else
            snprintf(app->lines[0], 40, "Capture %s", app->capture_stop ? "STOP / OFF" : "signal");
        snprintf(app->lines[1], 40, "Point remote at Flipper");
        snprintf(app->lines[2], 40, "Press its button once");
        snprintf(
            app->lines[3],
            40,
            "Waiting... %lu s left",
            (unsigned long)(15 - (lab_now_ms() - app->rx_started) / 1000));
        snprintf(app->lines[4], 40, "Carrier: %lu kHz", (unsigned long)(app->carrier / 1000));
        snprintf(app->lines[6], 40, "Back: cancel capture");
        break;
    case LabSend:
        snprintf(
            app->lines[0],
            40,
            "Send %s?",
            app->mutation.mode == IrLabReplay ? "original" : "hex data");
        if(app->mutation.mode != IrLabReplay && app->bits.valid)
            snprintf(
                app->lines[1],
                40,
                "F%u Byte %u: %02X > %02X",
                app->frame + 1,
                app->byte + 1,
                lab_byte_value(app, false, app->frame, app->byte),
                lab_byte_value(app, true, app->frame, app->byte));
        else
            snprintf(app->lines[1], 40, "Captured waveform");
        snprintf(
            app->lines[2],
            40,
            "%s",
            app->mutation.mode == IrLabReplay || !ir_lab_changed_count(&app->bits, app->changes) ?
                "Original captured data" :
                "Hold OK when ready");
        snprintf(app->lines[3], 40, "Hold OK: send once");
        snprintf(app->lines[4], 40, "Checksums kept as edited");
        snprintf(app->lines[6], 40, "Back: cancel send");
        break;
    case LabInfo:
        snprintf(app->lines[0], 40, "Signal details");
        snprintf(
            app->lines[1],
            40,
            "%u timings / %u bits",
            (unsigned)app->seed.count,
            app->bits.valid ? app->bits.count : 0);
        snprintf(
            app->lines[2],
            40,
            "%u data / %u repeats",
            app->bits.valid ? app->bits.frames : 0,
            app->bits.repeats);
        snprintf(app->lines[3], 40, "%lu Hz assumed", (unsigned long)app->seed.frequency);
        snprintf(app->lines[4], 40, "Hex: first bit = LSB");
        snprintf(app->lines[6], 40, "OK / Back: hex screen");
        break;
    case LabHelp:
        snprintf(app->lines[0], 40, "Capture > Fuzzer");
        snprintf(app->lines[1], 40, "Hex: Up/Down +1 / -1");
        snprintf(app->lines[2], 40, "Left/Right: choose byte");
        snprintf(app->lines[3], 40, "Each byte: Start End Step");
        snprintf(app->lines[4], 40, "Hold Back: STOP + lock");
        snprintf(app->lines[6], 40, "OK / Back: settings");
        break;
    case LabMessage:
        for(unsigned i = 0; i < 4; ++i)
            snprintf(app->lines[i], 40, "%s", app->message[i]);
        snprintf(app->lines[6], 40, "%s", "OK / Back: continue");
        break;
    }
    furi_mutex_release(app->mutex);
    view_port_update(app->view);
}

static void lab_fit_text(Canvas* canvas, int x, int y, const char* text) {
    char fit[40];
    snprintf(fit, sizeof(fit), "%s", text);
    bool shortened = false;
    while(strlen(fit) && canvas_string_width(canvas, fit) > (unsigned)(125 - x)) {
        fit[strlen(fit) - 1] = '\0';
        shortened = true;
    }
    if(shortened && strlen(fit) > 2) {
        fit[strlen(fit) - 1] = '.';
        fit[strlen(fit) - 2] = '.';
    }
    canvas_draw_str(canvas, x, y, fit);
}

static void lab_draw_icon(Canvas* canvas, LabScreen screen) {
    if(screen == LabSlots || screen == LabCompare || screen == LabPattern) {
        for(unsigned n = 0; n < 5; ++n)
            canvas_draw_box(canvas, 3, 2 + n * 2, 7 - n, 1);
    } else if(
        screen == LabRange || screen == LabRangeRun || screen == LabFuzzBytes ||
        screen == LabFuzzByte) {
        canvas_draw_line(canvas, 3, 2, 10, 6);
        canvas_draw_line(canvas, 10, 6, 3, 10);
        canvas_draw_line(canvas, 3, 10, 3, 2);
    } else {
        canvas_draw_box(canvas, 3, 5, 2, 3);
        canvas_draw_line(canvas, 6, 3, 8, 5);
        canvas_draw_line(canvas, 8, 5, 8, 7);
        canvas_draw_line(canvas, 8, 7, 6, 9);
    }
}

static void lab_draw(Canvas* canvas, void* context) {
    IrLabApp* app = context;
    canvas_clear(canvas);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_invert_color(canvas);
    lab_draw_icon(canvas, app->drawn_screen);
    canvas_set_font(canvas, FontPrimary);
    lab_fit_text(canvas, 15, 10, app->lines[0]);
    canvas_invert_color(canvas);
    canvas_set_font(canvas, FontSecondary);
    bool five = app->drawn_screen == LabSlots || app->drawn_screen == LabCompare;
    unsigned visible = five ? 5 : 4;
    for(unsigned i = 1; i <= visible; ++i) {
        int y = five ? 20 + (i - 1) * 8 : 22 + (i - 1) * 10;
        int top = five ? 14 + (i - 1) * 8 : 14 + (i - 1) * 10;
        if(app->highlighted == (int)i) {
            canvas_draw_box(canvas, 0, top, 124, five ? 8 : 10);
            canvas_invert_color(canvas);
        }
        if(app->drawn_screen == LabHex && app->hex_count && (i == 2 || i == 3)) {
            unsigned first = (i - 2) * 4;
            for(unsigned c = 0; c < 4 && first + c < app->hex_count; ++c) {
                bool selected = first + c == app->hex_selected;
                if(selected) {
                    canvas_draw_box(canvas, 2 + c * 28, top, 25, 10);
                    canvas_invert_color(canvas);
                }
                canvas_draw_str(canvas, 6 + c * 28, y, app->hex_cells[first + c]);
                if(selected) canvas_invert_color(canvas);
            }
        } else
            lab_fit_text(canvas, 3, y, app->lines[i]);
        if(app->highlighted == (int)i) canvas_invert_color(canvas);
    }
    if(app->menu_count > visible) {
        canvas_draw_line(canvas, 127, 14, 127, 53);
        unsigned height = 40 * visible / app->menu_count;
        if(height < 3) height = 3;
        unsigned top = 14 + (40 - height) * app->drawn_row / (app->menu_count - 1);
        canvas_draw_box(canvas, 125, top, 3, height);
    }
    canvas_draw_line(canvas, 0, 54, 127, 54);
    lab_fit_text(canvas, 3, 62, app->lines[6]);
    furi_mutex_release(app->mutex);
}

static void lab_input(InputEvent* event, void* context) {
    IrLabApp* app = context;
    if((event->key == InputKeyOk || event->key == InputKeyBack) && event->type == InputTypePress &&
       __atomic_load_n(&app->preview_active, __ATOMIC_RELAXED)) {
        __atomic_store_n(&app->preview_stop_requested, true, __ATOMIC_RELAXED);
        return;
    }
    if(event->key == InputKeyBack && event->type == InputTypeLong)
        __atomic_store_n(&app->stop_requested, true, __ATOMIC_RELAXED);
    else if(
        event->type == InputTypeShort ||
        (event->key == InputKeyOk && event->type == InputTypeLong) ||
        ((event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyLeft ||
          event->key == InputKeyRight) &&
         event->type == InputTypeRepeat))
        furi_message_queue_put(app->inputs, event, 0);
}

static void lab_back(IrLabApp* app) {
    switch(app->screen) {
    case LabProtocol:
        lab_go(app, app->protocol_return, 0);
        break;
    case LabProtocolActions:
        lab_go(app, LabProtocol, 0);
        break;
    case LabProtocolTiming:
    case LabProtocolChecks:
    case LabProtocolFields:
        lab_go(app, LabProtocolActions, 0);
        break;
    case LabProtocolField:
        lab_go(app, LabProtocolFields, app->protocol_byte);
        break;
    case LabHome:
        app->running = false;
        break;
    case LabCapture:
        lab_rx_stop(app);
        app->range_running = app->range_pending = false;
        lab_go(app, app->capture_sniff ? LabSniff : app->seed.valid ? LabHex : LabHome, 0);
        break;
    case LabSniffLabel:
        lab_go(app, LabSniff, 0);
        break;
    case LabProfileMaker:
        lab_go(app, app->profile_maker_return, 0);
        break;
    case LabProfileSignal:
        lab_go(app, LabProfileSignals, app->profile_selected);
        break;
    case LabProfileSignals:
        lab_go(app, LabSniff, 1);
        break;
    case LabSniff:
    case LabProfiles:
        lab_go(app, LabHome, 0);
        break;
    case LabSeries:
    case LabSlots:
    case LabSettings:
        lab_go(app, LabHome, 0);
        break;
    case LabHex:
        lab_go(
            app,
            app->seed_from_profile && app->seed_profile_id == app->profile.id &&
                    app->profile_selected < app->profile.count &&
                    app->profile.entries[app->profile_selected].file_id ==
                        app->seed_profile_file_id ?
                LabProfileSignal :
                LabHome,
            0);
        break;
    case LabSlotAction:
        lab_go(app, LabSlots, app->selected_slot);
        break;
    case LabCompare:
        lab_go(app, LabSlots, app->selected_slot);
        break;
    case LabCompareAction:
    case LabPattern:
        lab_go(app, LabCompare, app->selected_slot);
        break;
    case LabSignal:
    case LabMore:
    case LabFrames:
    case LabSend:
    case LabInfo:
        lab_go(app, LabHex, 0);
        break;
    case LabRange:
        lab_go(app, LabHex, 0);
        break;
    case LabFuzzPresets:
        lab_go(app, LabRange, LabRangePreset);
        break;
    case LabFuzzPick:
    case LabFuzzBytes:
    case LabFuzzOptions:
        lab_go(app, LabRange, LabRangeSteps);
        break;
    case LabFuzzByte:
        lab_go(app, LabFuzzBytes, app->rule_selected);
        break;
    case LabRangeValue:
        lab_go(app, LabFuzzByte, app->range_field);
        break;
    case LabRangeRun:
        if(app->range_live) {
            if(app->auto_started) lab_auto_pause(app, "user_back", false);
            app->range_live = app->range_running = false;
            __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
            lab_go(app, app->auto_started ? LabHex : LabRange, 0);
            break;
        }
        if(app->range_simulation) {
            app->range_simulation = app->range_running = false;
            __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
            lab_go(app, LabRange, LabRangeMore);
            break;
        }
        // fall through
    case LabResult:
        app->range_running = app->range_pending = false;
        lab_go(app, LabHex, 0);
        break;
    case LabCarrier:
        lab_go(app, LabSettings, 1);
        break;
    case LabHelp:
        lab_go(app, LabSettings, 3);
        break;
    case LabMessage:
        if(app->message_return != LabRangeRun) app->range_running = app->range_pending = false;
        lab_go(app, app->message_return, 0);
        break;
    }
}

static void lab_handle_input(IrLabApp* app, const InputEvent* event) {
    if(event->key == InputKeyBack) {
        lab_back(app);
        return;
    }
    if(app->screen == LabCapture) return;
    if(app->screen == LabRangeRun && app->range_live) {
        if(event->key == InputKeyOk && event->type == InputTypeLong) {
            if(app->preview_press_consumed)
                app->preview_press_consumed = false;
            else if(!app->auto_started && !app->auto_paused)
                lab_auto_begin(app);
            else if(app->auto_paused)
                lab_auto_resume(app);
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            if(app->preview_press_consumed)
                app->preview_press_consumed = false;
            else if(app->auto_started && !app->auto_paused)
                lab_auto_pause(app, "user_button", false);
        }
        return;
    }
    if(app->screen == LabRangeRun && app->range_simulation) {
        if(event->key == InputKeyOk && event->type == InputTypeLong) {
            app->preview_press_consumed = false;
            return;
        }
        if(event->key == InputKeyOk && event->type == InputTypeShort) {
            if(!app->preview_paused) {
                app->preview_paused = true;
                __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
            } else if(app->preview_press_consumed)
                app->preview_press_consumed = false;
            else {
                app->range_simulation = app->range_running = false;
                lab_go(app, LabHex, 0);
            }
        }
        return;
    }
    if(app->screen == LabSend || app->screen == LabRangeRun) {
        if(event->key == InputKeyOk && event->type == InputTypeLong) {
            if(!app->gate.armed && !app->gate.locked && app->storage_ready &&
               lab_log(app, "armed", "manual_bench_acknowledgement"))
                app->gate.armed = true;
            if(app->screen == LabRangeRun && app->storage_ready && !app->gate.locked &&
               app->gate.has_sent &&
               (uint32_t)(lab_now_ms() - app->gate.last_ms) < IR_LAB_COOLDOWN_MS)
                return;
            lab_send_trial(app);
        }
        return;
    }
    if(event->type == InputTypeLong) return;
    if(app->screen == LabHex && app->bits.valid) {
        if(event->key == InputKeyLeft || event->key == InputKeyRight) {
            unsigned bytes = (app->bits.length[app->frame] + 7) / 8;
            if(event->key == InputKeyRight && app->byte + 1U < bytes) ++app->byte;
            if(event->key == InputKeyLeft && app->byte) --app->byte;
            return;
        }
        if(event->key == InputKeyUp || event->key == InputKeyDown) {
            unsigned value = lab_byte_value(app, true, app->frame, app->byte);
            if(event->key == InputKeyUp && value < lab_byte_max(app)) ++value;
            if(event->key == InputKeyDown && value) --value;
            lab_set_hex(app, value);
            return;
        }
    }
    if(app->screen == LabRangeValue) {
        if(event->key == InputKeyLeft || event->key == InputKeyRight) {
            unsigned max_cursor = app->range_field == 2 ? 2 : 1;
            if(event->key == InputKeyLeft && app->range_nibble < max_cursor) ++app->range_nibble;
            if(event->key == InputKeyRight && app->range_nibble) --app->range_nibble;
            return;
        }
        if(event->key == InputKeyUp || event->key == InputKeyDown) {
            if(app->range_nibble == 2)
                app->range_edit_negative = !app->range_edit_negative;
            else {
                unsigned shift = app->range_nibble * 4;
                unsigned digit = (app->range_edit >> shift) & 15;
                digit = (digit + (event->key == InputKeyUp ? 1 : 15)) & 15;
                unsigned value = (app->range_edit & ~(15U << shift)) | (digit << shift);
                const IrLabByteRule* r = &app->rules[app->rule_selected];
                unsigned max =
                    app->range_field == 2 ?
                        255 :
                        (1U << ir_lab_window_width(&app->bits, r->frame, r->bit_offset, r->byte)) -
                            1;
                app->range_edit = value > max ? max : value;
            }
            return;
        }
    }
    if(app->screen == LabSlots && event->key == InputKeyRight) {
        app->selected_slot = app->row;
        app->compare_frame = 0;
        lab_open_compare(app);
        return;
    }
    if((app->screen == LabCompare || app->screen == LabPattern) &&
       (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        unsigned bytes = (app->comparison.bits + 7) / 8;
        if(event->key == InputKeyRight && app->compare_byte + 1U < bytes) ++app->compare_byte;
        if(event->key == InputKeyLeft && app->compare_byte) --app->compare_byte;
        return;
    }
    if(app->screen == LabProtocolFields &&
       (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        if(event->key == InputKeyRight && app->protocol_frame + 1 < app->bits.frames)
            ++app->protocol_frame;
        if(event->key == InputKeyLeft && app->protocol_frame) --app->protocol_frame;
        app->row = 0;
        return;
    }
    unsigned count = lab_menu_count(app);
    if(count && (event->key == InputKeyUp || event->key == InputKeyDown)) {
        app->row = event->key == InputKeyDown ? (app->row + 1) % count :
                   app->row                   ? (unsigned)app->row - 1U :
                                                count - 1;
        return;
    }
    if(event->key != InputKeyOk || event->type != InputTypeShort) return;
    switch(app->screen) {
    case LabProtocol:
        lab_go(app, LabProtocolActions, 0);
        break;
    case LabProtocolTiming:
    case LabProtocolChecks:
        lab_go(app, LabProtocolActions, 0);
        break;
    case LabProtocolActions:
        if(app->row == 0)
            lab_use_protocol_preset(app);
        else if(app->row == 1)
            lab_go(app, LabProtocolTiming, 0);
        else if(app->row == 2)
            lab_go(app, LabProtocolChecks, 0);
        else if(app->row == 3)
            lab_go(app, LabProtocolFields, 0);
        else if(app->row == 4)
            lab_protocol_matches(app);
        else
            lab_protocol_save(app);
        break;
    case LabProtocolFields:
        app->protocol_byte = app->row;
        lab_go(app, LabProtocolField, 0);
        break;
    case LabProtocolField:
        lab_protocol_apply_steps(app);
        break;
    case LabHome:
        if(app->row == LabHomeCapture) {
            app->capture_batch = false;
            app->capture_slot = app->active_slot;
            lab_capture_start(app, false);
        } else if(app->row == LabHomeSets)
            lab_go(app, LabSeries, app->series);
        else if(app->row == LabHomeFuzzer) {
            if(app->seed.valid)
                lab_open_fuzzer(app);
            else
                lab_message(
                    app,
                    LabHome,
                    "Capture first",
                    "Choose Capture signal",
                    "Press a remote button",
                    "");
        } else if(app->row == LabHomeSniff) {
            if(app->profile_open)
                lab_go(app, LabSniff, 0);
            else
                lab_sniff_new(app);
        } else if(app->row == LabHomeProfiles)
            lab_profiles_open(app);
        else if(app->row == LabHomeAnalyze)
            lab_open_protocol(app);
        else
            lab_go(app, LabSettings, 0);
        break;
    case LabSniff:
        if(app->row == 0) {
            if(app->profile.count >= IR_LAB_PROFILE_SIGNALS)
                lab_message(
                    app,
                    LabSniff,
                    "Profile is full",
                    "64 signals saved",
                    "Start a new profile",
                    "");
            else
                lab_capture_start(app, false);
        } else if(app->row == 1) {
            if(app->profile.count)
                lab_go(app, LabProfileSignals, 0);
            else
                lab_message(app, LabSniff, "No labeled signals", "Capture a button first", "", "");
        } else if(app->row == 2) {
            if(app->profile.count) {
                app->profile_maker_return = LabSniff;
                lab_go(app, LabProfileMaker, app->profile.maker);
            } else
                lab_message(app, LabSniff, "No saved signals", "Capture a button first", "", "");
        } else if(app->row == 4)
            lab_profile_dedup(app);
        else
            lab_sniff_new(app);
        break;
    case LabProfileMaker:
        if(app->profile_maker_return != LabSniff)
            lab_set_profile_save(app, app->row);
        else if(ir_lab_profile_set_maker(app->storage, LAB_PROFILES, &app->profile, app->row)) {
            if(app->seed_from_profile && app->seed_profile_id == app->profile.id)
                app->seed_profile_maker = app->profile.maker;
            app->profile_open = false;
            lab_go(app, LabHome, 0);
        } else
            lab_message(
                app,
                LabProfileMaker,
                "Maker save failed",
                "Existing profile kept",
                "Check SD and retry",
                "No IR sent");
        break;
    case LabSniffLabel:
        lab_sniff_save(app, app->row);
        break;
    case LabProfiles:
        lab_profile_load(app, app->profile_ids[app->row]);
        break;
    case LabProfileSignals:
        lab_profile_activate(app, app->row);
        break;
    case LabProfileSignal:
        if(app->row == 0) {
            ir_lab_protocol_analyze(&app->seed, &app->bits, &app->protocol_analysis);
            lab_use_protocol_preset(app);
        } else if(app->row == 1)
            lab_open_protocol(app);
        else if(app->row == 2) {
            app->mutation.mode = IrLabReplay;
            lab_open_send(app);
        } else
            lab_go(app, LabHex, 0);
        break;
    case LabSeries:
        lab_set_open(app, app->row);
        break;
    case LabSlots:
        if(app->row == lab_set_capacity(app->series)) {
            lab_set_profile_begin(app);
            break;
        }
        app->selected_slot = app->row;
        if(app->slots[app->selected_slot].count)
            lab_go(app, LabSlotAction, 0);
        else {
            app->capture_slot = app->selected_slot;
            app->capture_batch = true;
            lab_capture_start(app, false);
        }
        break;
    case LabSlotAction:
        if(app->row == 2) {
            app->capture_slot = app->selected_slot;
            app->capture_batch = true;
            lab_capture_start(app, false);
        } else if(lab_slot_load(app, app->selected_slot)) {
            if(app->row == 1) {
                app->mutation.mode = IrLabReplay;
                lab_open_send(app);
            } else if(app->row == 3)
                lab_open_fuzzer(app);
            else if(app->row == 4)
                lab_open_protocol(app);
            else
                lab_go(app, LabHex, 0);
        }
        break;
    case LabCompare:
        app->selected_slot = app->row;
        lab_go(app, LabCompareAction, 0);
        break;
    case LabCompareAction:
        if(app->row == 5)
            lab_set_profile_begin(app);
        else if(app->row < 3) {
            unsigned action = app->row;
            if(action < 2 &&
               !ir_lab_view_width(
                   &app->views[app->selected_slot], app->compare_frame, app->compare_byte)) {
                lab_message(
                    app,
                    LabCompare,
                    "Data unavailable",
                    "Choose a matching row",
                    "Frame / byte retained",
                    "");
                break;
            }
            if(lab_slot_load(app, app->selected_slot)) {
                if(ir_lab_byte_width(&app->bits, app->compare_frame, app->compare_byte)) {
                    app->frame = app->compare_frame;
                    app->byte = app->compare_byte;
                }
                if(action == 0)
                    lab_open_fuzzer(app);
                else if(action == 1)
                    lab_go(app, LabHex, 0);
                else {
                    app->mutation.mode = IrLabReplay;
                    lab_open_send(app);
                }
            } else if(!app->gate.pending_observation && app->screen != LabMessage)
                lab_message(
                    app, LabCompare, "Empty capture slot", "Choose a captured row", "", "");
        } else if(app->row == 3) {
            unsigned next = app->compare_frame;
            for(unsigned n = 1; n <= IR_LAB_MAX_FRAMES; ++n) {
                app->compare_frame = (next + n) % IR_LAB_MAX_FRAMES;
                if(lab_comparison(app) >= 2) break;
            }
            lab_open_compare_frame(app);
        } else
            lab_go(app, LabPattern, 0);
        break;
    case LabPattern:
        lab_go(app, LabCompare, app->selected_slot);
        break;
    case LabHex:
        lab_go(app, LabSignal, 0);
        break;
    case LabSignal:
        if(app->row == 0)
            lab_open_fuzzer(app);
        else if(app->row == 1)
            lab_open_protocol(app);
        else if(app->row < 4) {
            app->range_running = app->range_pending = app->range_simulation = app->range_live =
                false;
            __atomic_store_n(&app->preview_active, false, __ATOMIC_RELAXED);
            if(app->row == 2 && !lab_editable(app)) break;
            app->mutation.mode = app->row == 2 ? IrLabDataBits : IrLabReplay;
            lab_open_send(app);
        } else
            lab_go(app, LabMore, 0);
        break;
    case LabMore:
        if(app->row == 0) {
            if(lab_editable(app)) lab_go(app, LabFrames, app->frame);
        } else if(app->row == 1) {
            if(lab_editable(app)) {
                lab_set_hex(app, lab_byte_value(app, false, app->frame, app->byte));
                lab_go(app, LabHex, 0);
            }
        } else if(app->row == 2) {
            memset(app->changes, 0, sizeof(app->changes));
            app->mutation.mode = IrLabDataBits;
            lab_go(app, LabHex, 0);
        } else if(app->row == 3) {
            app->mutation.mode = IrLabDataBits;
            if(!lab_editable(app) || !lab_candidate(app)) break;
            char name[32];
            snprintf(name, sizeof(name), "edited-%03lu", (unsigned long)++app->capture_id);
            if(lab_save_signal(app, &app->candidate, name) && lab_log(app, "edit_saved", name))
                lab_message(app, LabHex, "Hex copy saved", name, "In the session folder", "");
            else {
                app->storage_ready = false;
                ir_lab_stop(&app->gate);
                lab_message(
                    app, LabHex, "Save failed", "Check the SD card", "Sending is locked", "");
            }
        } else if(app->row == 4) {
            app->capture_slot = app->active_slot;
            app->capture_batch = false;
            lab_capture_start(app, false);
        } else
            lab_go(app, LabInfo, 0);
        break;
    case LabFrames:
        app->frame = app->row;
        app->byte = 0;
        lab_go(app, LabHex, 0);
        break;
    case LabRange:
        if(app->row == LabRangeStart)
            lab_start_auto(app);
        else if(app->row == LabRangePreset)
            lab_go(
                app,
                LabFuzzPresets,
                app->fuzz_preset == IrLabProtocolToto ? 1 :
                app->fuzz_preset == IrLabProtocolInax ? 2 :
                                                        0);
        else if(app->row == LabRangeFrom || app->row == LabRangeTo) {
            app->fuzz_pick_end = app->row == LabRangeTo;
            lab_go(app, LabFuzzPick, 0);
        } else if(app->row == LabRangeSteps) {
            app->range_field = 2;
            lab_go(app, LabFuzzBytes, app->rule_selected);
        } else if(app->row == LabRangeAnalyze)
            lab_open_protocol(app);
        else if(app->row == LabRangeDelay)
            app->preview_delay = app->preview_delay == 2 ? 1 : 2;
        else
            lab_go(app, LabFuzzOptions, 0);
        break;
    case LabFuzzPresets:
        lab_open_fuzzer_preset(
            app,
            app->row == 1 ? IrLabProtocolToto :
            app->row == 2 ? IrLabProtocolInax :
                            IrLabProtocolUnknown);
        break;
    case LabFuzzPick:
        if(app->row == 0) {
            app->range_field = app->fuzz_pick_end ? 1 : 0;
            lab_go(app, LabFuzzBytes, app->rule_selected);
        } else {
            unsigned slot = lab_fuzz_pick_slot(app, app->row);
            if(app->fuzz_pick_end) {
                if(lab_fuzz_end_capture(app, slot)) lab_go(app, LabRange, LabRangeSteps);
            } else {
                IrLabProtocolKind preset = app->fuzz_preset;
                if(lab_slot_load(app, slot)) {
                    IrLabProtocolAnalysis analysis;
                    ir_lab_protocol_analyze(&app->seed, &app->bits, &analysis);
                    lab_open_fuzzer_preset(
                        app, analysis.kind == preset ? preset : IrLabProtocolUnknown);
                }
            }
        }
        break;
    case LabFuzzBytes:
        app->rule_selected = app->row;
        if(app->fuzz_keep_check && app->rules[app->rule_selected].byte == 2)
            lab_message(
                app,
                LabFuzzBytes,
                "B3 is automatic",
                app->fuzz_preset == IrLabProtocolToto ? "P3=P1 XOR P2" : "B3=B2 XOR FF",
                "More options: check off",
                "Then edit it manually");
        else
            lab_go(app, LabFuzzByte, app->range_field);
        break;
    case LabFuzzByte:
        if(app->row == 3)
            lab_go(app, LabFuzzBytes, app->rule_selected);
        else {
            const IrLabByteRule* r = &app->rules[app->rule_selected];
            app->range_field = app->row;
            app->range_edit = app->row == 0 ? r->from :
                              app->row == 1 ? r->to :
                              r->step < 0   ? -r->step :
                                              r->step;
            app->range_edit_negative = app->row == 2 && r->step < 0;
            app->range_nibble = 0;
            lab_go(app, LabRangeValue, 0);
        }
        break;
    case LabRangeValue:
        lab_fuzz_set_edit(app);
        break;
    case LabFuzzOptions:
        if(app->row == 0)
            app->range_repeat = !app->range_repeat;
        else if(app->row == 1)
            lab_start_preview(app);
        else if(app->row == 2)
            lab_start_range(app);
        else if(app->row == 4)
            lab_invert_command(app);
        else if(
            app->fuzz_preset == IrLabProtocolToto ||
            (app->fuzz_preset == IrLabProtocolInax && app->protocol_analysis.checks_tested &&
             app->protocol_analysis.checks_passed == app->protocol_analysis.checks_tested)) {
            app->fuzz_keep_check = !app->fuzz_keep_check;
            if(app->fuzz_keep_check)
                for(unsigned n = 0; n < app->rule_count; ++n)
                    if(app->rules[n].byte == 2) {
                        app->rules[n].to = app->rules[n].from;
                        app->rules[n].step = 0;
                    }
        } else
            lab_message(
                app,
                LabFuzzOptions,
                "No automatic check",
                "Need a matching preset",
                "INAX pair must fit seed",
                "Manual byte steps retained");
        break;
    case LabResult: {
        static const char* const outcomes[] = {
            "no_observed_response",
            "beep_heard_manual",
            "action_observed_manual",
            "unexpected_response"};
        unsigned outcome = app->row;
        if(outcome == 3) lab_emergency_stop(app);
        if(lab_log(app, "observation", outcomes[outcome])) {
            ir_lab_observe(&app->gate, outcome == 3);
            if(outcome == 3) break;
            if(app->range_running && app->range_pending) {
                app->range_pending = false;
                ++app->range_index;
                if(app->range_repeat) app->range_index %= lab_range_count(app);
                if(app->range_index < lab_range_count(app)) {
                    lab_range_apply(app);
                    lab_go(app, LabRangeRun, 0);
                } else {
                    app->range_running = false;
                    char completed[40];
                    snprintf(completed, 40, "%u values sent", app->range_index);
                    lab_message(
                        app, LabHex, "Range complete", completed, "Last hex value kept", "");
                }
            } else
                lab_go(app, LabHex, 0);
        }
        break;
    }
    case LabSettings:
        if(app->row == 0) {
            lab_capture_start(app, true);
        } else if(app->row == 1)
            lab_go(app, LabCarrier, app->carrier / 1000 - 20);
        else if(app->row == 2) {
            char trials[32];
            snprintf(
                trials, sizeof(trials), "Sent/reserved: %lu", (unsigned long)app->gate.trials);
            lab_message(
                app,
                LabSettings,
                "Session status",
                trials,
                app->gate.locked ? "Sending locked" : "Auto gap: 1 or 2 seconds",
                app->gate.pending_observation ? "Result still required" : "No pending result");
        } else
            lab_go(app, LabHelp, 0);
        break;
    case LabCarrier:
        app->carrier = (app->row + 20) * 1000;
        lab_go(app, LabSettings, 1);
        break;
    case LabMessage:
        lab_go(app, app->message_return, 0);
        break;
    case LabInfo:
        lab_go(app, LabHex, 0);
        break;
    case LabHelp:
        lab_go(app, LabSettings, 3);
        break;
    default:
        break;
    }
}

int32_t ir_boundary_lab_app(void* argument) {
    UNUSED(argument);
    IrLabApp* app = calloc(1, sizeof(IrLabApp));
    furi_check(app);
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->inputs = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->worker = infrared_worker_alloc();
    app->view = view_port_alloc();
    app->carrier = 38000;
    app->series = 3;
    lab_sets_init(app);
    app->preview_delay = 1;
    app->running = true;
    app->storage_ready = lab_storage_init(app);
    infrared_worker_rx_set_received_signal_callback(app->worker, lab_received, app);
    view_port_draw_callback_set(app->view, lab_draw, app);
    view_port_input_callback_set(app->view, lab_input, app);
    gui_add_view_port(app->gui, app->view, GuiLayerFullscreen);
    while(app->running) {
        if(__atomic_exchange_n(&app->stop_requested, false, __ATOMIC_RELAXED))
            lab_emergency_stop(app);
        lab_capture_poll(app);
        lab_preview_poll(app);
        lab_auto_poll(app);
        lab_refresh(app);
        InputEvent event;
        if(furi_message_queue_get(app->inputs, &event, furi_ms_to_ticks(50)) == FuriStatusOk &&
           !__atomic_load_n(&app->stop_requested, __ATOMIC_RELAXED))
            lab_handle_input(app, &event);
    }
    lab_rx_stop(app);
    if(app->storage_ready)
        lab_log(app, "exit", app->gate.pending_observation ? "observation_missing" : "normal");
    view_port_enabled_set(app->view, false);
    gui_remove_view_port(app->gui, app->view);
    view_port_free(app->view);
    infrared_worker_free(app->worker);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->inputs);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
