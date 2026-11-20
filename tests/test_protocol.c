#include "ir_lab_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static IrLabSignal toto(unsigned frames, bool bad_check) {
    IrLabSignal s = {.valid = true, .frequency = 38000, .duty_cycle = .33f};
    for(unsigned f = 0; f < frames; ++f) {
        s.timings[s.count++] = 6197;
        s.timings[s.count++] = 2754;
        uint8_t bytes[] = {0x10, 0xE0, (uint8_t)(0xF0 ^ (bad_check && f == 0))};
        for(unsigned b = 0; b < 39; ++b) {
            bool one = b < 15 ? (0x0802 >> b) & 1 : (bytes[(b - 15) / 8] >> ((b - 15) % 8)) & 1;
            s.timings[s.count++] = 600;
            s.timings[s.count++] = one ? 1634 : 516;
        }
        s.timings[s.count++] = 600;
        if(f + 1 < frames) s.timings[s.count++] = 38000;
    }
    return s;
}

static void test_inax(void) {
    IrLabSignal seed = {.valid = true, .frequency = 38000, .duty_cycle = .33f}, output;
    // Distinct frames ensure each frame keeps its own first byte and command.
    for(unsigned f = 0; f < 2; ++f) {
        seed.timings[seed.count++] = 9000;
        seed.timings[seed.count++] = 4500;
        uint8_t data[] = {(uint8_t)(0x3A + f), (uint8_t)(0x10 + f), (uint8_t)(0xEF - f)};
        for(unsigned b = 0; b < 24; ++b) {
            seed.timings[seed.count++] = 560;
            seed.timings[seed.count++] = (data[b / 8] >> (b % 8)) & 1 ? 1675 : 560;
        }
        seed.timings[seed.count++] = 560;
        if(f == 0) seed.timings[seed.count++] = 40000;
    }
    IrLabBits bits;
    IrLabProtocolAnalysis a;
    assert(ir_lab_extract_bits(&seed, &bits));
    assert(ir_lab_protocol_analyze(&seed, &bits, &a));
    assert(a.kind == IrLabProtocolInax && a.checks_passed == 2 && a.unique_frames == 2);
    uint8_t changes[IR_LAB_MASK_BYTES], saved[IR_LAB_MASK_BYTES];
    for(unsigned value = 0; value < 256; ++value) {
        memset(changes, 0, sizeof(changes));
        assert(ir_lab_window_set(&bits, changes, 0, 0, 1, value));
        assert(ir_lab_window_set(&bits, changes, 1, 0, 1, 255 - value));
        assert(ir_lab_protocol_repair_inax(&bits, changes));
        for(unsigned f = 0; f < 2; ++f) {
            assert(ir_lab_window_value(&bits, changes, f, 0, 0) == 0x3A + f);
            assert(
                (ir_lab_window_value(&bits, changes, f, 0, 1) ^
                 ir_lab_window_value(&bits, changes, f, 0, 2)) == 255);
        }
        assert(ir_lab_apply_bits(&seed, &bits, changes, &output));
        IrLabBits emitted;
        assert(ir_lab_extract_bits(&output, &emitted));
        assert(ir_lab_protocol_analyze(&output, &emitted, &a) && a.checks_passed == 2);
    }
    assert(ir_lab_window_set(&bits, changes, 1, 0, 0, 0));
    memcpy(saved, changes, sizeof(saved));
    assert(!ir_lab_protocol_repair_inax(&bits, changes));
    assert(!memcmp(saved, changes, sizeof(saved)));
    memset(changes, 0, sizeof(changes));
    bits.data[(bits.start[1] + 16) / 8] ^= 1U << ((bits.start[1] + 16) % 8);
    memcpy(saved, changes, sizeof(saved));
    assert(!ir_lab_protocol_repair_inax(&bits, changes));
    assert(!memcmp(saved, changes, sizeof(saved)));
    bits.length[1] = 23;
    assert(!ir_lab_protocol_repair_inax(&bits, changes));
    assert(!ir_lab_protocol_repair_inax(NULL, changes));
    assert(!ir_lab_protocol_repair_inax(&bits, NULL));
}

