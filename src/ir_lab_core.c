#include "ir_lab_core.h"

#include <string.h>

static uint32_t ir_lab_mask(uint8_t bits) {
    return bits == 32 ? UINT32_MAX : (UINT32_C(1) << bits) - 1;
}

bool ir_lab_signal_valid(const IrLabSignal* signal) {
    if(!signal || !signal->valid) return false;
    if(signal->decoded) {
        return signal->protocol >= 0 && signal->address_bits > 0 && signal->address_bits <= 32 &&
               signal->command_bits > 0 && signal->command_bits <= 32 &&
               !(signal->address & ~ir_lab_mask(signal->address_bits)) &&
               !(signal->command & ~ir_lab_mask(signal->command_bits));
    }
    if(signal->frequency < 20000 || signal->frequency > 60000 ||
       !(signal->duty_cycle >= 0.1f && signal->duty_cycle <= 0.5f) || signal->count == 0 ||
       signal->count > IR_LAB_MAX_TIMINGS) {
        return false;
    }
    uint64_t duration = 0;
    for(size_t i = 0; i < signal->count; ++i) {
        if(signal->timings[i] < 10 || signal->timings[i] > 150000) return false;
        duration += signal->timings[i];
    }
    return duration <= IR_LAB_MAX_DURATION_US;
}

bool ir_lab_mutate(const IrLabSignal* seed, const IrLabMutation* mutation, IrLabSignal* output) {
    if(!mutation || !output || output == seed || !ir_lab_signal_valid(seed)) return false;
    // Always rebuild from the seed: editing a preview never accumulates mutations.
    *output = *seed;
    switch(mutation->mode) {
    case IrLabReplay:
        break;
    case IrLabCommandBit:
        if(!seed->decoded || mutation->index >= seed->command_bits) return false;
        output->command ^= UINT32_C(1) << mutation->index;
        break;
    case IrLabCommandField: {
        if(!seed->decoded || mutation->width == 0 || mutation->width > 8 ||
           mutation->index >= seed->command_bits ||
           mutation->index + mutation->width > seed->command_bits || mutation->value < 0 ||
           (uint32_t)mutation->value > ir_lab_mask(mutation->width)) {
            return false;
        }
        const uint32_t mask = ir_lab_mask(mutation->width) << mutation->index;
        output->command = (seed->command & ~mask) | ((uint32_t)mutation->value << mutation->index);
        break;
    }
    case IrLabRawScale:
        if(seed->decoded || mutation->index >= seed->count || mutation->value < -50 ||
           mutation->value > 50) {
            return false;
        }
        output->timings[mutation->index] =
            ((uint64_t)seed->timings[mutation->index] * (100 + mutation->value)) / 100;
        break;
    case IrLabRawSet:
        if(seed->decoded || mutation->index >= seed->count || mutation->value < 10 ||
           mutation->value > 20000) {
            return false;
        }
        output->timings[mutation->index] = (uint32_t)mutation->value;
        break;
    default:
        return false;
    }
    return ir_lab_signal_valid(output);
}

bool ir_lab_can_send(const IrLabGate* gate, uint32_t now_ms) {
    return gate && gate->armed && !gate->locked && !gate->pending_observation &&
           gate->trials < IR_LAB_MAX_TRIALS &&
           (!gate->has_sent || (uint32_t)(now_ms - gate->last_ms) >= IR_LAB_COOLDOWN_MS);
}

bool ir_lab_reserve_trial(IrLabGate* gate, uint32_t now_ms) {
    if(!ir_lab_can_send(gate, now_ms)) return false;
    // Reserve BEFORE transmission. Storage/TX failure must never refund a trial.
    gate->trials++;
    gate->last_ms = now_ms;
    gate->has_sent = true;
    gate->pending_observation = true;
    return true;
}

bool ir_lab_can_send_range(const IrLabGate* gate, uint32_t now_ms, uint32_t delay_ms) {
    return gate && gate->armed && !gate->locked && !gate->pending_observation &&
           gate->trials < IR_LAB_MAX_TRIALS && (delay_ms == 1000 || delay_ms == 2000) &&
           (!gate->has_sent || (uint32_t)(now_ms - gate->last_ms) >= delay_ms);
}

