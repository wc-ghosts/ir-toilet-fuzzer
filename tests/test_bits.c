#include "ir_lab_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static IrLabSignal pair_width(unsigned width, bool different) {
    IrLabSignal raw = {.valid = true, .frequency = 38000, .duty_cycle = 0.33f};
    for(unsigned f = 0; f < 2; ++f) {
        raw.timings[raw.count++] = 9000;
        raw.timings[raw.count++] = 4500;
        for(unsigned b = 0; b < width; ++b) {
            raw.timings[raw.count++] = 500;
            raw.timings[raw.count++] = ((b % 2) ^ (different && f == 1 && b == 3)) ? 1500 : 500;
        }
        raw.timings[raw.count++] = 500;
        if(f == 0) raw.timings[raw.count++] = 30000;
    }
    return raw;
}
static IrLabSignal pair(bool different) {
    return pair_width(8, different);
}

static void test_hex(void) {
    IrLabBits bits;
    uint8_t changes[IR_LAB_MASK_BYTES] = {0};
    IrLabSignal seed = pair(false), edited;
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(ir_lab_byte_value(&bits, NULL, 0, 0) == 0xAA);
    for(unsigned value = 0; value < 256; ++value) {
        assert(ir_lab_set_byte_linked(&bits, changes, 0, 0, value));
        assert(ir_lab_byte_value(&bits, changes, 0, 0) == value);
        assert(ir_lab_byte_value(&bits, changes, 1, 0) == value);
        assert(ir_lab_byte_value(&bits, NULL, 0, 0) == 0xAA);
        assert(ir_lab_apply_bits(&seed, &bits, changes, &edited));
        for(unsigned n = 0; n < seed.count; ++n)
            if(n % 2 == 0 || n == 1 || n == 19 || n == 21)
                assert(seed.timings[n] == edited.timings[n]);
    }
    uint8_t snapshot[IR_LAB_MASK_BYTES];
    memcpy(snapshot, changes, sizeof(snapshot));
    assert(!ir_lab_set_byte_linked(&bits, changes, 0, 0, 256));
    assert(!ir_lab_set_byte_linked(&bits, changes, 2, 0, 0));
    assert(!ir_lab_set_byte_linked(&bits, changes, 0, 1, 0));
    assert(!ir_lab_set_byte_linked(&bits, NULL, 0, 0, 0));
    assert(memcmp(snapshot, changes, sizeof(changes)) == 0);
    seed = pair_width(15, false);
    assert(ir_lab_extract_bits(&seed, &bits));
    memset(changes, 0, sizeof(changes));
    assert(ir_lab_byte_width(&bits, 0, 1) == 7);
    assert(ir_lab_set_byte_linked(&bits, changes, 0, 1, 0x7F));
    assert(ir_lab_byte_value(&bits, changes, 1, 1) == 0x7F);
    memcpy(snapshot, changes, sizeof(snapshot));
    assert(!ir_lab_set_byte_linked(&bits, changes, 0, 1, 0x80));
    assert(memcmp(snapshot, changes, sizeof(changes)) == 0);
    seed = pair(true);
    assert(ir_lab_extract_bits(&seed, &bits));
    memset(changes, 0, sizeof(changes));
    unsigned other = ir_lab_byte_value(&bits, NULL, 1, 0);
    assert(ir_lab_set_byte_linked(&bits, changes, 0, 0, 0xFF));
    assert(ir_lab_byte_value(&bits, changes, 1, 0) == other);

    const unsigned steps[] = {1, 2, 17, 255};
    for(unsigned from = 0; from < 256; ++from)
        for(unsigned to = 0; to < 256; ++to)
            for(unsigned s = 0; s < sizeof(steps) / sizeof(*steps); ++s) {
                unsigned step = steps[s];
                unsigned count = ir_lab_hex_range_count(from, to, step);
                unsigned distance = from < to ? to - from : from - to;
                assert(count == distance / step + 1);
                for(unsigned i = 0; i < count; ++i) {
                    uint8_t value;
                    assert(ir_lab_hex_range_value(from, to, step, i, &value));
                    assert(value == (from <= to ? from + i * step : from - i * step));
                    assert(from <= to ? value <= to : value >= to);
                }
                uint8_t untouched = 0x5A;
                assert(!ir_lab_hex_range_value(from, to, step, count, &untouched));
                assert(untouched == 0x5A);
            }
    assert(!ir_lab_hex_range_count(0, 255, 0));
    assert(!ir_lab_hex_range_count(256, 0, 1));
    assert(!ir_lab_hex_range_count(0, 256, 1));
    assert(!ir_lab_hex_range_count(0, 255, 256));
    assert(!ir_lab_hex_range_value(0, 255, 1, 0, NULL));
}
static void test_linked_fields(void) {
    assert(ir_lab_hex_linked_count(0x10, 0x14, 1, 0xEF, 1, true, 8) == 5);
    assert(ir_lab_hex_linked_count(0x10, 0xFF, 1, 2, 1, true, 8) == 3);
    assert(ir_lab_hex_linked_count(0, 255, 2, 0xFE, 3, false, 8) == 1);
    assert(ir_lab_hex_linked_count(0x10, 0, 1, 0xFD, 1, false, 8) == 3);
    assert(ir_lab_hex_linked_count(0, 255, 1, 7, 1, true, 3) == 8);
    assert(!ir_lab_hex_linked_count(0, 255, 1, 8, 1, true, 3));
    assert(!ir_lab_hex_linked_count(0, 255, 1, 7, 0, true, 3));
    assert(!ir_lab_hex_linked_count(0, 255, 1, 0, 1, false, 0));
    assert(!ir_lab_hex_linked_count(0, 255, 1, 0, 1, false, 9));
    assert(!ir_lab_hex_linked_count(0, 255, 0, 0, 1, false, 8));
    assert(!ir_lab_hex_linked_count(0, 255, 1, 0, 256, false, 8));
    uint8_t value = 0xA5;
    assert(!ir_lab_hex_linked_value(0, 1, true, 8, 1, &value) && value == 0xA5);
    assert(!ir_lab_hex_linked_value(255, 1, false, 8, 1, &value));
    assert(!ir_lab_hex_linked_value(128, 1, true, 8, UINT32_MAX, &value));
    assert(!ir_lab_hex_linked_value(0, 1, false, 8, 0, NULL));
    assert(!ir_lab_hex_linked_value(256, 1, true, 8, 0, &value));
    for(unsigned width = 1; width <= 8; ++width) {
        unsigned maximum = (1U << width) - 1;
        for(unsigned start = 0; start <= maximum; ++start)
            for(unsigned step = 1; step <= 17; ++step)
                for(unsigned direction = 0; direction < 2; ++direction) {
                    unsigned count =
                        ir_lab_hex_linked_count(0, 255, 1, start, step, direction, width);
                    assert(count == (direction ? start / step + 1 : (maximum - start) / step + 1));
                    for(unsigned index = 0; index < count; ++index) {
                        assert(
                            ir_lab_hex_linked_value(start, step, direction, width, index, &value));
                        assert(value == (direction ? start - index * step : start + index * step));
                    }
                    assert(!ir_lab_hex_linked_value(start, step, direction, width, count, &value));
                }
    }
    IrLabSignal seed = pair_width(16, false);
    IrLabBits bits;
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(ir_lab_byte_targets_overlap(&bits, 0, 0, 0, 0));
    assert(ir_lab_byte_targets_overlap(&bits, 0, 0, 1, 0));
    assert(!ir_lab_byte_targets_overlap(&bits, 0, 0, 1, 1));
    seed = pair_width(16, true);
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(!ir_lab_byte_targets_overlap(&bits, 0, 0, 1, 0));
    assert(ir_lab_byte_targets_overlap(NULL, 0, 0, 0, 1));
    assert(ir_lab_byte_targets_overlap(&bits, 8, 0, 0, 1));
    bits.start[0] = 510;
    assert(ir_lab_byte_targets_overlap(&bits, 0, 0, 1, 1));
}
static void test_byte_plans(void) {
    IrLabBits bits;
    IrLabSignal seed = pair_width(24, true);
    assert(ir_lab_extract_bits(&seed, &bits));
    IrLabByteRule rules[] = {
        {.frame = 0, .byte = 0, .from = 0x10, .to = 0x40, .step = 0x10},
        {.frame = 0, .byte = 1, .from = 0xEF, .to = 0xBF, .step = -0x10},
        {.frame = 0, .byte = 2, .from = 1, .to = 7, .step = 2}};
    uint8_t base[IR_LAB_MASK_BYTES] = {0}, changes[IR_LAB_MASK_BYTES], snapshot[IR_LAB_MASK_BYTES];
    assert(ir_lab_byte_plan_count(&bits, rules, 3) == 4);
    for(unsigned n = 0; n < 4; ++n) {
        assert(ir_lab_byte_plan_apply(&bits, rules, 3, base, n, changes));
        assert(ir_lab_byte_value(&bits, changes, 0, 0) == 0x10 + n * 0x10);
        assert(ir_lab_byte_value(&bits, changes, 0, 1) == 0xEF - n * 0x10);
        assert(ir_lab_byte_value(&bits, changes, 0, 2) == 1 + n * 2);
        assert(ir_lab_byte_value(&bits, changes, 1, 0) == ir_lab_byte_value(&bits, NULL, 1, 0));
    }
    memcpy(snapshot, changes, sizeof(snapshot));
    assert(!ir_lab_byte_plan_apply(&bits, rules, 3, base, 4, changes));
    assert(memcmp(snapshot, changes, sizeof(snapshot)) == 0);
    rules[2].to = 8;
    assert(ir_lab_byte_plan_count(&bits, rules, 3) == 4); // No clamp onto unaligned end.
    rules[2].to = 3;
    assert(ir_lab_byte_plan_count(&bits, rules, 3) == 2); // First byte bound wins.
    rules[2].step = -2;
    assert(!ir_lab_byte_plan_count(&bits, rules, 3));
    assert(!ir_lab_byte_plan_apply(&bits, rules, 3, base, 0, changes));
    assert(memcmp(snapshot, changes, sizeof(snapshot)) == 0);
    rules[2].step = 0;
    assert(!ir_lab_byte_plan_count(&bits, rules, 3));
    rules[2].to = rules[2].from;
    assert(ir_lab_byte_plan_count(&bits, rules, 3) == 4);
    rules[2] = rules[0];
    assert(!ir_lab_byte_plan_count(&bits, rules, 3));
    rules[2].frame = 1;
    assert(
        ir_lab_byte_plan_count(&bits, rules, 3) == 4); // Distinct original frame is independent.
    seed = pair_width(24, false);
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(!ir_lab_byte_plan_count(&bits, rules, 3)); // Identical repeated byte is an alias.
    rules[2] = (IrLabByteRule){.frame = 0, .byte = 2, .from = 7, .to = 7, .step = 0};
    assert(ir_lab_byte_plan_apply(&bits, rules, 3, base, 2, changes));
    assert(
        ir_lab_byte_value(&bits, changes, 1, 0) == 0x30 &&
        ir_lab_byte_value(&bits, changes, 1, 1) == 0xCF);
    for(unsigned width = 1; width <= 8; ++width) {
        seed = pair_width(8 + width, false);
        assert(ir_lab_extract_bits(&seed, &bits));
        unsigned maximum = (1U << width) - 1;
        IrLabByteRule rule = {.frame = 0, .byte = 1, .from = maximum, .to = 0, .step = -1};
        assert(ir_lab_byte_plan_count(&bits, &rule, 1) == maximum + 1);
        for(unsigned n = 0; n <= maximum; ++n) {
            assert(ir_lab_byte_plan_apply(&bits, &rule, 1, base, n, changes));
            assert(ir_lab_byte_value(&bits, changes, 0, 1) == maximum - n);
            assert(ir_lab_byte_value(&bits, changes, 1, 1) == maximum - n);
        }
        rule.from = 0;
        rule.to = maximum;
        rule.step = 1;
        assert(ir_lab_byte_plan_count(&bits, &rule, 1) == maximum + 1);
        rule.step = 256;
        assert(!ir_lab_byte_plan_count(&bits, &rule, 1));
        rule.step = -256;
        assert(!ir_lab_byte_plan_count(&bits, &rule, 1));
    }
    assert(!ir_lab_byte_plan_count(NULL, rules, 3));
    assert(!ir_lab_byte_plan_count(&bits, NULL, 3));
    assert(!ir_lab_byte_plan_count(&bits, rules, 0));
    assert(!ir_lab_byte_plan_count(&bits, rules, IR_LAB_MAX_BYTE_RULES + 1));
    bits.frames = IR_LAB_MAX_FRAMES + 1;
    assert(!ir_lab_byte_plan_count(&bits, rules, 3));
    memset(&bits, 0, sizeof(bits));
    bits.valid = true;
    bits.frames = 8;
    bits.count = 512;
    IrLabByteRule maximum_rules[IR_LAB_MAX_BYTE_RULES];
    unsigned position = 0, count = 0;
    for(unsigned f = 0; f < 8; ++f) {
        bits.start[f] = position;
        bits.length[f] = f == 7 ? 113 : 57;
        for(unsigned b = 0; b < 3; ++b)
            if((f >> b) & 1) bits.data[(position + b) / 8] |= 1U << ((position + b) % 8);
        position += bits.length[f];
        for(unsigned byte = 0; byte < (bits.length[f] + 7U) / 8U; ++byte)
            maximum_rules[count++] = (IrLabByteRule){.frame = f, .byte = byte, .from = 0, .to = 0};
    }
    assert(count == IR_LAB_MAX_BYTE_RULES);
    assert(ir_lab_byte_plan_count(&bits, maximum_rules, count) == 1);
    assert(ir_lab_byte_plan_apply(&bits, maximum_rules, count, base, 0, changes));
    puts(
        "Signed multi-byte plans: synchronized steps, fixed bytes, endpoints, aliases, all widths and 71 rules passed");
}