int main(void) {
    test_inax();
    IrLabSignal s = toto(2, false), out;
    IrLabBits bits;
    IrLabProtocolAnalysis a;
    assert(ir_lab_extract_bits(&s, &bits));
    assert(ir_lab_protocol_analyze(&s, &bits, &a));
    assert(
        a.valid && a.kind == IrLabProtocolToto && a.payload_offset == 15 && a.frames == 2 &&
        a.unique_frames == 1 && a.checks_passed == 2);
    uint8_t changes[IR_LAB_MASK_BYTES] = {0};
    IrLabByteRule rules[] = {
        {.frame = 0, .byte = 0, .bit_offset = 15, .from = 0x10, .to = 0x40, .step = 0x10},
        {.frame = 0, .byte = 1, .bit_offset = 15, .from = 0xE0, .to = 0xB0, .step = -0x10}};
    assert(ir_lab_byte_plan_count(&bits, rules, 2) == 4);
    for(unsigned n = 0; n < 4; ++n) {
        uint8_t base[IR_LAB_MASK_BYTES] = {0};
        assert(ir_lab_byte_plan_apply(&bits, rules, 2, base, n, changes));
        assert(ir_lab_protocol_repair_toto(&bits, changes));
        for(unsigned f = 0; f < 2; ++f) {
            assert(ir_lab_window_value(&bits, changes, f, 15, 0) == 0x10 + n * 0x10);
            assert(ir_lab_window_value(&bits, changes, f, 15, 1) == 0xE0 - n * 0x10);
            assert(
                ir_lab_window_value(&bits, changes, f, 15, 2) ==
                ((0x10 + n * 0x10) ^ (0xE0 - n * 0x10)));
            for(unsigned b = 0; b < 15; ++b)
                assert(!ir_lab_bit_get(changes, bits.start[f] + b));
        }
        assert(ir_lab_apply_bits(&s, &bits, changes, &out));
        IrLabBits edited;
        assert(ir_lab_extract_bits(&out, &edited));
        assert(ir_lab_protocol_analyze(&out, &edited, &a) && a.checks_passed == 2);
    }
    IrLabByteRule overlap[] = {
        rules[0], {.frame = 1, .byte = 1, .bit_offset = 8, .from = 0, .to = 1, .step = 1}};
    assert(!ir_lab_byte_plan_count(&bits, overlap, 2)); // Alias overlaps cells 16..22.
    uint8_t saved[IR_LAB_MASK_BYTES];
    memcpy(saved, changes, sizeof(saved));
    ir_lab_set_linked(&bits, changes, 0, true);
    memcpy(saved, changes, sizeof(saved));
    assert(!ir_lab_protocol_repair_toto(&bits, changes));
    assert(!memcmp(saved, changes, sizeof(saved)));
    IrLabDataView raw, payload, copy;
    ir_lab_data_view(&bits, &raw);
    assert(ir_lab_protocol_view(&raw, 15, &payload));
    assert(payload.count == 48 && payload.length[0] == 24 && payload.frames == 2);
    assert(ir_lab_view_byte(&payload, 0, 0) == 0x10 && ir_lab_view_byte(&payload, 0, 2) == 0xF0);
    copy = payload;
    assert(ir_lab_protocol_distance(&payload, &copy) == 0);
    copy.data[0] ^= 1;
    assert(ir_lab_protocol_distance(&payload, &copy) == 1);
    copy.frames = 1;
    assert(ir_lab_protocol_distance(&payload, &copy) == UINT16_MAX);
    assert(!ir_lab_protocol_view(&raw, 39, &payload));
    s = toto(2, true);
    assert(ir_lab_extract_bits(&s, &bits));
    assert(ir_lab_protocol_analyze(&s, &bits, &a));
    assert(a.kind == IrLabProtocolToto && a.checks_passed == 1 && a.checks_tested == 2);
    assert(!ir_lab_protocol_analyze(NULL, &bits, &a) && !a.valid);
    assert(!ir_lab_protocol_analyze(&s, &bits, NULL));
    bits.frames = 9;
    assert(!ir_lab_protocol_analyze(&s, &bits, &a));
    puts(
        "Protocol candidates, 15-bit payload alignment, XOR repair, repeat preservation and data matching passed");
}