bool ir_lab_reserve_range_trial(IrLabGate* gate, uint32_t now_ms, uint32_t delay_ms) {
    if(!ir_lab_can_send_range(gate, now_ms, delay_ms)) return false;
    gate->trials++;
    gate->last_ms = now_ms;
    gate->has_sent = true;
    gate->pending_observation = true;
    return true;
}

void ir_lab_observe(IrLabGate* gate, bool unexpected) {
    if(!gate || !gate->pending_observation) return;
    gate->pending_observation = false;
    if(unexpected) ir_lab_stop(gate);
}

void ir_lab_stop(IrLabGate* gate) {
    if(!gate) return;
    gate->armed = false;
    gate->locked = true;
}

const char* ir_lab_mode_name(IrLabMode mode) {
    static const char* const names[] = {
        "Replay", "Command bit", "Command field", "Raw delta %", "Raw set us", "Data bits"};
    return (unsigned)mode < IrLabModeCount ? names[mode] : "Invalid";
}

bool ir_lab_bit_get(const uint8_t* data, uint16_t index) {
    return data && index < IR_LAB_MAX_BITS && ((data[index / 8] >> (index % 8)) & 1U);
}

static void ir_lab_bit_put(uint8_t* data, uint16_t index, bool value) {
    uint8_t mask = 1U << (index % 8);
    if(value)
        data[index / 8] |= mask;
    else
        data[index / 8] &= (uint8_t)~mask;
}

enum {
    BitsOk,
    BitsSignal,
    BitsHeader,
    BitsMark,
    BitsSpace,
    BitsLimit,
    BitsTrailer,
    BitsLevels,
    BitsClusters
};
const char* ir_lab_bits_error(const IrLabBits* bits) {
    static const char* const errors[] = {
        "Ready to edit",
        "Invalid raw capture",
        "Header not recognized",
        "Data marks vary",
        "Bit space ambiguous",
        "Capture too long",
        "Frame is incomplete",
        "Need both bit levels",
        "Timing clusters vary"};
    return bits && bits->error < sizeof(errors) / sizeof(errors[0]) ? errors[bits->error] :
                                                                      "Data not recognized";
}

static bool ir_lab_extract_width(const IrLabSignal* seed, IrLabBits* bits) {
    bits->pulse_width = true;
    size_t pos = 0;
    while(pos < seed->count) {
        bits->error = BitsHeader;
        if(bits->frames >= IR_LAB_MAX_FRAMES || pos + 1 >= seed->count ||
           seed->timings[pos] < 2000 || seed->timings[pos] > 3000 ||
           seed->timings[pos + 1] < 350 || seed->timings[pos + 1] > 850)
            return false;
        unsigned frame = bits->frames++;
        bits->start[frame] = bits->count;
        pos += 2;
        uint64_t short_sum = 0, long_sum = 0, space_sum = 0;
        unsigned short_n = 0, long_n = 0, space_n = 0;
        while(pos < seed->count) {
            bits->error = BitsMark;
            uint32_t mark = seed->timings[pos];
            bool one = mark >= 1000 && mark <= 1500;
            if(!one && !(mark >= 350 && mark <= 850)) return false;
            bits->error = BitsLimit;
            if(bits->count >= IR_LAB_MAX_BITS) return false;
            bits->timing[bits->count] = pos;
            ir_lab_bit_put(bits->data, bits->count++, one);
            if(one) {
                long_sum += mark;
                ++long_n;
            } else {
                short_sum += mark;
                ++short_n;
            }
            if(pos + 1 == seed->count) {
                ++pos;
                break;
            }
            uint32_t space = seed->timings[pos + 1];
            if(space >= 8000) {
                pos += 2;
                break;
            }
            bits->error = BitsSpace;
            if(space < 350 || space > 850) return false;
            space_sum += space;
            ++space_n;
            pos += 2;
        }
        unsigned length = bits->count - bits->start[frame];
        bits->error = BitsTrailer;
        if(length < 8) return false;
        bits->error = BitsLevels;
        if(!short_n || !long_n || !space_n) return false;
        bits->length[frame] = length;
        bits->short_symbol[frame] = short_sum / short_n;
        bits->long_symbol[frame] = long_sum / long_n;
        uint32_t mean_space = space_sum / space_n;
        bits->error = BitsClusters;
        for(unsigned b = bits->start[frame]; b < bits->count; ++b) {
            unsigned timing = bits->timing[b];
            uint32_t mark = seed->timings[timing];
            uint32_t mean = ir_lab_bit_get(bits->data, b) ? bits->long_symbol[frame] :
                                                            bits->short_symbol[frame];
            if(mark * 100 < mean * 70 || mark * 100 > mean * 130) return false;
            if(timing + 1 < seed->count && seed->timings[timing + 1] < 8000) {
                uint32_t space = seed->timings[timing + 1];
                if(space * 100 < mean_space * 70 || space * 100 > mean_space * 130) return false;
            }
        }
    }
    bits->error = BitsOk;
    bits->valid = bits->count > 0;
    return bits->valid;
}

