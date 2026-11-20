#pragma once
#include "ir_lab_protocol.h"

typedef struct Storage Storage;

#define IR_LAB_MAX_PROFILES    64U
#define IR_LAB_PROFILE_SIGNALS 64U

// These values are persisted on SD; append new makers without renumbering.
typedef enum {
    IrLabMakerUnknown,
    IrLabMakerLixil,
    IrLabMakerToto,
    IrLabMakerPanasonic,
    IrLabMakerBioBidet,
    IrLabMakerToshiba,
    IrLabMakerOther,
    IrLabMakerCount,
} IrLabMaker;

typedef enum {
    IrLabActionFlushBig,
    IrLabActionFlushSmall,
    IrLabActionMusicStart,
    IrLabActionMusicStop,
    IrLabActionRearWash,
    IrLabActionFrontWash,
    IrLabActionWashStop,
    IrLabActionMassage,
    IrLabActionPressureUp,
    IrLabActionPressureDown,
    IrLabActionWaterTemp,
    IrLabActionSeatTemp,
    IrLabActionNozzleClean,
    IrLabActionPositionForward,
    IrLabActionPositionBack,
    IrLabActionOscillate,
    IrLabActionDryStart,
    IrLabActionDryStop,
    IrLabActionEco,
    IrLabActionOther,
    IrLabActionPressure,
    IrLabActionCount,
} IrLabAction;

typedef struct {
    uint8_t action, protocol;
    uint16_t timings;
    uint32_t frequency;
    float duty_cycle;
    uint32_t file_id, signature;
} IrLabProfileEntry;

typedef struct {
    uint8_t id, count, maker, retired_count;
    uint32_t next_id;
    uint32_t retired[IR_LAB_PROFILE_SIGNALS];
    IrLabProfileEntry entries[IR_LAB_PROFILE_SIGNALS];
} IrLabProfile;

const char* ir_lab_action_name(unsigned action);
const char* ir_lab_maker_name(unsigned maker);
// Raw captures keep stable file IDs. profile.meta stores visible IDs and maker.
// Index changes publish a synced temporary file before redundant raw captures
// are deleted. Existing v0.15 profiles load without rewriting original IR.
bool ir_lab_profile_create(Storage* storage, const char* root, IrLabProfile* output);
unsigned ir_lab_profile_list(Storage* storage, const char* root, uint8_t* ids, uint8_t* makers);
bool ir_lab_profile_load(Storage* storage, const char* root, unsigned id, IrLabProfile* output);
bool ir_lab_profile_append(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    const IrLabSignal* signal,
    unsigned action);
bool ir_lab_profile_read(
    Storage* storage,
    const char* root,
    const IrLabProfile* profile,
    unsigned index,
    IrLabSignal* signal);
bool ir_lab_profile_set_maker(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    unsigned maker);
typedef bool (
    *IrLabProfileSource)(void* context, unsigned index, IrLabSignal* signal, unsigned* action);
// Publish a new profile only after every original signal has been saved.
// On failure output is unchanged and any unfinished profile stays hidden.
bool ir_lab_profile_import(
    Storage* storage,
    const char* root,
    unsigned count,
    unsigned maker,
    IrLabProfileSource source,
    void* context,
    IrLabProfile* output);
// Only same-label, same-data complete transmissions are duplicates. Supported
// encodings allow up to 15% timing jitter; undecodable waves must match exactly.
// A committed index is published before redundant files are removed.
bool ir_lab_profile_deduplicate(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    unsigned* removed,
    bool* cleanup_pending);
