#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IR_LAB_MAX_TIMINGS     1024U
#define IR_LAB_MAX_DURATION_US 750000U
#define IR_LAB_COOLDOWN_MS     10000U
// Stop only at counter exhaustion, so trial IDs cannot wrap and overwrite files.
#define IR_LAB_MAX_TRIALS      UINT32_MAX

typedef struct {
    bool valid;
    bool decoded;
    int32_t protocol;
    uint8_t address_bits;
    uint8_t command_bits;
    uint32_t address;
    uint32_t command;
    uint32_t frequency;
    float duty_cycle;
    size_t count;
    uint32_t timings[IR_LAB_MAX_TIMINGS];
} IrLabSignal;

typedef enum {
    IrLabReplay,
    IrLabCommandBit,
    IrLabCommandField,
    IrLabRawScale,
    IrLabRawSet,
    IrLabDataBits,
    IrLabModeCount,
} IrLabMode;

typedef struct {
    IrLabMode mode;
    uint16_t index; // Command LSB or zero-based raw timing index.
    uint8_t width;
    int32_t value; // Field value, percentage delta, or absolute microseconds.
} IrLabMutation;

typedef struct {
    bool armed;
    bool locked;
    bool pending_observation;
    bool has_sent;
    uint32_t last_ms;
    uint32_t trials;
} IrLabGate;

bool ir_lab_signal_valid(const IrLabSignal* signal);
bool ir_lab_mutate(const IrLabSignal* seed, const IrLabMutation* mutation, IrLabSignal* output);
bool ir_lab_can_send(const IrLabGate* gate, uint32_t now_ms);
bool ir_lab_reserve_trial(IrLabGate* gate, uint32_t now_ms);
bool ir_lab_can_send_range(const IrLabGate* gate, uint32_t now_ms, uint32_t delay_ms);
bool ir_lab_reserve_range_trial(IrLabGate* gate, uint32_t now_ms, uint32_t delay_ms);
void ir_lab_observe(IrLabGate* gate, bool unexpected);
void ir_lab_stop(IrLabGate* gate);
const char* ir_lab_mode_name(IrLabMode mode);

#define IR_LAB_MAX_BITS     512U
#define IR_LAB_MASK_BYTES   (IR_LAB_MAX_BITS / 8U)
#define IR_LAB_MAX_FRAMES   8U
#define IR_LAB_MAX_CAPTURES 5U

// Bit numbering follows arrival order. Bit 0 is the first data cell after
// the header, independent of any vendor's byte order or protocol semantics.
typedef struct {
    bool valid;
    uint16_t count;
    uint8_t frames;
    uint8_t repeats; // Unmodified NEC-style short repeat frames, not editable data.
    uint8_t error;
    bool pulse_width; // True: selected timings are data marks, not data spaces.
    uint16_t start[IR_LAB_MAX_FRAMES];
    uint16_t length[IR_LAB_MAX_FRAMES];
    uint32_t short_symbol[IR_LAB_MAX_FRAMES];
    uint32_t long_symbol[IR_LAB_MAX_FRAMES];
    uint16_t timing[IR_LAB_MAX_BITS];
    uint8_t data[IR_LAB_MASK_BYTES];
} IrLabBits;

// Compact immutable data for capture comparison; no timing-to-command inference.
typedef struct {
    bool valid;
    bool pulse_width;
    uint16_t count;
    uint8_t frames;
    uint16_t start[IR_LAB_MAX_FRAMES];
    uint16_t length[IR_LAB_MAX_FRAMES];
    uint8_t data[IR_LAB_MASK_BYTES];
} IrLabDataView;

typedef struct {
    uint8_t comparable_mask;
    uint8_t different_layout_mask;
    uint16_t bits;
    uint16_t changing_bits;
    uint8_t varying[IR_LAB_MASK_BYTES];
} IrLabComparison;

void ir_lab_data_view(const IrLabBits* bits, IrLabDataView* view);
unsigned ir_lab_view_width(const IrLabDataView* view, unsigned frame, unsigned byte);
uint8_t ir_lab_view_byte(const IrLabDataView* view, unsigned frame, unsigned byte);
unsigned ir_lab_compare(
    const IrLabDataView* views,
    unsigned count,
    unsigned frame,
    IrLabComparison* comparison);