bool ir_lab_extract_bits(const IrLabSignal* seed, IrLabBits* bits) {
    if(!bits) return false;
    memset(bits, 0, sizeof(*bits));
    bits->error = BitsSignal;
    if(!ir_lab_signal_valid(seed) || seed->decoded) return false;
    if(seed->timings[0] >= 2000 && seed->timings[0] <= 3000)
        return ir_lab_extract_width(seed, bits);
    size_t pos = 0;
    while(pos < seed->count) {
        // A full NEC frame may be followed by header/space/trailer repeat tokens.
        // Keep their timings exact; they do not contain new editable data bits.
        if(bits->frames && bits->length[bits->frames - 1] == 32 && seed->timings[0] >= 8000 &&
           pos + 2 < seed->count && seed->timings[pos] >= 8000 && seed->timings[pos] <= 10000 &&
           seed->timings[pos + 1] >= 1800 && seed->timings[pos + 1] <= 2600 &&
           seed->timings[pos + 2] >= 350 && seed->timings[pos + 2] <= 900 &&
           (pos + 3 == seed->count || seed->timings[pos + 3] >= 8000)) {
            bits->error = BitsLimit;
            if(bits->repeats >= IR_LAB_MAX_FRAMES) return false;
            ++bits->repeats;
            pos += pos + 3 == seed->count ? 3 : 4;
            continue;
        }
        bits->error = BitsHeader;
        if(bits->frames >= IR_LAB_MAX_FRAMES || pos + 1 >= seed->count) return false;
        uint32_t header = seed->timings[pos], space = seed->timings[pos + 1];
        // Conservative pulse-distance framing matching the supplied captures.
        // Reject Manchester, repeat-only captures, and ambiguous cells.
        if(header < 4000 || header > 10000 || space < 1800 || space > 5500 ||
           space * 10 < header * 4 || space * 100 > header * 65)
            return false;
        unsigned frame = bits->frames++;
        bits->start[frame] = bits->count;
        pos += 2;
        uint64_t short_sum = 0, long_sum = 0, mark_sum = 0;
        unsigned short_n = 0, long_n = 0;
        bool trailer = false;
        while(pos < seed->count) {
            bits->error = BitsMark;
            uint32_t mark = seed->timings[pos];
            if(mark < 350 || mark > 900) return false;
            if(pos + 1 == seed->count || seed->timings[pos + 1] >= 8000) {
                trailer = true;
                pos += pos + 1 == seed->count ? 1 : 2;
                break;
            }
            space = seed->timings[pos + 1];
            bits->error = BitsSpace;
            bool one = space >= 1200 && space <= 2200;
            if(!one && !(space >= 300 && space <= 950)) return false;
            bits->error = BitsLimit;
            if(bits->count >= IR_LAB_MAX_BITS) return false;
            bits->timing[bits->count] = pos + 1;
            ir_lab_bit_put(bits->data, bits->count++, one);
            mark_sum += mark;
            if(one) {
                long_sum += space;
                ++long_n;
            } else {
                short_sum += space;
                ++short_n;
            }
            pos += 2;
        }
        unsigned length = bits->count - bits->start[frame];
        bits->error = BitsTrailer;
        if(!trailer || length < 8) return false;
        bits->error = BitsLevels;
        if(!short_n || !long_n) return false;
        bits->length[frame] = length;
        bits->short_symbol[frame] = short_sum / short_n;
        bits->long_symbol[frame] = long_sum / long_n;
        bits->error = BitsClusters;
        uint32_t mean_mark = mark_sum / length;
        for(unsigned j = bits->start[frame]; j < bits->count; ++j) {
            uint32_t mark = seed->timings[bits->timing[j] - 1];
            uint32_t actual = seed->timings[bits->timing[j]];
            uint32_t mean = ir_lab_bit_get(bits->data, j) ? bits->long_symbol[frame] :
                                                            bits->short_symbol[frame];
            if(mark * 100 < mean_mark * 70 || mark * 100 > mean_mark * 130 ||
               actual * 100 < mean * 70 || actual * 100 > mean * 130)
                return false;
        }
    }
    bits->error = BitsOk;
    bits->valid = bits->count > 0;
    return bits->valid;
}

