#include "ir_lab_protocol.h"
#include <string.h>

// Framing references: crankyoldgit/IRremoteESP8266, ir_Toto.cpp and ir_Inax.cpp.
// This is an independent C implementation. The INAX inverse pair is supported
// by our reference captures; upstream defines timing/length, not that relation.

static bool near(uint32_t value, uint32_t expected) {
    return value * 100U >= expected * 75U && value * 100U <= expected * 125U;
}

const char* ir_lab_protocol_name(IrLabProtocolKind kind) {
    static const char* const names[] = {
        "Unknown",
        "Pulse-distance",
        "SIRC-like",
        "NEC32 fit",
        "NEC addr16 fit",
        "NECext32-like",
        "TOTO39 fit",
        "INAX24-like",
        "Bio71 candidate"};
    return (unsigned)kind <= IrLabProtocolBio71 ? names[kind] : "Unknown";
}

const char* ir_lab_protocol_check(IrLabProtocolKind kind) {
    switch(kind) {
    case IrLabProtocolToto:
        return "P3=P1 XOR P2";
    case IrLabProtocolInax:
        return "B2 XOR B3=FF";
    case IrLabProtocolBio71:
        return "B8=sum(B1..B5)";
    case IrLabProtocolNec:
    case IrLabProtocolNecExtended:
        return "B3 XOR B4=FF";
    default:
        return "No check established";
    }
}

bool ir_lab_protocol_analyze(
    const IrLabSignal* seed,
    const IrLabBits* bits,
    IrLabProtocolAnalysis* result) {
    if(!result) return false;
    memset(result, 0, sizeof(*result));
    if(!seed || !bits || !bits->valid || !bits->frames || bits->frames > IR_LAB_MAX_FRAMES ||
       bits->count > IR_LAB_MAX_BITS || !ir_lab_signal_valid(seed) || seed->decoded)
        return false;
    result->frames = bits->frames;
    result->frame_bits = bits->length[0];
    bool same_width = true, toto = !bits->pulse_width, bio = !bits->pulse_width;
    bool nec = true, sirc = bits->pulse_width;
    unsigned address_inverse = 0, command_inverse = 0;
    for(unsigned f = 0; f < bits->frames; ++f) {
        if(!bits->length[f] || bits->start[f] + bits->length[f] > bits->count) return false;
        unsigned t = bits->timing[bits->start[f]], head = bits->pulse_width ? 2 : 3;
        if(t < head || t >= seed->count || (bits->pulse_width && t + 1 >= seed->count))
            return false;
        uint32_t hm = seed->timings[t - head], hs = seed->timings[t - head + 1];
        same_width &= bits->length[f] == result->frame_bits;
        result->header_mark += hm;
        result->header_space += hs;
        result->short_symbol += bits->short_symbol[f];
        result->long_symbol += bits->long_symbol[f];
        result->constant += seed->timings[bits->pulse_width ? t + 1 : t - 1];
        bool duplicate = false;
        for(unsigned prior = 0; prior < f; ++prior)
            duplicate |= ir_lab_byte_targets_overlap(bits, prior, 0, f, 0);
        result->unique_frames += !duplicate;
        uint16_t prefix = 0;
        for(unsigned b = 0; b < 15 && b < bits->length[f]; ++b)
            prefix |= (unsigned)ir_lab_bit_get(bits->data, bits->start[f] + b) << b;
        toto &= bits->length[f] == 39 && prefix == 0x0802 && near(hm, 6197) && near(hs, 2754) &&
                near(bits->short_symbol[f], 516) && near(bits->long_symbol[f], 1634) &&
                near(seed->timings[t - 1], 600);
        bio &= bits->length[f] == 71 && ir_lab_byte_value(bits, NULL, f, 0) == 0x3F &&
               ir_lab_byte_value(bits, NULL, f, 1) == 0x7F;
        nec &= !bits->pulse_width && near(hm, 9000) && near(hs, 4500) &&
               near(seed->timings[t - 1], 560) && near(bits->short_symbol[f], 560) &&
               near(bits->long_symbol[f], 1690);
        sirc &= near(hm, 2400) && near(hs, 600) && near(bits->short_symbol[f], 600) &&
                near(bits->long_symbol[f], 1200) &&
                (bits->length[f] == 12 || bits->length[f] == 15 || bits->length[f] == 20);
        address_inverse +=
            (ir_lab_byte_value(bits, NULL, f, 0) ^ ir_lab_byte_value(bits, NULL, f, 1)) == 255;
        command_inverse +=
            (ir_lab_byte_value(bits, NULL, f, 2) ^ ir_lab_byte_value(bits, NULL, f, 3)) == 255;
    }
    result->header_mark /= bits->frames;
    result->header_space /= bits->frames;
    result->constant /= bits->frames;
    result->short_symbol /= bits->frames;
    result->long_symbol /= bits->frames;
    result->kind = sirc              ? IrLabProtocolSirc :
                   bits->pulse_width ? IrLabProtocolUnknown :
                                       IrLabProtocolPulseDistance;
    if(toto) {
        result->kind = IrLabProtocolToto;
        result->payload_offset = 15;
    } else if(nec && same_width && result->frame_bits == 24)
        result->kind = IrLabProtocolInax;
    else if(nec && same_width && result->frame_bits == 32)
        result->kind =
            command_inverse == bits->frames ?
                (address_inverse == bits->frames ? IrLabProtocolNec : IrLabProtocolNecExtended) :
                IrLabProtocolNecExt;
    else if(nec && bio)
        result->kind = IrLabProtocolBio71;
    if(result->kind == IrLabProtocolToto || result->kind == IrLabProtocolInax ||
       result->kind == IrLabProtocolBio71 || result->kind == IrLabProtocolNec ||
       result->kind == IrLabProtocolNecExtended) {
        result->checks_tested = bits->frames;
        for(unsigned f = 0; f < bits->frames; ++f) {
            bool pass = false;
            if(result->kind == IrLabProtocolToto)
                pass = (ir_lab_window_value(bits, NULL, f, 15, 0) ^
                        ir_lab_window_value(bits, NULL, f, 15, 1) ^
                        ir_lab_window_value(bits, NULL, f, 15, 2)) == 0;
            else if(result->kind == IrLabProtocolInax)
                pass = (ir_lab_byte_value(bits, NULL, f, 1) ^
                        ir_lab_byte_value(bits, NULL, f, 2)) == 255;
            else if(result->kind == IrLabProtocolBio71) {
                unsigned sum = 0;
                for(unsigned b = 0; b < 5; ++b)
                    sum += ir_lab_byte_value(bits, NULL, f, b);
                pass = (sum & 255) == ir_lab_byte_value(bits, NULL, f, 7);
            } else
                pass = (ir_lab_byte_value(bits, NULL, f, 2) ^
                        ir_lab_byte_value(bits, NULL, f, 3)) == 255;
            result->checks_passed += pass;
        }
    }
    result->valid = true;
    return true;
}