bool ir_lab_hex_sequence(
    const IrLabDataView* views,
    unsigned count,
    unsigned frame,
    unsigned byte,
    int16_t* step);
// Signed adjacent subtraction, never modulo wrap. Reject missing/mismatched data.
bool ir_lab_hex_delta(
    const IrLabDataView* earlier,
    const IrLabDataView* later,
    unsigned frame,
    unsigned byte,
    int16_t* delta);

bool ir_lab_extract_bits(const IrLabSignal* seed, IrLabBits* bits);
bool ir_lab_bit_get(const uint8_t* data, uint16_t index);
uint8_t ir_lab_bit_frame(const IrLabBits* bits, uint16_t index);
unsigned ir_lab_toggle_linked(const IrLabBits* bits, uint8_t* changes, uint16_t index);
unsigned ir_lab_set_linked(const IrLabBits* bits, uint8_t* changes, uint16_t index, bool value);
// Arrival-order cells, first cell in the low bit; NULL changes reads the seed.
// A final partial byte accepts only its observed bit width.
unsigned ir_lab_byte_width(const IrLabBits* bits, unsigned frame, unsigned byte);
uint8_t
    ir_lab_byte_value(const IrLabBits* bits, const uint8_t* changes, unsigned frame, unsigned byte);
bool ir_lab_set_byte_linked(
    const IrLabBits* bits,
    uint8_t* changes,
    unsigned frame,
    unsigned byte,
    unsigned value);
// A byte window can start after a non-byte-aligned prefix (e.g. TOTO's 15 cells).
unsigned
    ir_lab_window_width(const IrLabBits* bits, unsigned frame, unsigned offset, unsigned byte);
uint8_t ir_lab_window_value(
    const IrLabBits* bits,
    const uint8_t* changes,
    unsigned frame,
    unsigned offset,
    unsigned byte);
bool ir_lab_window_set(
    const IrLabBits* bits,
    uint8_t* changes,
    unsigned frame,
    unsigned offset,
    unsigned byte,
    unsigned value);
// A synchronized plan contains one signed rule per independent data byte.
#define IR_LAB_MAX_BYTE_RULES (IR_LAB_MASK_BYTES + IR_LAB_MAX_FRAMES - 1U)
typedef struct {
    uint8_t frame, byte, from, to;
    uint8_t bit_offset; // Leading observed cells retained outside this byte window.
    int16_t step; // Signed hex magnitude; zero holds a byte fixed.
} IrLabByteRule;
// Returns zero for invalid width, step, direction, or overlapping repeat aliases.
// All active bytes advance together. Stop before the first endpoint is crossed.
unsigned ir_lab_byte_plan_count(const IrLabBits* bits, const IrLabByteRule* rules, unsigned count);
bool ir_lab_byte_plan_apply(
    const IrLabBits* bits,
    const IrLabByteRule* rules,
    unsigned count,
    const uint8_t* base,
    unsigned index,
    uint8_t* changes);
// Positive step; direction follows from/to. Includes to only if reached exactly.
unsigned ir_lab_hex_range_count(unsigned from, unsigned to, unsigned step);
bool ir_lab_hex_range_value(
    unsigned from,
    unsigned to,
    unsigned step,
    unsigned index,
    uint8_t* value);
// Synchronized second field. No wrapping or clamping; end at the first bound.
unsigned ir_lab_hex_linked_count(
    unsigned from,
    unsigned to,
    unsigned step,
    unsigned linked_from,
    unsigned linked_step,
    bool decrease,
    unsigned width);
bool ir_lab_hex_linked_value(
    unsigned from,
    unsigned step,
    bool decrease,
    unsigned width,
    unsigned index,
    uint8_t* value);
// Identical original repeat frames share edits, including equal byte positions.
bool ir_lab_byte_targets_overlap(
    const IrLabBits* bits,
    unsigned frame_a,
    unsigned byte_a,
    unsigned frame_b,
    unsigned byte_b);
const char* ir_lab_bits_error(const IrLabBits* bits);
unsigned ir_lab_changed_count(const IrLabBits* bits, const uint8_t* changes);
bool ir_lab_apply_bits(
    const IrLabSignal* seed,
    const IrLabBits* bits,
    const uint8_t* changes,
    IrLabSignal* output);