uint8_t ir_lab_bit_frame(const IrLabBits* bits, uint16_t index) {
    if(!bits || !bits->valid || index >= bits->count) return UINT8_MAX;
    for(uint8_t f = 0; f < bits->frames; ++f)
        if(index >= bits->start[f] && index < bits->start[f] + bits->length[f]) return f;
    return UINT8_MAX;
}

unsigned ir_lab_set_linked(const IrLabBits* bits, uint8_t* changes, uint16_t index, bool value) {
    uint8_t selected = ir_lab_bit_frame(bits, index);
    if(selected == UINT8_MAX || !changes) return 0;
    bool changed = ir_lab_bit_get(bits->data, index) ^ value;
    unsigned linked = 0;
    for(unsigned f = 0; f < bits->frames; ++f) {
        if(bits->length[f] != bits->length[selected]) continue;
        bool same = true;
        for(unsigned b = 0; b < bits->length[f]; ++b)
            if(ir_lab_bit_get(bits->data, bits->start[f] + b) !=
               ir_lab_bit_get(bits->data, bits->start[selected] + b)) {
                same = false;
                break;
            }
        if(same) {
            ir_lab_bit_put(changes, bits->start[f] + index - bits->start[selected], changed);
            ++linked;
        }
    }
    return linked;
}

unsigned ir_lab_toggle_linked(const IrLabBits* bits, uint8_t* changes, uint16_t index) {
    if(!bits || !changes || index >= bits->count) return 0;
    bool value = ir_lab_bit_get(bits->data, index) ^ ir_lab_bit_get(changes, index);
    return ir_lab_set_linked(bits, changes, index, !value);
}

unsigned ir_lab_byte_width(const IrLabBits* bits, unsigned frame, unsigned byte) {
    if(!bits || !bits->valid || frame >= bits->frames || byte >= (bits->length[frame] + 7U) / 8U)
        return 0;
    unsigned remaining = bits->length[frame] - byte * 8;
    return remaining < 8 ? remaining : 8;
}

uint8_t ir_lab_byte_value(
    const IrLabBits* bits,
    const uint8_t* changes,
    unsigned frame,
    unsigned byte) {
    unsigned width = ir_lab_byte_width(bits, frame, byte);
    unsigned value = 0;
    for(unsigned b = 0; b < width; ++b) {
        unsigned index = bits->start[frame] + byte * 8 + b;
        bool bit = ir_lab_bit_get(bits->data, index);
        if(changes) bit ^= ir_lab_bit_get(changes, index);
        value |= (unsigned)bit << b;
    }
    return value;
}

bool ir_lab_set_byte_linked(
    const IrLabBits* bits,
    uint8_t* changes,
    unsigned frame,
    unsigned byte,
    unsigned value) {
    unsigned width = ir_lab_byte_width(bits, frame, byte);
    if(!changes || !width || value >= (1U << width)) return false;
    for(unsigned b = 0; b < width; ++b)
        ir_lab_set_linked(bits, changes, bits->start[frame] + byte * 8 + b, (value >> b) & 1U);
    return true;
}

unsigned ir_lab_hex_range_count(unsigned from, unsigned to, unsigned step) {
    if(from > 255 || to > 255 || !step || step > 255) return 0;
    unsigned distance = from > to ? from - to : to - from;
    return distance / step + 1;
}