int main(void) {
    test_hex();
    test_linked_fields();
    test_byte_plans();
    IrLabSignal seed = pair(false), edited;
    IrLabBits bits;
    uint8_t changes[IR_LAB_MASK_BYTES] = {0};
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(bits.count == 16 && bits.frames == 2 && bits.length[0] == 8 && bits.length[1] == 8);
    assert(bits.timing[0] == 3 && bits.timing[8] == 23);
    assert(ir_lab_toggle_linked(&bits, changes, 1) == 2);
    assert(ir_lab_changed_count(&bits, changes) == 2);
    assert(ir_lab_apply_bits(&seed, &bits, changes, &edited));
    assert(edited.timings[5] == 500 && edited.timings[25] == 500);
    for(unsigned i = 0; i < seed.count; ++i)
        if(i != 5 && i != 25) assert(seed.timings[i] == edited.timings[i]);
    assert(ir_lab_toggle_linked(&bits, changes, 1) == 2);
    assert(ir_lab_changed_count(&bits, changes) == 0);
    assert(ir_lab_apply_bits(&seed, &bits, changes, &edited));
    assert(memcmp(&seed, &edited, sizeof(seed)) == 0);
    seed = pair(true);
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(ir_lab_toggle_linked(&bits, changes, 1) == 1); // Different seed payload is not linked.
    assert(ir_lab_changed_count(&bits, changes) == 1);
    assert(!ir_lab_toggle_linked(&bits, changes, 16));
    assert(!ir_lab_apply_bits(&seed, &bits, changes, &seed));
    assert(!ir_lab_apply_bits(NULL, &bits, changes, &edited));
    bits.timing[0] = 2;
    assert(!ir_lab_apply_bits(&seed, &bits, changes, &edited));
    seed = pair(false);
    seed.timings[3] = 1050;
    assert(!ir_lab_extract_bits(&seed, &bits));
    assert(!bits.valid);
    seed = pair(false);
    seed.count--;
    assert(!ir_lab_extract_bits(&seed, &bits));
    seed = pair(false);
    seed.timings[0] = 2500;
    assert(!ir_lab_extract_bits(&seed, &bits));
    seed = pair(false);
    seed.timings[4] = 850;
    assert(!ir_lab_extract_bits(&seed, &bits));
    seed = pair(false);
    for(unsigned i = 3; i < 18; i += 2)
        seed.timings[i] = 500;
    assert(!ir_lab_extract_bits(&seed, &bits)); // No long cluster: do not guess.
    seed = pair(false);
    seed.decoded = true;
    assert(!ir_lab_extract_bits(&seed, &bits));
    assert(!ir_lab_extract_bits(NULL, &bits));
    assert(!ir_lab_extract_bits(&seed, NULL));
    seed = pair(false);
    assert(ir_lab_extract_bits(&seed, &bits));
    memset(changes, 0, sizeof(changes));
    assert(ir_lab_set_linked(&bits, changes, 1, false) == 2);
    assert(ir_lab_changed_count(&bits, changes) == 2);
    assert(ir_lab_set_linked(&bits, changes, 1, false) == 2);
    assert(ir_lab_changed_count(&bits, changes) == 2); // Setting is idempotent.
    assert(ir_lab_set_linked(&bits, changes, 1, true) == 2);
    assert(!ir_lab_changed_count(&bits, changes));
    seed = pair(false); // 8-bit custom frame cannot masquerade as full NEC before a repeat.
    seed.count = 19;
    seed.timings[seed.count++] = 30000;
    seed.timings[seed.count++] = 9000;
    seed.timings[seed.count++] = 2250;
    seed.timings[seed.count++] = 500;
    assert(!ir_lab_extract_bits(&seed, &bits));
    assert(strcmp(ir_lab_bits_error(&bits), "Header not recognized") == 0);
    puts("Hex values, partial bytes, range boundaries and repeat linking passed");
}
