#include "ir_lab_core.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static IrLabSignal raw_signal(void) {
    IrLabSignal signal = {
        .valid = true,
        .frequency = 38000,
        .duty_cycle = 0.33f,
        .count = 5,
        .timings = {7000, 3000, 400, 600, 400},
    };
    return signal;
}

static IrLabSignal decoded_signal(void) {
    IrLabSignal signal = {
        .valid = true,
        .decoded = true,
        .protocol = 0,
        .address_bits = 16,
        .command_bits = 32,
        .address = 0x1234,
        .command = 0xaabbccdd,
    };
    return signal;
}

static void test_raw_validation(void) {
    IrLabSignal signal = raw_signal();
    assert(ir_lab_signal_valid(&signal));
    assert(!ir_lab_signal_valid(NULL));
    signal.count = IR_LAB_MAX_TIMINGS + 1;
    assert(!ir_lab_signal_valid(&signal));
    signal.count = 0;
    assert(!ir_lab_signal_valid(&signal));
    signal = raw_signal();
    signal.duty_cycle = NAN;
    assert(!ir_lab_signal_valid(&signal));
    signal.duty_cycle = INFINITY;
    assert(!ir_lab_signal_valid(&signal));
    signal.duty_cycle = 0.0;
    assert(!ir_lab_signal_valid(&signal));
    signal = raw_signal();
    signal.frequency = 19999;
    assert(!ir_lab_signal_valid(&signal));
    signal.frequency = 60001;
    assert(!ir_lab_signal_valid(&signal));
    signal = raw_signal();
    signal.timings[0] = 9;
    assert(!ir_lab_signal_valid(&signal));
    signal.timings[0] = UINT32_MAX;
    assert(!ir_lab_signal_valid(&signal));
    signal.count = 2;
    signal.timings[0] = 150000;
    signal.timings[1] = 100000;
    assert(ir_lab_signal_valid(&signal));
    signal.count = 6;
    for(unsigned i = 0; i < 6; ++i)
        signal.timings[i] = 150000;
    assert(!ir_lab_signal_valid(&signal));
}

static void test_decoded_mutation(void) {
    IrLabSignal seed = decoded_signal(), output;
    IrLabMutation mutation = {.mode = IrLabCommandBit, .index = 31};
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.command == (seed.command ^ 0x80000000));
    assert(output.address == seed.address);
    assert(seed.command == 0xaabbccdd);
    mutation.index = 32;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation = (IrLabMutation){.mode = IrLabCommandField, .index = 24, .width = 8, .value = 255};
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.command == 0xffbbccdd);
    assert(output.address == seed.address);
    mutation.value = 0;
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.command == 0x00bbccdd);
    mutation.value = 256;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.value = -1;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.value = 0;
    mutation.index = 25;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.index = 0;
    mutation.width = 0;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.width = 9;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation = (IrLabMutation){.mode = IrLabReplay};
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(memcmp(&seed, &output, sizeof(seed)) == 0);
    assert(!ir_lab_mutate(&seed, &mutation, &seed));
    mutation.mode = (IrLabMode)-1;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    assert(strcmp(ir_lab_mode_name(mutation.mode), "Invalid") == 0);
    seed.command_bits = 8;
    assert(!ir_lab_signal_valid(&seed));
    seed = decoded_signal();
    seed.address_bits = 0;
    assert(!ir_lab_signal_valid(&seed));
    seed.address_bits = 33;
    assert(!ir_lab_signal_valid(&seed));
}

static void test_raw_mutation(void) {
    IrLabSignal seed = raw_signal(), output;
    IrLabMutation mutation = {.mode = IrLabRawScale, .index = 3, .value = 50};
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.timings[3] == 900);
    assert(output.timings[2] == seed.timings[2]);
    mutation.value = -50;
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.timings[3] == 300); // Does not scale the previous preview.
    assert(seed.timings[3] == 600);
    mutation.value = 51;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.value = -51;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.value = 0;
    mutation.index = seed.count;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation = (IrLabMutation){.mode = IrLabRawSet, .index = 3, .value = 1600};
    assert(ir_lab_mutate(&seed, &mutation, &output));
    assert(output.timings[3] == 1600);
    mutation.value = 20001;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.value = 0;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    mutation.mode = IrLabCommandBit;
    assert(!ir_lab_mutate(&seed, &mutation, &output));
    seed.count = 6;
    for(unsigned i = 0; i < 4; ++i)
        seed.timings[i] = 150000;
    seed.timings[4] = 149990;
    seed.timings[5] = 10;
    mutation = (IrLabMutation){.mode = IrLabRawScale, .index = 5, .value = 50};
    assert(!ir_lab_mutate(&seed, &mutation, &output)); // Total would exceed cap.
    mutation.value = -50;
    assert(!ir_lab_mutate(&seed, &mutation, &output)); // Pulse would be too short.
}