bool ir_lab_hex_range_value(
    unsigned from,
    unsigned to,
    unsigned step,
    unsigned index,
    uint8_t* value) {
    unsigned count = ir_lab_hex_range_count(from, to, step);
    if(!value || index >= count) return false;
    *value = from <= to ? from + index * step : from - index * step;
    return true;
}

unsigned ir_lab_hex_linked_count(
    unsigned from,
    unsigned to,
    unsigned step,
    unsigned linked_from,
    unsigned linked_step,
    bool decrease,
    unsigned width) {
    unsigned count = ir_lab_hex_range_count(from, to, step);
    if(!count || !width || width > 8 || !linked_step || linked_step > 255) return 0;
    unsigned maximum = (1U << width) - 1;
    if(linked_from > maximum) return 0;
    unsigned room = decrease ? linked_from : maximum - linked_from;
    unsigned linked_count = room / linked_step + 1;
    return count < linked_count ? count : linked_count;
}

bool ir_lab_hex_linked_value(
    unsigned from,
    unsigned step,
    bool decrease,
    unsigned width,
    unsigned index,
    uint8_t* value) {
    if(!value || !width || width > 8 || !step || step > 255) return false;
    unsigned maximum = (1U << width) - 1;
    if(from > maximum) return false;
    unsigned room = decrease ? from : maximum - from;
    if(index > room / step) return false;
    *value = decrease ? from - index * step : from + index * step;
    return true;
}

bool ir_lab_byte_targets_overlap(
    const IrLabBits* bits,
    unsigned frame_a,
    unsigned byte_a,
    unsigned frame_b,
    unsigned byte_b) {
    if(!bits || bits->frames > IR_LAB_MAX_FRAMES || bits->count > IR_LAB_MAX_BITS ||
       !ir_lab_byte_width(bits, frame_a, byte_a) || !ir_lab_byte_width(bits, frame_b, byte_b))
        return true;
    if(bits->start[frame_a] + bits->length[frame_a] > bits->count ||
       bits->start[frame_b] + bits->length[frame_b] > bits->count)
        return true;
    if(byte_a != byte_b || bits->length[frame_a] != bits->length[frame_b]) return false;
    for(unsigned bit = 0; bit < bits->length[frame_a]; ++bit)
        if(ir_lab_bit_get(bits->data, bits->start[frame_a] + bit) !=
           ir_lab_bit_get(bits->data, bits->start[frame_b] + bit))
            return false;
    return true;
}

unsigned
    ir_lab_window_width(const IrLabBits* bits, unsigned frame, unsigned offset, unsigned byte) {
    if(!bits || !bits->valid || bits->frames > IR_LAB_MAX_FRAMES || frame >= bits->frames ||
       bits->count > IR_LAB_MAX_BITS || bits->start[frame] + bits->length[frame] > bits->count ||
       offset >= bits->length[frame] || byte >= (bits->length[frame] - offset + 7U) / 8U)
        return 0;
    unsigned remaining = bits->length[frame] - offset - byte * 8;
    return remaining < 8 ? remaining : 8;
}

uint8_t ir_lab_window_value(
    const IrLabBits* bits,
    const uint8_t* changes,
    unsigned frame,
    unsigned offset,
    unsigned byte) {
    unsigned width = ir_lab_window_width(bits, frame, offset, byte), value = 0;
    for(unsigned b = 0; b < width; ++b) {
        unsigned index = bits->start[frame] + offset + byte * 8 + b;
        bool bit = ir_lab_bit_get(bits->data, index);
        if(changes) bit ^= ir_lab_bit_get(changes, index);
        value |= (unsigned)bit << b;
    }
    return value;
}

bool ir_lab_window_set(
    const IrLabBits* bits,
    uint8_t* changes,
    unsigned frame,
    unsigned offset,
    unsigned byte,
    unsigned value) {
    unsigned width = ir_lab_window_width(bits, frame, offset, byte);
    if(!changes || !width || value >= (1U << width)) return false;
    for(unsigned b = 0; b < width; ++b)
        ir_lab_set_linked(
            bits, changes, bits->start[frame] + offset + byte * 8 + b, (value >> b) & 1U);
    return true;
}

