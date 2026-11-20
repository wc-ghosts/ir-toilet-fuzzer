#include "ir_lab_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static IrLabDataView view(unsigned value) {
    IrLabDataView result = {.valid = true, .count = 32, .frames = 1, .length = {32}};
    result.data[0] = 0x34;
    result.data[1] = 0x12;
    result.data[2] = value;
    result.data[3] = 0x56;
    return result;
}
int main(void) {
    IrLabDataView views[5];
    for(unsigned n = 0; n < 5; ++n)
        views[n] = view(0x10 + n);
    IrLabComparison result;
    assert(ir_lab_compare(views, 5, 0, &result) == 5);
    assert(result.comparable_mask == 31 && !result.different_layout_mask);
    assert(result.bits == 32 && result.changing_bits == 3);
    assert(result.varying[2] == 7);
    assert(!result.varying[0] && !result.varying[1] && !result.varying[3]);
    int16_t step = 99;
    assert(ir_lab_hex_sequence(views, 5, 0, 2, &step) && step == 1);
    for(unsigned n = 0; n < 5; ++n)
        views[n] = view(0x14 - n);
    assert(ir_lab_hex_sequence(views, 5, 0, 2, &step) && step == -1);
    views[3].data[2] = 0x30;
    assert(!ir_lab_hex_sequence(views, 5, 0, 2, &step));
    for(unsigned n = 0; n < 5; ++n)
        views[n] = view(0x10);
    assert(ir_lab_compare(views, 5, 0, &result) == 5 && !result.changing_bits);
    assert(ir_lab_hex_sequence(views, 5, 0, 2, &step) && step == 0);
    views[3].length[0] = 24;
    views[4].pulse_width = true;
    assert(ir_lab_compare(views, 5, 0, &result) == 3);
    assert(result.comparable_mask == 7 && result.different_layout_mask == 24);
    assert(!ir_lab_hex_sequence(views, 5, 0, 2, &step));
    views[1].valid = false;
    assert(ir_lab_compare(views, 5, 0, &result) == 2 && result.comparable_mask == 5);
    assert(!ir_lab_compare(NULL, 5, 0, &result));
    assert(!ir_lab_compare(views, 6, 0, &result));
    assert(!ir_lab_compare(views, 5, 8, &result));
    assert(!ir_lab_compare(views, 5, 0, NULL));
    assert(!ir_lab_hex_sequence(views, 1, 0, 0, &step));
    assert(!ir_lab_hex_sequence(views, 5, 0, 0, NULL));
    assert(!ir_lab_view_width(NULL, 0, 0));
    for(unsigned n = 0; n < 5; ++n) {
        views[n] = view(0);
        views[n].length[0] = 15;
        views[n].data[1] = 0x7F;
    }
    assert(ir_lab_view_width(&views[0], 0, 1) == 7);
    assert(ir_lab_view_byte(&views[0], 0, 1) == 0x7F);
    assert(!ir_lab_view_width(&views[0], 0, 2));
    views[2].data[1] = 0x3F;
    assert(ir_lab_compare(views, 5, 0, &result) == 5);
    assert(result.changing_bits == 1 && result.varying[1] == 0x40);
    // Invalid metadata must never cause out-of-bounds access.
    views[0].start[0] = 510;
    assert(!ir_lab_view_width(&views[0], 0, 0));
    views[1].count = 513;
    assert(!ir_lab_view_width(&views[1], 0, 0));
    IrLabBits bits = {
        .valid = true, .count = 30, .frames = 2, .start = {0, 15}, .length = {15, 15}};
    bits.data[2] = 0xD5;
    ir_lab_data_view(&bits, &views[0]);
    assert(views[0].valid && views[0].start[1] == 15 && views[0].data[2] == 0xD5);
    assert(ir_lab_view_width(&views[0], 1, 1) == 7);
    ir_lab_data_view(NULL, &views[0]);
    assert(!views[0].valid && !views[0].count);
    int16_t delta = 0;
    IrLabDataView earlier = view(0x10), later = view(0x14);
    assert(ir_lab_hex_delta(&earlier, &later, 0, 2, &delta) && delta == 4);
    earlier = view(0x14);
    later = view(0x19);
    assert(ir_lab_hex_delta(&earlier, &later, 0, 2, &delta) && delta == 5);
    earlier = view(0x19);
    later = view(0x17);
    assert(ir_lab_hex_delta(&earlier, &later, 0, 2, &delta) && delta == -2);
    earlier = view(0xFF);
    later = view(0);
    assert(ir_lab_hex_delta(&earlier, &later, 0, 2, &delta) && delta == -255);
    assert(ir_lab_hex_delta(&later, &earlier, 0, 2, &delta) && delta == 255);
    assert(ir_lab_hex_delta(&earlier, &earlier, 0, 2, &delta) && delta == 0);
    assert(!ir_lab_hex_delta(NULL, &later, 0, 2, &delta));
    assert(!ir_lab_hex_delta(&earlier, NULL, 0, 2, &delta));
    assert(!ir_lab_hex_delta(&earlier, &later, 0, 2, NULL));
    later.valid = false;
    assert(!ir_lab_hex_delta(&earlier, &later, 0, 2, &delta));
    later = view(1);
    later.pulse_width = true;
    assert(!ir_lab_hex_delta(&earlier, &later, 0, 2, &delta));
    later = view(1);
    later.length[0] = 24;
    assert(!ir_lab_hex_delta(&earlier, &later, 0, 2, &delta));
    assert(!ir_lab_hex_delta(&earlier, &earlier, 9, 0, &delta));
    assert(!ir_lab_hex_delta(&earlier, &earlier, 0, 99, &delta));
    puts("Five-capture comparison, layout exclusion, partial bytes and hex sequences passed");
}