static void test_gate(void) {
    IrLabGate gate = {0};
    assert(!ir_lab_reserve_trial(&gate, 0));
    gate.armed = true;
    assert(ir_lab_can_send(&gate, 0)); // No STOP dependency.
    assert(ir_lab_reserve_trial(&gate, 100));
    assert(gate.trials == 1 && gate.pending_observation);
    assert(!ir_lab_can_send(&gate, 20000)); // Requires observation.
    ir_lab_observe(&gate, false);
    assert(!ir_lab_can_send(&gate, 10099));
    assert(ir_lab_can_send(&gate, 10100));
    for(unsigned i = 1; i < 1024; ++i) {
        assert(ir_lab_reserve_trial(&gate, 100 + i * IR_LAB_COOLDOWN_MS));
        ir_lab_observe(&gate, false);
    }
    assert(gate.trials == 1024);
    assert(ir_lab_can_send(&gate, 10240000 + 100));
    gate.trials = UINT32_MAX;
    assert(!ir_lab_can_send(&gate, 10240000 + 100));
    gate = (IrLabGate){.armed = true};
    assert(ir_lab_reserve_trial(&gate, UINT32_MAX - 100));
    ir_lab_observe(&gate, false);
    assert(!ir_lab_can_send(&gate, 9800));
    assert(ir_lab_can_send(&gate, 9899)); // Wrap-safe elapsed milliseconds.
    assert(ir_lab_reserve_trial(&gate, 9899));
    ir_lab_observe(&gate, true);
    assert(gate.locked && !gate.armed);
    gate.armed = true;
    assert(!ir_lab_can_send(&gate, 999999)); // Arming cannot clear a lock.
    gate = (IrLabGate){.armed = true};
    ir_lab_stop(&gate);
    assert(!ir_lab_can_send(&gate, 999999));
}

static void test_range_gate(void) {
    IrLabGate gate = {0};
    assert(!ir_lab_can_send_range(NULL, 0, 1000));
    assert(!ir_lab_reserve_range_trial(&gate, 0, 1000));
    gate.armed = true;
    assert(ir_lab_can_send_range(&gate, 0, 1000)); // No STOP dependency.
    assert(!ir_lab_reserve_range_trial(&gate, 0, 0));
    assert(!ir_lab_reserve_range_trial(&gate, 0, 1500));
    assert(!ir_lab_reserve_range_trial(&gate, 0, 3000));
    assert(ir_lab_reserve_range_trial(&gate, 100, 1000));
    assert(!ir_lab_can_send_range(&gate, 1100, 1000));
    gate.pending_observation = false; // Parent logs unrecorded response before continuation.
    assert(!ir_lab_can_send_range(&gate, 1099, 1000));
    assert(ir_lab_can_send_range(&gate, 1100, 1000));
    assert(!ir_lab_can_send_range(&gate, 2099, 2000));
    assert(ir_lab_can_send_range(&gate, 2100, 2000));
    for(unsigned n = 1; n < 1024; ++n) {
        assert(ir_lab_reserve_range_trial(&gate, 100 + n * 1000, 1000));
        gate.pending_observation = false;
    }
    assert(gate.trials == 1024 && ir_lab_can_send_range(&gate, 1024100, 1000));
    gate.trials = UINT32_MAX;
    assert(!ir_lab_can_send_range(&gate, 1024100, 1000));
    gate = (IrLabGate){.armed = true};
    assert(ir_lab_reserve_range_trial(&gate, UINT32_MAX - 100, 1000));
    gate.pending_observation = false;
    assert(!ir_lab_can_send_range(&gate, 898, 1000));
    assert(ir_lab_can_send_range(&gate, 899, 1000));
    ir_lab_stop(&gate);
    gate.armed = true;
    assert(!ir_lab_can_send_range(&gate, 999999, 1000));
}

int main(void) {
    test_raw_validation();
    test_decoded_mutation();
    test_raw_mutation();
    test_gate();
    test_range_gate();
    puts("IR mutation and transmission gate tests passed");
    return 0;
}