unsigned
    ir_lab_byte_plan_count(const IrLabBits* bits, const IrLabByteRule* rules, unsigned count) {
    if(!bits || !bits->valid || !bits->frames || bits->frames > IR_LAB_MAX_FRAMES ||
       !bits->count || bits->count > IR_LAB_MAX_BITS || !rules || !count ||
       count > IR_LAB_MAX_BYTE_RULES)
        return 0;
    for(unsigned f = 0; f < bits->frames; ++f)
        if(!bits->length[f] || bits->start[f] + bits->length[f] > bits->count) return 0;
    unsigned positions = 256;
    bool active = false;
    for(unsigned n = 0; n < count; ++n) {
        const IrLabByteRule* rule = &rules[n];
        unsigned width = ir_lab_window_width(bits, rule->frame, rule->bit_offset, rule->byte);
        if(!width || rule->from >= (1U << width) || rule->to >= (1U << width) ||
           rule->step < -255 || rule->step > 255)
            return 0;
        for(unsigned other = 0; other < n; ++other) {
            const IrLabByteRule* previous = &rules[other];
            unsigned first = rule->bit_offset + rule->byte * 8;
            unsigned second = previous->bit_offset + previous->byte * 8;
            unsigned previous_width =
                ir_lab_window_width(bits, previous->frame, previous->bit_offset, previous->byte);
            if(ir_lab_byte_targets_overlap(bits, rule->frame, 0, previous->frame, 0) &&
               first < second + previous_width && second < first + width)
                return 0;
        }
        int distance = (int)rule->to - rule->from;
        if(!rule->step) {
            if(distance) return 0;
            continue;
        }
        if((distance < 0 && rule->step > 0) || (distance > 0 && rule->step < 0)) return 0;
        unsigned room = distance < 0 ? -distance : distance;
        unsigned step = rule->step < 0 ? -rule->step : rule->step;
        unsigned available = room / step + 1;
        if(available < positions) positions = available;
        active = true;
    }
    return active ? positions : 1;
}

bool ir_lab_byte_plan_apply(
    const IrLabBits* bits,
    const IrLabByteRule* rules,
    unsigned count,
    const uint8_t* base,
    unsigned index,
    uint8_t* changes) {
    unsigned positions = ir_lab_byte_plan_count(bits, rules, count);
    if(!base || !changes || index >= positions) return false;
    uint8_t next[IR_LAB_MASK_BYTES];
    memcpy(next, base, sizeof(next));
    for(unsigned n = 0; n < count; ++n) {
        int value = rules[n].from + (int)index * rules[n].step;
        if(value < 0 || !ir_lab_window_set(
                            bits, next, rules[n].frame, rules[n].bit_offset, rules[n].byte, value))
            return false;
    }
    memcpy(changes, next, sizeof(next));
    return true;
}

unsigned ir_lab_changed_count(const IrLabBits* bits, const uint8_t* changes) {
    if(!bits || !bits->valid || !changes) return 0;
    unsigned count = 0;
    for(unsigned b = 0; b < bits->count; ++b)
        count += ir_lab_bit_get(changes, b);
    return count;
}

bool ir_lab_apply_bits(
    const IrLabSignal* seed,
    const IrLabBits* bits,
    const uint8_t* changes,
    IrLabSignal* output) {
    if(!seed || !bits || !changes || !output || output == seed || seed->decoded || !bits->valid ||
       bits->count > IR_LAB_MAX_BITS || bits->frames > IR_LAB_MAX_FRAMES ||
       !ir_lab_signal_valid(seed))
        return false;
    *output = *seed;
    for(unsigned b = 0; b < bits->count; ++b) {
        uint8_t f = ir_lab_bit_frame(bits, b);
        unsigned timing = bits->timing[b];
        if(f == UINT8_MAX || (timing & 1) != (unsigned)!bits->pulse_width ||
           timing >= seed->count || (b && timing <= bits->timing[b - 1]))
            return false;
        if(ir_lab_bit_get(changes, b)) {
            output->timings[timing] = ir_lab_bit_get(bits->data, b) ? bits->short_symbol[f] :
                                                                      bits->long_symbol[f];
        }
    }
    return ir_lab_signal_valid(output);
}

