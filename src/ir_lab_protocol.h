#pragma once
#include "ir_lab_core.h"

typedef enum {
    IrLabProtocolUnknown,
    IrLabProtocolPulseDistance,
    IrLabProtocolSirc,
    IrLabProtocolNec,
    IrLabProtocolNecExtended,
    IrLabProtocolNecExt,
    IrLabProtocolToto,
    IrLabProtocolInax,
    IrLabProtocolBio71,
} IrLabProtocolKind;

typedef struct {
    IrLabProtocolKind kind;
    bool valid;
    uint8_t frames, unique_frames, payload_offset;
    uint16_t frame_bits;
    uint32_t header_mark, header_space, constant, short_symbol, long_symbol;
    uint8_t checks_passed, checks_tested;
} IrLabProtocolAnalysis;

bool ir_lab_protocol_analyze(
    const IrLabSignal* seed,
    const IrLabBits* bits,
    IrLabProtocolAnalysis* result);
const char* ir_lab_protocol_name(IrLabProtocolKind kind);
const char* ir_lab_protocol_check(IrLabProtocolKind kind);
bool ir_lab_protocol_repair_toto(const IrLabBits* bits, uint8_t* changes);
// Only use the inverse-pair rule when it holds in every original INAX frame.
bool ir_lab_protocol_repair_inax(const IrLabBits* bits, uint8_t* changes);
bool ir_lab_protocol_view(const IrLabDataView* source, unsigned offset, IrLabDataView* output);
unsigned ir_lab_protocol_distance(const IrLabDataView* a, const IrLabDataView* b);