bool ir_lab_protocol_repair_toto(const IrLabBits* bits, uint8_t* changes) {
    if(!bits || !bits->valid || !changes || !bits->frames || bits->frames > IR_LAB_MAX_FRAMES)
        return false;
    uint8_t next[IR_LAB_MASK_BYTES];
    memcpy(next, changes, sizeof(next));
    for(unsigned f = 0; f < bits->frames; ++f) {
        if(bits->length[f] != 39) return false;
        unsigned prefix = 0;
        for(unsigned b = 0; b < 15; ++b)
            prefix |= (unsigned)(ir_lab_bit_get(bits->data, bits->start[f] + b) ^
                                 ir_lab_bit_get(next, bits->start[f] + b))
                      << b;
        if(prefix != 0x0802) return false;
        uint8_t check = ir_lab_window_value(bits, next, f, 15, 0) ^
                        ir_lab_window_value(bits, next, f, 15, 1);
        if(!ir_lab_window_set(bits, next, f, 15, 2, check)) return false;
    }
    memcpy(changes, next, sizeof(next));
    return true;
}

bool ir_lab_protocol_repair_inax(const IrLabBits* bits, uint8_t* changes) {
    if(!bits || !bits->valid || !changes || bits->pulse_width || !bits->frames ||
       bits->frames > IR_LAB_MAX_FRAMES || bits->count > IR_LAB_MAX_BITS)
        return false;
    uint8_t next[IR_LAB_MASK_BYTES];
    memcpy(next, changes, sizeof(next));
    for(unsigned f = 0; f < bits->frames; ++f) {
        if(bits->length[f] != 24 || bits->start[f] + 24 > bits->count ||
           (ir_lab_byte_value(bits, NULL, f, 1) ^ ir_lab_byte_value(bits, NULL, f, 2)) != 255 ||
           ir_lab_window_value(bits, next, f, 0, 0) != ir_lab_byte_value(bits, NULL, f, 0))
            return false;
        uint8_t inverse = ir_lab_window_value(bits, next, f, 0, 1) ^ 255;
        if(!ir_lab_window_set(bits, next, f, 0, 2, inverse)) return false;
    }
    memcpy(changes, next, sizeof(next));
    return true;
}

bool ir_lab_protocol_view(const IrLabDataView* source, unsigned offset, IrLabDataView* output) {
    if(!output || !source || output == source || !source->valid || !source->frames ||
       source->frames > IR_LAB_MAX_FRAMES || source->count > IR_LAB_MAX_BITS)
        return false;
    memset(output, 0, sizeof(*output));
    output->pulse_width = source->pulse_width;
    output->frames = source->frames;
    for(unsigned f = 0; f < source->frames; ++f) {
        if(offset >= source->length[f] || source->start[f] + source->length[f] > source->count)
            return false;
        output->start[f] = output->count;
        output->length[f] = source->length[f] - offset;
        for(unsigned b = offset; b < source->length[f]; ++b) {
            unsigned index = output->count++;
            if(ir_lab_bit_get(source->data, source->start[f] + b))
                output->data[index / 8] |= 1U << (index % 8);
        }
    }
    output->valid = true;
    return true;
}

unsigned ir_lab_protocol_distance(const IrLabDataView* a, const IrLabDataView* b) {
    if(!a || !b || !a->valid || !b->valid || !a->frames || !a->count || a->frames != b->frames ||
       a->count != b->count || a->count > IR_LAB_MAX_BITS || a->frames > IR_LAB_MAX_FRAMES ||
       a->pulse_width != b->pulse_width)
        return UINT16_MAX;
    for(unsigned f = 0; f < a->frames; ++f)
        if(a->length[f] != b->length[f] || a->start[f] + a->length[f] > a->count ||
           b->start[f] + b->length[f] > b->count)
            return UINT16_MAX;
    unsigned distance = 0;
    for(unsigned f = 0; f < a->frames; ++f)
        for(unsigned i = 0; i < a->length[f]; ++i)
            distance += ir_lab_bit_get(a->data, a->start[f] + i) !=
                        ir_lab_bit_get(b->data, b->start[f] + i);
    return distance;
}