void ir_lab_data_view(const IrLabBits* bits, IrLabDataView* view) {
    if(!view) return;
    memset(view, 0, sizeof(*view));
    if(!bits || !bits->valid) return;
    view->valid = true;
    view->pulse_width = bits->pulse_width;
    view->count = bits->count;
    view->frames = bits->frames;
    memcpy(view->start, bits->start, sizeof(view->start));
    memcpy(view->length, bits->length, sizeof(view->length));
    memcpy(view->data, bits->data, sizeof(view->data));
}

unsigned ir_lab_view_width(const IrLabDataView* view, unsigned frame, unsigned byte) {
    if(!view || !view->valid || frame >= view->frames || frame >= IR_LAB_MAX_FRAMES ||
       view->count > IR_LAB_MAX_BITS || view->start[frame] + view->length[frame] > view->count ||
       byte >= (view->length[frame] + 7U) / 8U)
        return 0;
    unsigned remaining = view->length[frame] - byte * 8;
    return remaining < 8 ? remaining : 8;
}

uint8_t ir_lab_view_byte(const IrLabDataView* view, unsigned frame, unsigned byte) {
    unsigned width = ir_lab_view_width(view, frame, byte);
    unsigned value = 0;
    for(unsigned b = 0; b < width; ++b)
        value |= (unsigned)ir_lab_bit_get(view->data, view->start[frame] + byte * 8 + b) << b;
    return value;
}

unsigned ir_lab_compare(
    const IrLabDataView* views,
    unsigned count,
    unsigned frame,
    IrLabComparison* comparison) {
    if(!comparison) return 0;
    memset(comparison, 0, sizeof(*comparison));
    if(!views || !count || count > IR_LAB_MAX_CAPTURES || frame >= IR_LAB_MAX_FRAMES) return 0;
    unsigned baseline = count, comparable = 0;
    for(unsigned n = 0; n < count; ++n) {
        if(!ir_lab_view_width(&views[n], frame, 0)) continue;
        if(baseline == count) {
            baseline = n;
            comparison->bits = views[n].length[frame];
        }
        if(views[n].length[frame] != comparison->bits ||
           views[n].pulse_width != views[baseline].pulse_width) {
            comparison->different_layout_mask |= 1U << n;
            continue;
        }
        comparison->comparable_mask |= 1U << n;
        ++comparable;
        for(unsigned b = 0; b < comparison->bits; ++b)
            if(ir_lab_bit_get(views[n].data, views[n].start[frame] + b) !=
               ir_lab_bit_get(views[baseline].data, views[baseline].start[frame] + b))
                ir_lab_bit_put(comparison->varying, b, true);
    }
    for(unsigned b = 0; b < comparison->bits; ++b)
        comparison->changing_bits += ir_lab_bit_get(comparison->varying, b);
    return comparable;
}

bool ir_lab_hex_delta(
    const IrLabDataView* earlier,
    const IrLabDataView* later,
    unsigned frame,
    unsigned byte,
    int16_t* delta) {
    unsigned width = ir_lab_view_width(earlier, frame, byte);
    if(!delta || !width || width != ir_lab_view_width(later, frame, byte) ||
       earlier->length[frame] != later->length[frame] ||
       earlier->pulse_width != later->pulse_width)
        return false;
    *delta =
        (int16_t)ir_lab_view_byte(later, frame, byte) - ir_lab_view_byte(earlier, frame, byte);
    return true;
}

bool ir_lab_hex_sequence(
    const IrLabDataView* views,
    unsigned count,
    unsigned frame,
    unsigned byte,
    int16_t* step) {
    if(!views || !step || count < 2 || count > IR_LAB_MAX_CAPTURES) return false;
    IrLabComparison comparison;
    if(ir_lab_compare(views, count, frame, &comparison) != count ||
       !ir_lab_view_width(&views[0], frame, byte))
        return false;
    int16_t delta = (int16_t)ir_lab_view_byte(&views[1], frame, byte) -
                    ir_lab_view_byte(&views[0], frame, byte);
    for(unsigned n = 2; n < count; ++n)
        if((int16_t)ir_lab_view_byte(&views[n], frame, byte) -
               ir_lab_view_byte(&views[n - 1], frame, byte) !=
           delta)
            return false;
    *step = delta;
    return true;
}
