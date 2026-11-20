#define IR_LAB_HOST_SIM
#include "../ir_boundary_lab.c"
#include "../ir_lab_profile.c"

static unsigned sim_checks;
static FILE* sim_report;

static void check(bool condition, const char* name) {
    ++sim_checks;
    if(sim_report) fprintf(sim_report, "%s: %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        abort();
    }
}

static IrLabApp* sim_app(void) {
    mkdir("/tmp/ir-lab-simulation", 0700);
    sim_tick += 1000;
    sim_sd_fail = sim_sync_fail = sim_rename_fail = sim_remove_fail = false;
    sim_meta_rename_fail_after = -1;
    sim_write_hook = sim_tx_hook = NULL;
    IrLabApp* app = calloc(1, sizeof(*app));
    app->storage = calloc(1, sizeof(Storage));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->inputs = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->worker = infrared_worker_alloc();
    app->view = view_port_alloc();
    app->carrier = 38000;
    app->series = 3;
    lab_sets_init(app);
    app->preview_delay = 1;
    app->running = true;
    app->mutation.width = 1;
    app->storage_ready = lab_storage_init(app);
    check(app->storage_ready, "host SD run directory initialized");
    infrared_worker_rx_set_received_signal_callback(app->worker, lab_received, app);
    return app;
}

static void sim_free(IrLabApp* app) {
    lab_rx_stop(app);
    infrared_worker_free(app->worker);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->inputs);
    view_port_free(app->view);
    free(app->storage);
    free(app);
}

static void sim_press(IrLabApp* app, InputKey key, InputType type) {
    InputEvent event = {.key = key, .type = type};
    if(type == InputTypeShort || type == InputTypeLong) {
        InputEvent press = {.key = key, .type = InputTypePress};
        lab_input(&press, app);
    }
    lab_input(&event, app);
    if(__atomic_exchange_n(&app->stop_requested, false, __ATOMIC_RELAXED)) lab_emergency_stop(app);
    lab_preview_poll(app);
    lab_auto_poll(app);
    while(furi_message_queue_get(app->inputs, &event, 0) == FuriStatusOk)
        lab_handle_input(app, &event);
    lab_preview_poll(app);
    lab_auto_poll(app);
    lab_refresh(app);
    Canvas canvas;
    lab_draw(&canvas, app);
}

static void sim_snapshot(IrLabApp* app, const char* title) {
    lab_refresh(app);
    Canvas canvas;
    lab_draw(&canvas, app);
    fprintf(sim_report, "\nSCREEN: %s\n", title);
    for(unsigned i = 0; i < LAB_LINES; ++i)
        fprintf(sim_report, "| %-32s |\n", app->lines[i]);
    fprintf(
        sim_report,
        "META: selected=%d count=%u first=%u\n",
        app->highlighted,
        app->menu_count,
        app->menu_first);
    for(unsigned n = 0; n < sim_draw_count; ++n) {
        SimDraw* d = &sim_draw[n];
        fprintf(
            sim_report,
            "DRAW: {\"op\":\"%c\",\"x\":%d,\"y\":%d,\"x2\":%d,\"y2\":%d,\"color\":%d,\"font\":%d,\"text\":\"",
            d->op,
            d->x,
            d->y,
            d->x2,
            d->y2,
            d->color,
            d->font);
        for(const char* p = d->text; *p; ++p) {
            if(*p == '\"' || *p == '\\') fputc('\\', sim_report);
            fputc(*p, sim_report);
        }
        fprintf(sim_report, "\"}\n");
    }
}

// Generate a synthetic remote press using the real SDK encoder, then deliver it
// through the real SDK decoder and the app's unmodified receive callback.
static IrLabSignal reference_press(InfraredMessage message, unsigned frames) {
    IrLabSignal signal = {
        .valid = true,
        .frequency = infrared_get_protocol_frequency(message.protocol),
        .duty_cycle = infrared_get_protocol_duty_cycle(message.protocol)};
    InfraredEncoderHandler* encoder = infrared_alloc_encoder();
    infrared_reset_encoder(encoder, &message);
    unsigned completed = 0;
    for(unsigned i = 0; completed < frames && i < 4096; ++i) {
        uint32_t duration = 0;
        bool level = false;
        InfraredStatus status = infrared_encode(encoder, &duration, &level);
        assert(status == InfraredStatusOk || status == InfraredStatusDone);
        if(duration && (signal.count || level)) {
            if(signal.count && level == ((signal.count - 1) % 2 == 0))
                signal.timings[signal.count - 1] += duration;
            else {
                assert(signal.count < 1024);
                signal.timings[signal.count++] = duration;
            }
        }
        if(status == InfraredStatusDone) ++completed;
    }
    assert(completed == frames);
    infrared_free_encoder(encoder);
    return signal;
}

static void sim_deliver(IrLabApp* app, const IrLabSignal* signal) {
    assert(app->worker->running);
    InfraredWorkerSignal received = {.count = signal->count};
    memcpy(received.timings, signal->timings, signal->count * sizeof(uint32_t));
    if(app->worker->decoding) {
        InfraredDecoderHandler* decoder = infrared_alloc_decoder();
        for(size_t i = 0; i < signal->count; ++i) {
            const InfraredMessage* message =
                infrared_decode(decoder, i % 2 == 0, signal->timings[i]);
            if(message && !message->repeat) {
                received.message = *message;
                received.decoded = true;
                break;
            }
        }
        if(!received.decoded) {
            const InfraredMessage* message = infrared_check_decoder_ready(decoder);
            if(message && !message->repeat) {
                received.message = *message;
                received.decoded = true;
            }
        }
        infrared_free_decoder(decoder);
    }
    app->worker->callback(app->worker->context, &received);
    lab_capture_poll(app);
}

static unsigned received_frames(const IrLabSignal* signal, InfraredMessage expected) {
    if(signal->frequency != infrared_get_protocol_frequency(expected.protocol)) return 0;
    unsigned frames = 0;
    InfraredDecoderHandler* decoder = infrared_alloc_decoder();
    for(size_t i = 0; i < signal->count; ++i) {
        const InfraredMessage* message = infrared_decode(decoder, i % 2 == 0, signal->timings[i]);
        if(message && message->protocol == expected.protocol &&
           message->address == expected.address && message->command == expected.command)
            ++frames;
    }
    const InfraredMessage* message = infrared_check_decoder_ready(decoder);
    if(message && message->protocol == expected.protocol && message->address == expected.address &&
       message->command == expected.command)
        ++frames;
    infrared_free_decoder(decoder);
    return frames;
}

static void choose(IrLabApp* app, unsigned row) {
    if(row >= lab_menu_count(app))
        fprintf(
            stderr,
            "Invalid menu selection: screen=%u row=%u count=%u\n",
            app->screen,
            row,
            lab_menu_count(app));
    assert(row < lab_menu_count(app));
    while(app->row != row)
        sim_press(app, InputKeyDown, InputTypeShort);
    sim_press(app, InputKeyOk, InputTypeShort);
}

static void set_byte_from_hex(IrLabApp* app, unsigned byte, unsigned value) {
    lab_go(app, LabHex, 0);
    assert(
        app->bits.valid && value <= ((1U << ir_lab_byte_width(&app->bits, app->frame, byte)) - 1));
    while(app->byte < byte)
        sim_press(app, InputKeyRight, InputTypeShort);
    while(app->byte > byte)
        sim_press(app, InputKeyLeft, InputTypeShort);
    while(lab_byte_value(app, true, app->frame, byte) < value)
        sim_press(app, InputKeyUp, InputTypeShort);
    while(lab_byte_value(app, true, app->frame, byte) > value)
        sim_press(app, InputKeyDown, InputTypeShort);
}

static void set_bit_from_signal(IrLabApp* app, unsigned bit, bool value) {
    app->frame = 0;
    unsigned byte = bit / 8;
    unsigned desired = lab_byte_value(app, true, 0, byte);
    if(value)
        desired |= 1U << (bit % 8);
    else
        desired &= ~(1U << (bit % 8));
    set_byte_from_hex(app, byte, desired);
}

static void signal_action(IrLabApp* app, unsigned row) {
    lab_go(app, LabHex, 0);
    sim_press(app, InputKeyOk, InputTypeShort);
    static const unsigned action_rows[] = {2, 3, 0, 4};
    choose(app, action_rows[row]);
}

static IrLabByteRule* focus_rule(IrLabApp* app) {
    for(unsigned n = 0; n < app->rule_count; ++n)
        if(app->rules[n].frame == app->frame && app->rules[n].byte == app->byte)
            return &app->rules[n];
    assert(false);
    return NULL;
}

static void fuzz_options(IrLabApp* app, unsigned row) {
    choose(app, LabRangeMore);
    choose(app, row);
    if(row == 0) sim_press(app, InputKeyBack, InputTypeShort);
}

static void test_hex_flow(void) {
    IrLabApp* app = sim_app();
    sim_snapshot(app, "Main menu: one press to capture");
    choose(app, 0);
    check(
        app->screen == LabCapture && !app->worker->decoding,
        "Capture starts immediately from main menu");
    sim_snapshot(app, "Capture signal");
    InfraredMessage message = {
        .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5678};
    IrLabSignal raw = reference_press(message, 1);
    sim_deliver(app, &raw);
    check(app->screen == LabHex && app->bits.valid, "Capture opens direct hex workspace");
    check(lab_byte_value(app, true, 0, 0) == 0x34, "Captured NECext displays actual payload hex");
    sim_snapshot(app, "Captured hex bytes");
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyUp, InputTypeShort);
    check(
        lab_byte_value(app, true, 0, 0) == 0x35 && sim_tx_count == before,
        "Up increments hex without transmission");
    sim_press(app, InputKeyRight, InputTypeShort);
    sim_press(app, InputKeyDown, InputTypeShort);
    check(
        app->byte == 1 && lab_byte_value(app, true, 0, 1) == 0x11,
        "Right selects next byte and Down decrements");
    check(lab_candidate(app), "Direct hex edit produces waveform");
    message.address = 0x1135;
    check(
        received_frames(&app->candidate, message) == 1,
        "Real SDK decoder verifies two manually edited bytes");
    check(memcmp(&app->seed, &raw, sizeof(raw)) == 0, "Hex editing preserves immutable capture");
    sim_snapshot(app, "Direct hex +/- editor");
    check(sim_boxes >= 2 && sim_inversions >= 4, "Renderer highlights selected hex byte");
    sim_press(app, InputKeyOk, InputTypeLong);
    check(sim_tx_count == before, "Holding OK in workspace cannot transmit");
    signal_action(app, 3);
    choose(app, 3);
    check(
        app->screen == LabMessage && strstr(app->message[0], "saved") && sim_tx_count == before,
        "Save hex exports waveform without TX");
    char path[180], line[16384];
    snprintf(
        path, sizeof(path), "%s/edited-%03lu.ir", app->directory, (unsigned long)app->capture_id);
    FILE* file = fopen(path, "r");
    assert(file);
    unsigned exported = 0;
    while(fgets(line, sizeof(line), file)) {
        if(strncmp(line, "data:", 5)) continue;
        char* p = line + 5;
        while(*p) {
            char* end;
            unsigned long duration = strtoul(p, &end, 10);
            if(end == p) break;
            assert(
                exported < app->candidate.count && duration == app->candidate.timings[exported++]);
            p = end;
        }
    }
    fclose(file);
    check(exported == app->candidate.count, "Exported .ir equals actual hex candidate");
    sim_press(app, InputKeyOk, InputTypeShort);
    signal_action(app, 0);
    check(
        app->screen == LabSend && !app->stop.valid && !app->rx_running &&
            lab_byte_value(app, true, 0, 0) == 0x35,
        "Edited first send opens confirmation directly with no STOP capture");
    sim_snapshot(app, "Send current hex");
    sim_press(app, InputKeyOk, InputTypeShort);
    check(sim_tx_count == before, "Short OK cannot send on confirmation");
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        sim_tx_count == before + 1 && app->screen == LabResult,
        "One hold sends one edited waveform");
    message.command = 0x5678;
    check(
        received_frames(&sim_last_tx, message) == 1, "Actual modeled TX decodes as displayed hex");
    sim_snapshot(app, "Manual response");
    choose(app, 1);
    check(
        app->screen == LabHex && !app->gate.pending_observation,
        "Beep observation returns directly to hex");
    signal_action(app, 1);
    check(app->screen == LabMessage && strstr(app->message[1], "Wait"), "Cooldown blocks replay");
    sim_tick += 10000;
    signal_action(app, 1);
    sim_press(app, InputKeyOk, InputTypeLong);
    check(memcmp(&sim_last_tx, &raw, sizeof(raw)) == 0, "Replay original ignores manual edits");
    choose(app, 0);
    sim_press(app, InputKeyUp, InputTypeShort);
    check(app->mutation.mode == IrLabDataBits, "Editing after original replay restores hex mode");
    message.command = 0;
    IrLabSignal off = reference_press(message, 1);
    lab_capture_start(app, true); // Optional user-selected STOP remains available.
    sim_deliver(app, &off);
    sim_press(app, InputKeyBack, InputTypeLong);
    check(
        app->gate.locked && memcmp(&sim_last_tx, &off, sizeof(off)) == 0,
        "Hold Back sends unchanged STOP and locks");
    sim_free(app);
}

static IrLabSignal repeat_press(void) {
    InfraredMessage message = {.protocol = InfraredProtocolNEC, .address = 1, .command = 21};
    IrLabSignal raw = reference_press(message, 1);
    raw.timings[raw.count++] = 40000;
    raw.timings[raw.count++] = 9000;
    raw.timings[raw.count++] = 2250;
    raw.timings[raw.count++] = 560;
    return raw;
}
static void test_original_replay(InfraredProtocol protocol) {
    IrLabApp* app = sim_app();
    app->carrier = infrared_get_protocol_frequency(protocol);
    InfraredMessage message = {.protocol = protocol, .address = 1, .command = 21};
    IrLabSignal raw = reference_press(message, infrared_get_protocol_min_repeat_count(protocol));
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    message.command = 0;
    IrLabSignal off = reference_press(message, infrared_get_protocol_min_repeat_count(protocol));
    lab_capture_start(app, true);
    sim_deliver(app, &off);
    lab_go(app, LabSignal, 0);
    choose(app, 3);
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        sim_tx_count == before + 1 && memcmp(&sim_last_tx, &app->seed, sizeof(app->seed)) == 0,
        "Original send menu preserves full captured waveform");
    message.command = 21;
    check(
        received_frames(&sim_last_tx, message) >= infrared_get_protocol_min_repeat_count(protocol),
        "real SDK decoder receives baseline payload with required repeats");
    sim_free(app);
}

static void test_nec_repeats(void) {
    IrLabApp* app = sim_app();
    IrLabSignal raw = repeat_press();
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    check(
        app->bits.valid && app->bits.count == 32 && app->bits.repeats == 1,
        "FIXED: full NEC packet plus short repeat remains editable");
    unsigned original = ir_lab_bit_get(app->bits.data, 16);
    set_bit_from_signal(app, 16, !original);
    check(lab_candidate(app), "NEC with repeat produces edited candidate");
    unsigned selected = app->bits.timing[16];
    for(unsigned i = 0; i < raw.count; ++i)
        if(i != selected) assert(raw.timings[i] == app->candidate.timings[i]);
    check(
        app->candidate.timings[selected] != raw.timings[selected],
        "edit changes selected cell and preserves every repeat/header/gap timing");
    sim_snapshot(app, "Editing an actual NEC repeat capture");
    lab_go(app, LabSignal, 0);
    choose(app, 4);
    choose(app, 5);
    sim_snapshot(app, "Signal details with short repeats");
    sim_free(app);
}

static IrLabApp* hook_app;
static void request_stop_during_write(void) {
    __atomic_store_n(&hook_app->stop_requested, true, __ATOMIC_RELAXED);
}
static void test_faults(void) {
    IrLabApp* app = sim_app();
    IrLabSignal raw = repeat_press();
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    lab_capture_start(app, true);
    sim_deliver(app, &raw);
    lab_go(app, LabSignal, 0);
    choose(app, 3);
    unsigned before = sim_tx_count;
    sim_sync_fail = true;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(sim_tx_count == before && app->gate.locked, "SD failure blocks emission");
    sim_press(app, InputKeyBack, InputTypeLong);
    check(sim_tx_count == before + 1, "STOP bypasses failed SD");
    sim_free(app);
    app = sim_app();
    lab_capture_start(app, false);
    sim_tick += 15001;
    lab_capture_poll(app);
    check(
        !app->rx_running && app->screen == LabMessage && !app->seed.valid,
        "RX timeout displays clear failure and stops capture");
    sim_free(app);
    app = sim_app();
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    app->gate.pending_observation = true;
    lab_go(app, LabSignal, 0);
    choose(app, 3);
    check(app->screen == LabResult, "pending result cannot be bypassed through send menu");
    sim_free(app);
    app = sim_app();
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    lab_capture_start(app, true);
    sim_deliver(app, &raw);
    lab_go(app, LabSignal, 0);
    choose(app, 3);
    hook_app = app;
    sim_write_hook = request_stop_during_write;
    before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(sim_tx_count == before && app->stop_requested, "STOP during SD writing cancels trial");
    sim_write_hook = NULL;
    sim_press(app, InputKeyBack, InputTypeLong);
    check(
        sim_tx_count == before + 1 && app->gate.locked, "queued STOP sent after cancelled trial");
    sim_free(app);
}

static void test_frame_navigation(void) {
    IrLabApp* app = sim_app();
    IrLabSignal raw = {.valid = true, .frequency = 38000, .duty_cycle = .33f};
    for(unsigned f = 0; f < 8; ++f) {
        raw.timings[raw.count++] = 9000;
        raw.timings[raw.count++] = 4500;
        for(unsigned b = 0; b < 56; ++b) {
            raw.timings[raw.count++] = 400;
            raw.timings[raw.count++] = b % 2 ? 1200 : 400;
        }
        raw.timings[raw.count++] = 400;
        if(f < 7) raw.timings[raw.count++] = 8000;
    }
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    signal_action(app, 3);
    choose(app, 0);
    sim_snapshot(app, "Optional frame selection");
    choose(app, 7);
    check(
        app->screen == LabHex && app->frame == 7,
        "Frame selector returns directly to hex workspace");
    set_byte_from_hex(app, 6, 0xAB);
    check(
        ir_lab_changed_count(&app->bits, app->changes) == 8,
        "Hex edit links identical frames including frame eight");
    sim_free(app);
}

static void test_width_edit(InfraredProtocol protocol, unsigned bits) {
    IrLabApp* app = sim_app();
    app->carrier = 40000;
    InfraredMessage message = {.protocol = protocol, .address = 1, .command = 21};
    IrLabSignal raw = reference_press(message, 3);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    check(
        app->bits.valid && app->bits.pulse_width && app->bits.frames == 3 &&
            app->bits.length[0] == bits,
        "SIRC-family raw capture is editable with full repeated frames");
    set_bit_from_signal(app, 0, false);
    check(
        lab_candidate(app) && ir_lab_changed_count(&app->bits, app->changes) == 3,
        "width edit changes selected bit in all matching frames");
    message.command = 20;
    check(
        received_frames(&app->candidate, message) >= 3,
        "real SDK decoder receives edited SIRC command with original address");
    for(unsigned i = 0; i < raw.count; ++i) {
        bool selected = false;
        for(unsigned b = 0; b < app->bits.count; ++b)
            if(app->bits.timing[b] == i && ir_lab_bit_get(app->changes, b)) selected = true;
        if(!selected) assert(raw.timings[i] == app->candidate.timings[i]);
    }
    check(
        memcmp(&app->seed, &raw, sizeof(raw)) == 0,
        "pulse-width editing preserves all spaces, headers, gaps and seed");
    sim_snapshot(app, "SIRC data-bit editing");
    sim_free(app);
}

static unsigned fixture_count;
static void test_reference(IrLabSignal* raw, unsigned expected_bits, const char* name) {
    IrLabApp* app = sim_app();
    lab_capture_start(app, false);
    sim_deliver(app, raw);
    check(app->screen == LabHex && app->bits.valid, "reference opens direct hex workspace");
    for(unsigned f = 0; f < app->bits.frames; ++f)
        check(
            app->bits.length[f] == expected_bits,
            "observed cell count and partial final byte retained");
    IrLabProtocolAnalysis analysis;
    check(
        ir_lab_protocol_analyze(&app->seed, &app->bits, &analysis),
        "Reference protocol timing/layout analysis succeeds");
    check(
        (expected_bits == 32 ?
             analysis.kind >= IrLabProtocolNec && analysis.kind <= IrLabProtocolNecExt :
             analysis.kind == (expected_bits == 39 ? IrLabProtocolToto :
                               expected_bits == 24 ? IrLabProtocolInax :
                                                     IrLabProtocolBio71)),
        "Reference family candidate matches published or corpus-supported layout");
    check(
        expected_bits == 32 || (analysis.checks_passed == analysis.frames &&
                                analysis.checks_tested == analysis.frames),
        "Protocol relation checked against every reference frame");
    IrLabSignal seed = app->seed;
    app->mutation.mode = IrLabDataBits;
    check(
        lab_candidate(app) && memcmp(&app->candidate, &seed, sizeof(seed)) == 0,
        "zero edits preserve reference waveform exactly");
    unsigned bit = expected_bits > 24 ? 24 : 8;
    unsigned before = sim_tx_count;
    set_bit_from_signal(app, bit, !ir_lab_bit_get(app->bits.data, bit));
    unsigned changes = ir_lab_changed_count(&app->bits, app->changes);
    check(
        changes >= 1 && lab_candidate(app) && sim_tx_count == before,
        "selected reference bit previews without TX");
    unsigned timing_changes = 0;
    for(size_t i = 0; i < seed.count; ++i) {
        if(seed.timings[i] == app->candidate.timings[i]) continue;
        ++timing_changes;
        bool mapped = false;
        for(unsigned b = 0; b < app->bits.count; ++b)
            if(app->bits.timing[b] == i && ir_lab_bit_get(app->changes, b)) mapped = true;
        check(mapped && i % 2 == 1, "only selected data spaces change");
    }
    check(
        timing_changes == changes && memcmp(&app->seed, &seed, sizeof(seed)) == 0,
        "headers, marks, gaps, trailer and seed remain unchanged");
    IrLabBits edited;
    check(ir_lab_extract_bits(&app->candidate, &edited), "mutated reference retains framing");
    for(unsigned b = 0; b < app->bits.count; ++b)
        assert(
            ir_lab_bit_get(edited.data, b) ==
            (ir_lab_bit_get(app->bits.data, b) ^ ir_lab_bit_get(app->changes, b)));
    for(unsigned b = 0; b < expected_bits; ++b) {
        memset(app->changes, 0, sizeof(app->changes));
        ir_lab_toggle_linked(&app->bits, app->changes, b);
        assert(lab_candidate(app));
    }
    check(true, "every first-frame bit previews independently");
    memset(app->changes, 0, sizeof(app->changes));
    bit = expected_bits - 1;
    set_bit_from_signal(app, bit, !ir_lab_bit_get(app->bits.data, bit));
    lab_go(app, LabHex, 0);
    sim_snapshot(app, name);
    for(unsigned i = 0; i < 100; ++i) {
        sim_press(app, InputKeyRight, InputTypeShort);
        assert(app->byte < (expected_bits + 7) / 8);
    }
    check(true, "Hex byte selector remains bounded across partial bytes and repeated frames");
    unsigned maximum = (1U << ir_lab_byte_width(&app->bits, 0, app->byte)) - 1;
    set_byte_from_hex(app, app->byte, maximum);
    sim_press(app, InputKeyUp, InputTypeRepeat);
    check(
        lab_byte_value(app, true, 0, app->byte) == maximum,
        "Hex increment clamps at observed byte width");
    set_byte_from_hex(app, app->byte, 0);
    sim_press(app, InputKeyDown, InputTypeRepeat);
    check(lab_byte_value(app, true, 0, app->byte) == 0, "Hex decrement clamps at zero");
    ++fixture_count;
    sim_free(app);
}

static void test_reference_file(const char* path, unsigned expected_bits) {
    FILE* file = fopen(path, "r");
    assert(file);
    char line[16384], name[80] = {0};
    IrLabSignal raw = {.valid = true, .duty_cycle = 0.33f};
    while(fgets(line, sizeof(line), file)) {
        if(strncmp(line, "name:", 5) == 0) {
            snprintf(name, sizeof(name), "%.70s", line + 6);
            name[strcspn(name, "\r\n")] = 0;
        } else if(strncmp(line, "frequency:", 10) == 0)
            raw.frequency = strtoul(line + 10, NULL, 10);
        else if(strncmp(line, "data:", 5) == 0) {
            raw.count = 0;
            char* p = line + 5;
            while(*p) {
                char* end;
                unsigned long value = strtoul(p, &end, 10);
                if(end == p) break;
                assert(raw.count < IR_LAB_MAX_TIMINGS && value <= UINT32_MAX);
                raw.timings[raw.count++] = value;
                p = end;
            }
            fprintf(sim_report, "\nREFERENCE: %s / %s\n", path, name);
            test_reference(&raw, expected_bits, name);
        }
    }
    fclose(file);
}

static void test_parsed_references(void) {
    FILE* file = fopen("tests/fixtures/biobidet_bb2000.ir", "r");
    assert(file);
    char line[256], name[80] = {0};
    InfraredMessage message = {.protocol = InfraredProtocolNECext};
    while(fgets(line, sizeof(line), file)) {
        unsigned b[4];
        if(strncmp(line, "name:", 5) == 0) {
            snprintf(name, sizeof(name), "%.70s", line + 6);
            name[strcspn(name, "\r\n")] = 0;
        } else if(sscanf(line, "address: %x %x %x %x", &b[0], &b[1], &b[2], &b[3]) == 4)
            message.address = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
        else if(sscanf(line, "command: %x %x %x %x", &b[0], &b[1], &b[2], &b[3]) == 4) {
            message.command = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
            IrLabSignal raw = reference_press(message, 1);
            fprintf(
                sim_report,
                "\nPARSED REFERENCE: %s synthesized with SDK, no optical capture\n",
                name);
            test_reference(&raw, 32, name);
        }
    }
    fclose(file);
}

static void test_unsupported(void) {
    IrLabApp* app = sim_app();
    InfraredMessage message = {.protocol = InfraredProtocolRC5, .address = 1, .command = 21};
    IrLabSignal raw = reference_press(message, 1);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    check(
        app->seed.valid && !app->bits.valid && app->screen == LabHex,
        "Manchester capture remains available with editing unsupported");
    sim_free(app);
    app = sim_app();
    message.protocol = InfraredProtocolNEC;
    raw = reference_press(message, 1);
    raw.timings[3] = 1050;
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    check(!app->bits.valid && app->screen == LabHex, "ambiguous timing cannot enter bit editor");
    sim_free(app);
}

static void test_capture_bank(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    choose(app, LabHomeSets);
    check(app->screen == LabSeries, "Capture five starts with descriptive labels");
    choose(app, 0);
    check(app->screen == LabSlots, "Capture bank shows five slots together");
    sim_snapshot(app, "Five capture slots: pressure labels");
    IrLabSignal originals[5];
    for(unsigned n = 0; n < 5; ++n) {
        choose(app, n);
        check(
            app->screen == LabCapture && app->capture_slot == n,
            "Empty slot starts its own capture");
        InfraredMessage message = {
            .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5610 + n};
        originals[n] = reference_press(message, 1);
        sim_deliver(app, &originals[n]);
        check(
            app->slots[n].count && !app->rx_running && app->slots[n].count == originals[n].count,
            "A remote press fills one slot and stops listening");
        if(n < 4)
            check(
                app->screen == LabSlots && app->row == n + 1,
                "Next empty slot is highlighted without auto-capture");
    }
    check(
        app->screen == LabCompare && lab_slot_count(app) == 5 &&
            app->comparison.comparable_mask == 31,
        "Fifth capture opens comparison of all five signals");
    check(
        app->compare_byte == 2 && app->comparison.changing_bits == 3,
        "Comparison opens at first observed varying byte");
    check(sim_tx_count == before, "Capture and comparison never transmit");
    sim_snapshot(app, "Five captures with adjacent signed hex subtraction");
    choose(app, 2);
    choose(app, 4);
    check(
        app->screen == LabPattern && strstr(app->lines[4], "14-13=+01"),
        "Step details show subtraction for every adjacent pair");
    sim_snapshot(app, "Pattern summary: observed hex increment");
    sim_press(app, InputKeyOk, InputTypeShort);
    choose(app, 2);
    choose(app, 1);
    check(
        app->screen == LabHex && app->active_slot == 2 && lab_byte_value(app, true, 0, 2) == 0x12,
        "Selected capture moves to editable workspace");
    sim_press(app, InputKeyUp, InputTypeShort);
    sim_snapshot(app, "Edit a selected capture while retaining original bank");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabHomeSets);
    choose(app, 0);
    choose(app, 0);
    choose(app, 0);
    check(
        app->active_slot == 0 && lab_byte_value(app, true, 0, 2) == 0x10,
        "Selecting another signal preserves its own data");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabHomeSets);
    choose(app, 0);
    choose(app, 2);
    choose(app, 0);
    check(
        app->active_slot == 2 && lab_byte_value(app, true, 0, 2) == 0x13,
        "Returning to capture restores its separate edits");
    for(unsigned n = 0; n < 5; ++n) {
        check(
            lab_slot_read(app, &app->slots[n], &app->candidate) &&
                memcmp(&app->candidate, &originals[n], sizeof(app->candidate)) == 0,
            "Capture-bank timing originals remain immutable");
        check(
            ir_lab_view_byte(&app->views[n], 0, 2) == 0x10 + n,
            "Comparison uses captured data rather than edited copies");
    }
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabHomeSets);
    sim_snapshot(app, "Capture sets show independent counts");
    choose(app, 1);
    check(
        app->screen == LabSlots && app->series == 1 && !lab_slot_count(app) && !app->seed.valid &&
            lab_set_count(&app->sets[0]) == 5,
        "Opening empty water-temperature set retains five pressure captures");
    app->gate.trials = 7;
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabHomeSets);
    choose(app, 0);
    check(
        app->screen == LabSlots && app->series == 0 && lab_slot_count(app) == 5 &&
            app->active_slot == 2 && lab_byte_value(app, true, 0, 2) == 0x13 &&
            app->gate.trials == 7,
        "Returning to pressure restores original captures, active signal and separate edit");
    check(sim_tx_count == before, "Bank operations have no IR side effects");
    fprintf(
        sim_report,
        "CAPTURE MEMORY: app=%zu bytes; SD-backed timing arrays require no per-slot RAM allocation\n",
        sizeof(*app));
    sim_free(app);
}

static void open_set_from_home(IrLabApp* app, unsigned series) {
    lab_go(app, LabHome, 0);
    choose(app, LabHomeSets);
    choose(app, series);
}

static IrLabSignal set_reference(unsigned series, unsigned slot) {
    InfraredMessage message = {
        .protocol = InfraredProtocolNECext,
        .address = 0x1234,
        .command = 0x5000 + series * 0x100 + slot};
    return reference_press(message, 1);
}

static void test_independent_capture_sets(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    app->gate.trials = 7;
    for(unsigned series = 0; series < 4; ++series) {
        open_set_from_home(app, series);
        check(
            app->screen == LabSlots && !lab_slot_count(app),
            "A new capture type opens its own empty five slots without replacement prompts");
        for(unsigned slot = 0; slot < IR_LAB_MAX_CAPTURES; ++slot) {
            choose(app, slot);
            IrLabSignal raw = set_reference(series, slot);
            sim_deliver(app, &raw);
            check(
                app->slots[slot].count == raw.count,
                "Each capture is indexed in the selected set only");
        }
        choose(app, 2);
        choose(app, 0);
        set_byte_from_hex(app, 2, 0x22 + series);
        sim_press(app, InputKeyRight, InputTypeShort);
        check(
            app->active_slot == 2 && app->byte == 3,
            "Each set retains its own selected capture and byte cursor");
    }
    lab_go(app, LabSeries, 0);
    sim_snapshot(app, "Four independent capture sets: all twenty slots retained");
    for(unsigned series = 0; series < 4; ++series) {
        open_set_from_home(app, series);
        check(
            app->series == series && lab_slot_count(app) == 5 && app->active_slot == 2 &&
                app->byte == 3 && lab_byte_value(app, true, 0, 2) == 0x22 + series,
            "Switching capture type restores its five originals, selected signal, edits and cursor");
        check(
            app->gate.trials == 7 && !app->stop.valid,
            "Switching sets neither resets the session budget nor requests STOP");
        for(unsigned slot = 0; slot < IR_LAB_MAX_CAPTURES; ++slot) {
            IrLabSignal expected = set_reference(series, slot);
            check(
                lab_slot_read(app, &app->slots[slot], &app->candidate) &&
                    memcmp(&app->candidate, &expected, sizeof(expected)) == 0,
                "All twenty SD-backed originals reload with exact waveform, carrier and duty");
            check(
                ir_lab_view_byte(&app->views[slot], 0, 2) == slot &&
                    ir_lab_view_byte(&app->views[slot], 0, 3) == 0x50 + series,
                "Comparison caches stay isolated per capture type and ignore edits");
        }
        sim_snapshot(
            app,
            series == 0 ? "Pressure set restored with five captures" :
            series == 1 ? "Water temperature set restored with five captures" :
            series == 2 ? "Seat temperature set restored with five captures" :
                          "Custom set restored with five captures");
    }
    IrLabSignal current = app->seed;
    uint8_t changes[IR_LAB_MASK_BYTES];
    memcpy(changes, app->changes, sizeof(changes));
    sim_sd_fail = true;
    open_set_from_home(app, 0);
    check(
        app->screen == LabMessage && app->series == 3 &&
            memcmp(&app->seed, &current, sizeof(current)) == 0 &&
            memcmp(app->changes, changes, sizeof(changes)) == 0,
        "Failed SD read leaves the current set, waveform and edits intact");
    for(unsigned series = 0; series < 4; ++series)
        check(
            lab_set_count(&app->sets[series]) == 5,
            "A failed set switch cannot discard any of the twenty captures");
    sim_sd_fail = false;
    sim_press(app, InputKeyOk, InputTypeShort);
    choose(app, 0);
    check(
        app->series == 0 && lab_slot_count(app) == 5,
        "Retrying a failed set switch opens the retained captures");
    // Remove just the target file temporarily: incomplete reads must be transactional too.
    char path[160], backup[170];
    snprintf(
        path,
        sizeof(path),
        "%s/seed-%03lu.ir",
        app->directory,
        (unsigned long)app->sets[1].slots[2].seed_id);
    snprintf(backup, sizeof(backup), "%s.backup", path);
    assert(rename(path, backup) == 0);
    current = app->seed;
    open_set_from_home(app, 1);
    check(
        app->screen == LabMessage && app->series == 0 &&
            memcmp(&current, &app->seed, sizeof(current)) == 0,
        "Missing target capture file cannot replace the current set");
    assert(rename(backup, path) == 0);
    // Recapture one pressure slot; every other set and pressure slot must remain intact.
    open_set_from_home(app, 0);
    uint32_t old_id = app->slots[0].seed_id;
    choose(app, 0);
    choose(app, 2);
    InfraredMessage message = {
        .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x50AA};
    IrLabSignal replacement = reference_press(message, 1);
    sim_deliver(app, &replacement);
    check(
        app->slots[0].seed_id != old_id && ir_lab_view_byte(&app->views[0], 0, 2) == 0xAA &&
            app->slots[2].changes[2] != 0,
        "Recapturing one slot updates only that slot and keeps other edits");
    for(unsigned series = 1; series < 4; ++series) {
        open_set_from_home(app, series);
        check(
            lab_slot_count(app) == 5 && lab_byte_value(app, true, 0, 2) == 0x22 + series,
            "Recapturing pressure leaves every other capture type unchanged");
    }
    check(
        sim_tx_count == before, "Creating, switching and restoring all capture sets emits no IR");
    sim_free(app);
}

static void test_maximum_capture_sets(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    IrLabSignal raw = {
        .valid = true, .frequency = 38000, .duty_cycle = 0.33f, .count = IR_LAB_MAX_TIMINGS};
    for(unsigned n = 0; n < raw.count; ++n)
        raw.timings[n] = n % 2 ? 500 : 400;
    raw.timings[0] = 8000;
    raw.timings[1] = 4000;
    for(unsigned series = 0; series < LAB_SETS; ++series) {
        open_set_from_home(app, series);
        for(unsigned slot = 0; slot < lab_set_capacity(series); ++slot) {
            choose(app, slot);
            raw.timings[3] = 500 + series * 10 + slot;
            sim_deliver(app, &raw);
            check(
                app->slots[slot].count == IR_LAB_MAX_TIMINGS,
                "Twenty-four maximum-size captures fit without allocating timing arrays in RAM");
        }
    }
    for(unsigned series = 0; series < LAB_SETS; ++series) {
        open_set_from_home(app, series);
        check(
            app->series == series && lab_slot_count(app) == lab_set_capacity(series),
            "A full maximum-size capture set remains selectable");
        for(unsigned slot = 0; slot < lab_set_capacity(series); ++slot)
            check(
                lab_slot_read(app, &app->slots[slot], &app->candidate) &&
                    app->candidate.count == IR_LAB_MAX_TIMINGS &&
                    app->candidate.timings[3] == 500 + series * 10 + slot,
                "Every maximum-size original reloads from its independent SD file");
    }
    check(sim_tx_count == before, "Maximum-size set management emits no IR");
    fprintf(
        sim_report,
        "TWENTY-FOUR CAPTURES: app=%zu bytes; maximum waveform data stays on SD\n",
        sizeof(*app));
    sim_free(app);
}

static void test_compare_second_frame(void) {
    IrLabApp* app = sim_app();
    app->capture_batch = true;
    for(unsigned n = 0; n < 5; ++n) {
        InfraredMessage message = {
            .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5600};
        IrLabSignal raw = reference_press(message, 1);
        message.command = 0x5610 + n;
        IrLabSignal second = reference_press(message, 1);
        raw.timings[raw.count++] = 40000;
        memcpy(raw.timings + raw.count, second.timings, second.count * sizeof(uint32_t));
        raw.count += second.count;
        app->capture_slot = n;
        lab_capture_start(app, false);
        sim_deliver(app, &raw);
    }
    check(
        app->screen == LabCompare && app->compare_frame == 1 && app->compare_byte == 2,
        "Comparison automatically opens the later frame containing differences");
    choose(app, 3);
    choose(app, 0);
    check(
        app->screen == LabRange && app->active_slot == 3 && app->frame == 1 && app->byte == 2,
        "Direct fuzzer transfer retains a differing later frame and selected capture");
    sim_press(app, InputKeyBack, InputTypeShort);
    lab_open_compare(app);
    choose(app, 0);
    choose(app, 3);
    check(
        app->screen == LabCompare && app->compare_frame == 0 && !app->comparison.changing_bits,
        "Next data frame can still inspect the identical first frame");
    sim_snapshot(app, "Compare a later data section across five captures");
    sim_free(app);
}

static void test_original_without_stop(void) {
    IrLabApp* app = sim_app();
    InfraredMessage message = {.protocol = InfraredProtocolNECext, .address = 1, .command = 21};
    IrLabSignal raw = reference_press(message, 1);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    signal_action(app, 0);
    check(
        app->screen == LabSend && !app->stop.valid,
        "Unedited current-hex replay does not require STOP either");
    sim_press(app, InputKeyBack, InputTypeShort);
    signal_action(app, 1);
    check(
        app->screen == LabSend && !app->stop.valid,
        "Unchanged replay no longer requires STOP capture");
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        sim_tx_count == before + 1 && memcmp(&sim_last_tx, &raw, sizeof(raw)) == 0,
        "Unchanged single replay works directly after capture");
    sim_free(app);
}

static IrLabApp* auto_app(unsigned delay, bool stop) {
    IrLabApp* app = sim_app();
    InfraredMessage message = {
        .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5678};
    IrLabSignal raw = reference_press(message, 1);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    if(stop) {
        message.command = 0;
        raw = reference_press(message, 1);
        lab_capture_start(app, true);
        sim_deliver(app, &raw);
    }
    set_byte_from_hex(app, 0, 0x35);
    set_byte_from_hex(app, 2, 0x10);
    signal_action(app, 2);
    focus_rule(app)->from = 0x10;
    focus_rule(app)->to = 0x12;
    focus_rule(app)->step = 1;
    if(delay == 2) choose(app, LabRangeDelay);
    choose(app, LabRangeStart);
    return app;
}

static void check_auto_saved(IrLabApp* app, const IrLabSignal* emitted, unsigned value) {
    char path[180], line[16384];
    snprintf(path, sizeof(path), "%s/%s.ir", app->directory, app->auto_saved_name);
    FILE* file = fopen(path, "r");
    assert(file);
    unsigned count = 0, frequency = 0;
    float duty = 0;
    while(fgets(line, sizeof(line), file)) {
        if(sscanf(line, "frequency: %u", &frequency) == 1) continue;
        if(sscanf(line, "duty_cycle: %f", &duty) == 1) continue;
        if(strncmp(line, "data:", 5)) continue;
        char* p = line + 5;
        while(*p) {
            char* end;
            unsigned long duration = strtoul(p, &end, 10);
            if(end == p) break;
            assert(count < emitted->count && duration == emitted->timings[count++]);
            p = end;
        }
    }
    fclose(file);
    check(
        count == emitted->count && frequency == emitted->frequency && duty == emitted->duty_cycle,
        "Pause file contains exact last emitted timings, carrier and duty cycle");
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, app->auto_saved_name);
    file = fopen(path, "r");
    assert(file);
    char note[2000] = {0}, expected[80];
    fread(note, 1, sizeof(note) - 1, file);
    fclose(file);
    snprintf(expected, sizeof(expected), "\"value_hex\": \"%02X\"", value);
    check(
        strstr(note, expected) && strstr(note, "\"receiver_response\": \"not_recorded\""),
        "Saved investigation note records actual value without fabricating beep detection");
    snprintf(
        expected,
        sizeof(expected),
        "\"last_emitted_trial\": %lu",
        (unsigned long)app->auto_last_trial);
    check(
        strstr(note, expected) && app->auto_save_waveform && app->auto_save_note,
        "Investigation note identifies actual emission and both saved files");
}

static void finish_packet_hook(void) {
    sim_tick += 250;
}
static void test_live_auto_range(unsigned delay, bool descending) {
    IrLabApp* app = auto_app(delay, false);
    if(descending) {
        sim_press(app, InputKeyBack, InputTypeShort);
        signal_action(app, 2);
        focus_rule(app)->from = 0x12;
        focus_rule(app)->to = 0x10;
        focus_rule(app)->step = -1;
        choose(app, LabRangeStart);
    }
    unsigned before = sim_tx_count;
    check(
        app->screen == LabRangeRun && app->range_live && !app->auto_started,
        "Live range opens explicit hold-to-start confirmation");
    sim_snapshot(app, "Live range confirmation");
    sim_press(app, InputKeyOk, InputTypeShort);
    check(sim_tx_count == before, "Selecting range or short OK cannot start transmission");
    sim_tx_hook = finish_packet_hook;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        sim_tx_count == before + 1 && app->auto_sent_count == 1 && !app->auto_paused,
        "One start hold begins live automatic range with first emission");
    for(unsigned n = 0; n < 3; ++n) {
        unsigned value = descending ? 0x12 - n : 0x10 + n;
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext, .address = 0x1235, .command = 0x5600 | value};
        check(
            received_frames(&sim_last_tx, expected) == 1,
            "Real SDK decodes emitted automatic range value and preserved other-byte edit");
        check(
            app->gate.last_ms == sim_tick && app->auto_wait_since == sim_tick,
            "Listening delay begins after modeled packet finishes, including transmission duration");
        sim_snapshot(
            app,
            delay == 1 ? "Live IR one-second listening delay" :
                         "Live IR two-second listening delay");
        sim_tick += delay * 1000 - 1;
        lab_auto_poll(app);
        check(sim_tx_count == before + n + 1, "No next emission before full configured delay");
        sim_tick += 1;
        lab_auto_poll(app);
        if(n < 2)
            check(
                sim_tx_count == before + n + 2 && !app->auto_paused,
                "Timer sends next range value automatically with no per-step keypress");
    }
    check(
        app->auto_complete && app->auto_paused && sim_tx_count == before + 3,
        "Endpoint stops automatic range without repeating or extra emission");
    check_auto_saved(app, &sim_last_tx, descending ? 0x10 : 0x12);
    sim_snapshot(app, "Live range completed and saved");
    sim_free(app);
}

static void test_auto_pause_and_resume(void) {
    IrLabApp* app = auto_app(1, true);
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    IrLabSignal last = sim_last_tx;
    sim_tick += 1000; // Next value is due at the exact button-press boundary.
    sim_press(app, InputKeyOk, InputTypeShort);
    check(
        app->auto_paused && sim_tx_count == before + 1 && app->auto_last_value == 0x10,
        "Human OK press at due deadline prevents next transmission and freezes last emitted value");
    check_auto_saved(app, &last, 0x10);
    sim_snapshot(app, "Human pause: exact last emitted signal saved");
    char saved_name[32];
    snprintf(saved_name, sizeof(saved_name), "%s", app->auto_saved_name);
    sim_tick += 10000;
    lab_auto_poll(app);
    check(
        sim_tx_count == before + 1 && strcmp(saved_name, app->auto_saved_name) == 0,
        "Paused runner neither transmits nor overwrites saved capture");
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        !app->auto_paused && sim_tx_count == before + 1,
        "Explicit resume starts a new listening delay");
    sim_tick += 999;
    lab_auto_poll(app);
    check(sim_tx_count == before + 1, "Resume waits full delay");
    sim_tick += 1;
    lab_auto_poll(app);
    InfraredMessage expected = {
        .protocol = InfraredProtocolNECext, .address = 0x1235, .command = 0x5611};
    check(
        sim_tx_count == before + 2 && received_frames(&sim_last_tx, expected) == 1,
        "Resume continues with next unsent value, not a repeat of paused signal");
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        !app->range_live && app->screen == LabHex && lab_byte_value(app, true, 0, 2) == 0x11,
        "Back stops, saves, and moves last emitted value into editor");
    sim_free(app);
}

static IrLabApp* auto_hook_app;
static void pause_during_write_hook(void) {
    InputEvent press = {.key = InputKeyOk, .type = InputTypePress};
    lab_input(&press, auto_hook_app);
}
static void pause_during_packet_hook(void) {
    sim_tick += 250;
    pause_during_write_hook();
}
static void fail_after_packet_hook(void) {
    sim_sync_fail = true;
}

static void test_auto_pause_races(void) {
    IrLabApp* app = auto_app(1, true);
    auto_hook_app = app;
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    IrLabSignal emitted = sim_last_tx;
    sim_tick += 1000;
    sim_write_hook = pause_during_write_hook;
    lab_auto_poll(app);
    sim_write_hook = NULL;
    check(
        app->auto_paused && sim_tx_count == before + 1 && app->auto_last_value == 0x10,
        "OK during next-step preparation cancels unsent candidate");
    check_auto_saved(app, &emitted, 0x10);
    sim_free(app);

    app = auto_app(1, true);
    auto_hook_app = app;
    before = sim_tx_count;
    sim_tx_hook = pause_during_packet_hook;
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tx_hook = NULL;
    check(
        app->auto_paused && sim_tx_count == before + 1 && app->auto_has_sent,
        "OK during in-flight packet pauses immediately after current packet completes");
    check_auto_saved(app, &sim_last_tx, 0x10);
    sim_free(app);

    app = auto_app(1, true);
    auto_hook_app = app;
    before = sim_tx_count;
    sim_write_hook = pause_during_write_hook;
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_write_hook = NULL;
    check(
        app->auto_paused && !app->auto_has_sent && !app->auto_save_waveform &&
            sim_tx_count == before,
        "Pause before first emission never saves an unsent signal as an emitted capture");
    sim_free(app);
}

static void test_auto_failures_and_stop(void) {
    IrLabApp* app = auto_app(2, false);
    check(
        app->range_live && app->screen == LabRangeRun && !app->auto_started && !app->stop.valid &&
            !app->rx_running,
        "Live edited range opens directly without STOP capture");
    unsigned before = sim_tx_count;
    sim_sync_fail = true;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        app->auto_paused && app->gate.locked && sim_tx_count == before,
        "SD arming failure blocks all automatic emission");
    sim_free(app);

    app = auto_app(1, true);
    before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_press(app, InputKeyOk, InputTypeShort);
    sim_sync_fail = true;
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        app->gate.locked && app->auto_paused && sim_tx_count == before + 1,
        "Resume logging failure locks runner without advancing or emitting");
    sim_free(app);

    app = auto_app(1, true);
    before = sim_tx_count;
    sim_tx_hook = fail_after_packet_hook;
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tx_hook = NULL;
    check(
        app->auto_paused && app->gate.locked && app->auto_has_sent && !app->auto_save_waveform &&
            sim_tx_count == before + 1,
        "Post-emission SD failure reports last signal as emitted but save failed");
    sim_free(app);

    app = auto_app(1, true);
    before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    IrLabSignal emitted = sim_last_tx;
    sim_press(app, InputKeyBack, InputTypeLong);
    check(
        app->gate.locked && sim_tx_count == before + 2 &&
            memcmp(&sim_last_tx, &app->stop, sizeof(app->stop)) == 0,
        "Emergency stop abandons range and emits saved STOP once");
    check_auto_saved(app, &emitted, 0x10);
    sim_free(app);
}

static void test_long_auto_ranges(void) {
    IrLabApp* app = auto_app(1, false);
    sim_press(app, InputKeyBack, InputTypeShort);
    signal_action(app, 2);
    check(
        focus_rule(app)->from == 0x10 && focus_rule(app)->to == 0x10,
        "New fuzzer starts from selected capture and holds bytes until configured");
    focus_rule(app)->from = 0;
    focus_rule(app)->to = 255;
    focus_rule(app)->step = 1;
    app->gate.trials = 19;
    choose(app, LabRangeStart);
    unsigned before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    for(unsigned value = 0; value < 256; ++value) {
        if(value) {
            sim_tick += 1000;
            lab_auto_poll(app);
        }
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext, .address = 0x1235, .command = 0x5600 | value};
        check(
            sim_tx_count == before + value + 1 && app->auto_last_value == value &&
                received_frames(&sim_last_tx, expected) == 1,
            "Full live range emits and decodes every value past the former session cap");
    }
    sim_tick += 1000;
    lab_auto_poll(app);
    check(
        app->auto_complete && app->auto_paused && app->gate.trials == 275 &&
            app->auto_sent_count == 256,
        "One-pass full byte completes exactly once with no 16/20-value cap");
    check_auto_saved(app, &sim_last_tx, 255);
    sim_snapshot(app, "All 256 live values completed and final signal saved");
    sim_free(app);

    app = auto_app(2, false);
    sim_press(app, InputKeyBack, InputTypeShort);
    signal_action(app, 2);
    focus_rule(app)->from = 0x10;
    focus_rule(app)->to = 0x12;
    focus_rule(app)->step = 1;
    fuzz_options(app, 0);
    check(app->range_repeat, "Repeat-until-paused is explicitly selectable");
    choose(app, LabRangeStart);
    before = sim_tx_count;
    sim_press(app, InputKeyOk, InputTypeLong);
    for(unsigned n = 1; n < 27; ++n) {
        sim_tick += 2000;
        lab_auto_poll(app);
        check(
            sim_tx_count == before + n + 1 && app->auto_last_value == 0x10 + n % 3 &&
                !app->auto_paused,
            "Loop crosses endpoint and continues past 20 emissions at configured delay");
    }
    sim_snapshot(app, "Continuous range running past the previous cap");
    IrLabSignal last = sim_last_tx;
    sim_tick += 2000;
    sim_press(app, InputKeyOk, InputTypeShort);
    check(
        app->auto_paused && !app->auto_complete && sim_tx_count == before + 27,
        "OK at loop endpoint saves last value before wrap without marking range complete");
    check_auto_saved(app, &last, 0x12);
    sim_snapshot(app, "Continuous range paused and retained for investigation");
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1999;
    lab_auto_poll(app);
    check(sim_tx_count == before + 27, "Loop resume waits a fresh complete gap");
    sim_tick += 1;
    lab_auto_poll(app);
    check(
        sim_tx_count == before + 28 && app->auto_last_value == 0x10,
        "Loop resumes with next wrapped value, without replaying paused value");
    // The long-run count must not wrap at the old uint16 boundary.
    app->auto_sent_count = 65535;
    app->gate.trials = 65535;
    sim_tick += 2000;
    lab_auto_poll(app);
    check(
        app->auto_sent_count == 65536 && app->gate.trials == 65536,
        "Long-running counters retain 32-bit totals");
    sim_press(app, InputKeyOk, InputTypeShort);
    check_auto_saved(app, &sim_last_tx, 0x11);
    sim_free(app);
}

static void test_compare_to_fuzzer(void) {
    IrLabApp* app = sim_app();
    const unsigned values[5] = {0x10, 0x14, 0x19, 0x17, 0x17};
    app->capture_batch = true;
    for(unsigned n = 0; n < 5; ++n) {
        InfraredMessage message = {
            .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5600 | values[n]};
        IrLabSignal raw = reference_press(message, 1);
        app->capture_slot = n;
        lab_capture_start(app, false);
        sim_deliver(app, &raw);
    }
    lab_refresh(app);
    check(
        strstr(app->lines[2], "14-10=+04") && strstr(app->lines[3], "19-14=+05") &&
            strstr(app->lines[4], "17-19=-02") && strstr(app->lines[5], "17-17=+00"),
        "Comparison shows all adjacent subtractions including unequal, negative and zero steps");
    sim_snapshot(app, "Compare unequal hex steps with signed subtraction");
    choose(app, 2);
    sim_snapshot(app, "Capture actions: direct send to fuzzer");
    choose(app, 0);
    check(
        app->screen == LabRange && app->active_slot == 2 && app->frame == 0 && app->byte == 2 &&
            focus_rule(app)->from == 0x19 && focus_rule(app)->to == 0x19 &&
            lab_byte_value(app, false, 0, 2) == 0x19,
        "Send-to-fuzzer preserves selected capture, compared frame and byte");
    sim_snapshot(app, "Selected capture opens Start End Steps fuzzer");
    focus_rule(app)->from = 0x19;
    focus_rule(app)->to = 0x1A;
    focus_rule(app)->step = 1;
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    InfraredMessage expected = {
        .protocol = InfraredProtocolNECext, .address = 0x1234, .command = 0x5619};
    check(
        received_frames(&sim_last_tx, expected) == 1,
        "Comparison-to-fuzzer workflow emits selected capture rather than a stale seed");
    sim_press(app, InputKeyOk, InputTypeShort);
    check_auto_saved(app, &sim_last_tx, 0x19);
    check(
        !app->gate.pending_observation,
        "Pause does not require a response form before further comparison");
    sim_press(app, InputKeyBack, InputTypeShort);
    signal_action(app, 2);
    check(
        app->screen == LabRange,
        "Saved paused candidate can immediately return to fuzzer settings");
    // A missing selected frame must never silently fuzz a different frame.
    app->views[4].valid = false;
    lab_open_compare(app);
    choose(app, 4);
    choose(app, 0);
    check(
        app->screen == LabMessage && strstr(app->message[0], "unavailable"),
        "Invalid compared row cannot send a stale or fallback frame to fuzzer");
    app->views[4].valid = true;
    for(unsigned n = 0; n < 5; ++n)
        check(
            ir_lab_view_byte(&app->views[n], 0, 2) == values[n],
            "Fuzzing preserves all comparison originals");
    sim_free(app);
}

static void test_command_pair_sets(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    open_set_from_home(app, 0);
    choose(app, 0);
    IrLabSignal pressure = set_reference(0, 0);
    sim_deliver(app, &pressure);
    uint32_t pressure_id = app->seed_id;
    IrLabSignal optional_stop = set_reference(3, 4);
    app->stop = optional_stop;
    app->stop_id = 777;
    for(unsigned series = 4; series < LAB_SETS; ++series) {
        open_set_from_home(app, series);
        check(
            app->screen == LabSlots && app->menu_count == 3 && lab_set_capacity(series) == 2 &&
                strstr(app->lines[3], "Save profile"),
            "Privacy sound and flush keep two capture slots plus Save profile");
        check(
            strstr(app->lines[1], series == 4 ? "Start" : "Big flush") &&
                strstr(app->lines[2], series == 4 ? "Stop" : "Small flush"),
            "Pair slots have explicit start/stop or big/small labels");
        sim_snapshot(
            app,
            series == 4 ? "Privacy sound: Start and Stop capture slots" :
                          "Flush: Big and Small capture slots");
        sim_press(app, InputKeyDown, InputTypeShort);
        sim_press(app, InputKeyDown, InputTypeShort);
        check(app->row == 2, "Pair list exposes Save profile after the two capture slots");
        sim_press(app, InputKeyDown, InputTypeShort);
        check(app->row == 0, "Pair list wraps after Save profile without a third capture slot");
        for(unsigned slot = 0; slot < 2; ++slot) {
            choose(app, slot);
            check(
                app->screen == LabCapture && app->capture_slot == slot &&
                    strstr(app->lines[0], lab_slot_role(series, slot)),
                "Capture screen names the command being learned");
            IrLabSignal raw = set_reference(series, slot);
            sim_deliver(app, &raw);
            if(slot == 0)
                check(
                    app->screen == LabSlots && app->row == 1 && lab_slot_count(app) == 1,
                    "First pair capture selects the second named slot");
        }
        lab_refresh(app);
        check(
            app->screen == LabCompare && app->menu_count == 2 &&
                app->comparison.comparable_mask == 3 && strstr(app->lines[2], "01-00=+01"),
            "Second pair capture opens two-signal subtraction without unavailable extra rows");
        sim_snapshot(
            app,
            series == 4 ? "Compare privacy Start versus Stop" : "Compare Big versus Small flush");
        check(
            !lab_slot_store(app, 2, &pressure, 123) && !lab_slot_load(app, 2),
            "Two-slot sets reject out-of-range storage and selection");
        check(
            app->stop_id == 777 && memcmp(&app->stop, &optional_stop, sizeof(optional_stop)) == 0,
            "Privacy Stop remains a normal capture and never replaces emergency STOP");
        unsigned selected = series == 4 ? 1 : 0;
        choose(app, selected);
        choose(app, 0);
        check(
            app->screen == LabRange && app->series == series && app->active_slot == selected &&
                app->byte == 2,
            "Named command pair can transfer its selected frame and byte to fuzzer");
        focus_rule(app)->from = focus_rule(app)->to = selected;
        choose(app, LabRangeStart);
        sim_tick += 1000;
        sim_press(app, InputKeyOk, InputTypeLong);
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext,
            .address = 0x1234,
            .command = 0x5000 + series * 0x100 + selected};
        check(
            received_frames(&sim_last_tx, expected) == 1,
            "Fuzzer sends the selected learned pair command through real codec");
        sim_press(app, InputKeyOk, InputTypeShort);
        check_auto_saved(app, &sim_last_tx, selected);
        char path[180], note[2000] = {0}, role[80], name[80];
        snprintf(path, sizeof(path), "%s/%s.json", app->directory, app->auto_saved_name);
        FILE* file = fopen(path, "r");
        assert(file);
        fread(note, 1, sizeof(note) - 1, file);
        fclose(file);
        snprintf(role, sizeof(role), "\"slot_role\": \"%s\"", lab_slot_role(series, selected));
        snprintf(name, sizeof(name), "\"series_name\": \"%s\"", lab_set_names[series]);
        check(
            strstr(note, role) && strstr(note, name),
            "Saved investigation identifies privacy/flush set and command role");
        sim_press(app, InputKeyBack, InputTypeShort);
        open_set_from_home(app, series);
        for(unsigned slot = 0; slot < 2; ++slot) {
            choose(app, slot);
            choose(app, 1);
            sim_tick += 10000;
            if(app->screen == LabMessage) {
                sim_press(app, InputKeyOk, InputTypeShort);
                signal_action(app, 1);
            }
            sim_press(app, InputKeyOk, InputTypeLong);
            IrLabSignal original = set_reference(series, slot);
            check(
                app->screen == LabResult && memcmp(&sim_last_tx, &original, sizeof(original)) == 0,
                "Both named command slots replay their exact original capture");
            choose(app, 0);
            open_set_from_home(app, series);
        }
    }
    open_set_from_home(app, 0);
    check(
        app->slots[0].seed_id == pressure_id &&
            memcmp(&app->seed, &pressure, sizeof(pressure)) == 0,
        "Adding privacy and flush retains an earlier pressure capture");
    for(unsigned series = 4; series < LAB_SETS; ++series) {
        open_set_from_home(app, series);
        check(
            lab_slot_count(app) == 2, "Switching between command pairs preserves both originals");
        uint32_t other_id = app->slots[1].seed_id;
        choose(app, 0);
        choose(app, 2);
        IrLabSignal replacement = set_reference(series, 2);
        sim_deliver(app, &replacement);
        check(
            lab_slot_count(app) == 2 && app->slots[1].seed_id == other_id &&
                ir_lab_view_byte(&app->views[0], 0, 2) == 2,
            "Recapturing one named command retains its opposite command");
    }
    check(
        sim_tx_count == before + 6,
        "Only explicit pair replays and fuzzer starts transmit in host tests");
    sim_free(app);
}

// Configure every byte through the actual menus and nibble editor.
static void edit_rule_ui(IrLabApp* app, unsigned byte_rule, unsigned field, int value) {
    assert(app->screen == LabRange);
    choose(app, LabRangeSteps);
    choose(app, byte_rule);
    choose(app, field);
    assert(app->screen == LabRangeValue);
    unsigned magnitude = value < 0 ? -value : value;
    for(unsigned nibble = 0; nibble < 2; ++nibble) {
        while(app->range_nibble < nibble)
            sim_press(app, InputKeyLeft, InputTypeShort);
        while(app->range_nibble > nibble)
            sim_press(app, InputKeyRight, InputTypeShort);
        for(unsigned attempts = 0;
            ((unsigned)app->range_edit >> (nibble * 4) & 15) != (magnitude >> (nibble * 4) & 15);
            ++attempts) {
            assert(attempts < 16);
            sim_press(app, InputKeyUp, InputTypeShort);
        }
    }
    if(field == 2 && app->range_edit_negative != (value < 0)) {
        sim_press(app, InputKeyLeft, InputTypeShort);
        sim_press(app, InputKeyUp, InputTypeShort);
    }
    sim_press(app, InputKeyOk, InputTypeShort);
    check(app->screen == LabFuzzByte, "Hex editor saves back to the same byte");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    assert(app->screen == LabRange);
}

static IrLabApp* simple_app(void) {
    IrLabApp* app = sim_app();
    InfraredMessage m = {.protocol = InfraredProtocolNECext, .address = 0x1012, .command = 0x20E0};
    IrLabSignal seed = reference_press(m, 1);
    lab_capture_start(app, false);
    sim_deliver(app, &seed);
    // Capture -> OK -> Send to fuzzer (first item).
    sim_press(app, InputKeyOk, InputTypeShort);
    lab_refresh(app);
    check(
        strstr(app->lines[1], "Send to fuzzer"),
        "Send to fuzzer is the first captured-signal action");
    sim_press(app, InputKeyOk, InputTypeShort);
    check(
        app->screen == LabRange && app->rule_count == 4,
        "Capture opens a simple four-byte fuzzer");
    return app;
}

static void test_simple_byte_ui(void) {
    IrLabApp* app = simple_app();
    unsigned before = sim_tx_count;
    lab_refresh(app);
    check(
        strstr(app->lines[1], "Start signal") && strstr(app->lines[2], "End signal") &&
            strstr(app->lines[3], "Byte steps") && strstr(app->lines[4], "Start fuzzing"),
        "Fuzzer shows Start signal, End signal, Byte steps and Start together");
    sim_snapshot(app, "Simple fuzzer: Start / End / Byte steps / Run");
    edit_rule_ui(app, 1, 2, 0x10);
    edit_rule_ui(app, 2, 2, -0x10);
    check(
        app->rules[1].step == 0x10 && app->rules[2].step == -0x10 && app->rules[1].to == 0xFF &&
            app->rules[2].to == 0,
        "Entering +10 and -10 directly opens each byte's remaining range");
    edit_rule_ui(app, 1, 1, 0x40);
    edit_rule_ui(app, 2, 1, 0xB0);
    choose(app, LabRangeSteps);
    lab_refresh(app);
    check(
        strstr(app->lines[2], "Byte 2 10>40 +10") && strstr(app->lines[3], "Byte 3 E0>B0 -10"),
        "One list displays both opposite byte steps in hex");
    sim_snapshot(app, "Byte steps: Byte 2 +10 and Byte 3 -10");
    choose(app, 2);
    sim_snapshot(app, "Byte editor: Start End Step Done");
    choose(app, 2);
    sim_press(app, InputKeyLeft, InputTypeShort);
    sim_press(app, InputKeyLeft, InputTypeShort);
    sim_snapshot(app, "Signed hex editor: minus sign selected");
    sim_press(app, InputKeyUp, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(app->rules[2].step == -0x10, "Back cancels sign editing without changing the rule");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        sim_tx_count == before && lab_range_count(app) == 4,
        "Configuration transmits nothing and computes four packets");
    choose(app, LabRangeDelay);
    choose(app, LabRangeStart);
    sim_snapshot(app, "Simple fuzzer ready to send");
    sim_press(app, InputKeyOk, InputTypeLong);
    for(unsigned n = 0; n < 4; ++n) {
        if(n) {
            sim_tick += 2000;
            lab_auto_poll(app);
        }
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext,
            .address = ((0x10 + n * 0x10) << 8) | 0x12,
            .command = 0x2000 | (0xE0 - n * 0x10)};
        check(
            received_frames(&sim_last_tx, expected) == 1 && sim_tx_count == before + n + 1,
            "Real decoder verifies byte 2 +10, byte 3 -10, with other bytes fixed");
        sim_snapshot(app, "Simple opposite-byte live packet and listening delay");
    }
    sim_tick += 2000;
    lab_auto_poll(app);
    check(
        app->auto_complete && app->auto_sent_count == 4,
        "Both byte ranges stop at configured endpoints");
    check_auto_saved(app, &sim_last_tx, 0x40);
    char path[180], note[4096] = {0};
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, app->auto_saved_name);
    FILE* file = fopen(path, "r");
    assert(file);
    fread(note, 1, sizeof(note) - 1, file);
    fclose(file);
    check(
        strstr(note, "\"step_hex\":\"+10\"") && strstr(note, "\"step_hex\":\"-10\"") &&
            strstr(note, "\"value_hex\":\"40\"") && strstr(note, "\"value_hex\":\"B0\""),
        "Snapshot records every signed byte rule and both actual emitted values");
    sim_snapshot(app, "Simple fuzzer completed and exact last packet saved");
    sim_free(app);
}

static void test_fuzz_end_captures(void) {
    IrLabApp* app = sim_app();
    const InfraredMessage messages[] = {
        {.protocol = InfraredProtocolNECext, .address = 0x1012, .command = 0x20E0},
        {.protocol = InfraredProtocolNECext, .address = 0x4012, .command = 0x20B0},
        {.protocol = InfraredProtocolSIRC, .address = 7, .command = 0x10}};
    app->capture_batch = true;
    for(unsigned n = 0; n < 3; ++n) {
        app->capture_slot = n;
        lab_capture_start(app, false);
        IrLabSignal raw = reference_press(messages[n], 1);
        sim_deliver(app, &raw);
    }
    lab_slot_load(app, 0);
    lab_go(app, LabHex, 0);
    signal_action(app, 2);
    choose(app, LabRangeTo);
    sim_snapshot(app, "Choose End signal from captures or manual hex");
    choose(app, 2);
    check(
        app->screen == LabRange && app->end_seed_id == app->slots[1].seed_id &&
            app->seed_id == app->slots[0].seed_id && app->rules[1].to == 0x40 &&
            app->rules[2].to == 0xB0 && app->rules[1].step == 1 && app->rules[2].step == -1 &&
            app->rules[0].step == 0,
        "End capture sets all endpoints and directions without replacing Start");
    edit_rule_ui(app, 1, 2, 0x10);
    edit_rule_ui(app, 2, 2, -0x10);
    check(
        app->end_seed_id == app->slots[1].seed_id && lab_range_count(app) == 4,
        "Step-only edit preserves the selected End capture");
    IrLabByteRule saved[IR_LAB_MAX_BYTE_RULES];
    memcpy(saved, app->rules, sizeof(saved));
    uint32_t seed_id = app->seed_id;
    choose(app, LabRangeTo);
    choose(app, 3);
    check(
        app->screen == LabMessage && memcmp(saved, app->rules, sizeof(saved)) == 0 &&
            app->seed_id == seed_id,
        "Mismatched end layout keeps the complete previous Start and byte rules");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_sd_fail = true;
    choose(app, 2);
    sim_sd_fail = false;
    check(
        app->screen == LabMessage && memcmp(saved, app->rules, sizeof(saved)) == 0,
        "Unreadable End capture leaves all prior byte settings intact");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabRangeFrom);
    choose(app, 2);
    check(
        app->screen == LabRange && app->seed_id == app->slots[1].seed_id &&
            app->rules[1].from == 0x40 && app->rules[2].from == 0xB0 && app->rules[1].step == 0 &&
            app->end_seed_id == 0,
        "Selecting a new Start capture resets endpoints and steps to that signal");
    sim_free(app);
}

static void test_many_byte_rules(void) {
    IrLabApp* app = simple_app();
    edit_rule_ui(app, 0, 2, 1);
    edit_rule_ui(app, 1, 2, 0x10);
    edit_rule_ui(app, 2, 2, -0x10);
    edit_rule_ui(app, 3, 2, 2);
    edit_rule_ui(app, 0, 1, 0x15);
    edit_rule_ui(app, 1, 1, 0x40);
    edit_rule_ui(app, 2, 1, 0xB0);
    edit_rule_ui(app, 3, 1, 0x26);
    check(lab_range_count(app) == 4, "All four bytes can have independent signed magnitudes");
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    for(unsigned n = 0; n < 4; ++n) {
        if(n) {
            sim_tick += 1000;
            lab_auto_poll(app);
        }
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext,
            .address = ((0x10 + n * 0x10) << 8) | (0x12 + n),
            .command = ((0x20 + n * 2) << 8) | (0xE0 - n * 0x10)};
        check(
            received_frames(&sim_last_tx, expected) == 1,
            "Whole packet advances four distinct byte rules atomically");
    }
    sim_press(app, InputKeyOk, InputTypeShort);
    check_auto_saved(app, &sim_last_tx, 0x15);
    sim_free(app);

    app = simple_app();
    edit_rule_ui(app, 1, 1, 0x40);
    edit_rule_ui(app, 1, 2, -0x10);
    unsigned before = sim_tx_count;
    choose(app, LabRangeStart);
    check(
        app->screen == LabMessage && strstr(app->message[0], "Check byte step") &&
            sim_tx_count == before,
        "Step pointing away from End names the offending byte without transmission");
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        app->screen == LabFuzzByte && app->rule_selected == 1,
        "Invalid direction returns directly to the offending byte");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    edit_rule_ui(app, 1, 2, 0);
    check(
        app->rules[1].from == app->rules[1].to && app->rules[1].step == 0,
        "Step 00 explicitly holds the byte at Start");
    sim_free(app);

    app = sim_app();
    app->carrier = 40000;
    InfraredMessage m = {.protocol = InfraredProtocolSIRC, .address = 7, .command = 0x10};
    IrLabSignal raw = reference_press(m, 1);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    signal_action(app, 2);
    check(
        app->rule_count == 2 && ir_lab_byte_width(&app->bits, 0, 1) == 4,
        "Partial data byte appears once with its real width");
    edit_rule_ui(app, 1, 2, -1);
    edit_rule_ui(app, 1, 1, 0);
    unsigned count = lab_range_count(app);
    check(count > 1 && count <= 16, "Partial byte never gains unobserved padding bits");
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    for(unsigned n = 1; n <= count; ++n) {
        sim_tick += 1000;
        lab_auto_poll(app);
    }
    check(
        app->auto_complete && app->auto_sent_count == count &&
            lab_byte_value(app, true, 0, 1) == 0,
        "Partial-byte runner reaches zero without wrapping");
    sim_free(app);

    app = sim_app();
    m = (InfraredMessage){
        .protocol = InfraredProtocolNECext, .address = 0x1012, .command = 0x20E0};
    raw = reference_press(m, 1);
    IrLabSignal repeat = raw;
    raw.timings[raw.count++] = 40000;
    memcpy(raw.timings + raw.count, repeat.timings, repeat.count * sizeof(uint32_t));
    raw.count += repeat.count;
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    signal_action(app, 2);
    check(
        app->bits.frames == 2 && app->rule_count == 4,
        "Identical repeated frames share four byte rules automatically");
    edit_rule_ui(app, 1, 2, 0x10);
    edit_rule_ui(app, 2, 2, -0x10);
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1000;
    lab_auto_poll(app);
    InfraredMessage expected = {
        .protocol = InfraredProtocolNECext, .address = 0x2012, .command = 0x20D0};
    check(
        received_frames(&sim_last_tx, expected) == 2,
        "Both repeated frames carry the same synchronized byte changes");
    sim_press(app, InputKeyOk, InputTypeShort);
    sim_free(app);
}

static void test_simple_preview_manual(void) {
    for(unsigned delay = 1; delay <= 2; ++delay) {
        IrLabApp* app = simple_app();
        edit_rule_ui(app, 1, 2, 0x10);
        edit_rule_ui(app, 2, 2, -0x10);
        if(delay == 2) choose(app, LabRangeDelay);
        unsigned before = sim_tx_count;
        fuzz_options(app, 1);
        sim_tick += delay * 1000 - 1;
        lab_preview_poll(app);
        check(app->range_index == 0, "Preview waits the full configured delay");
        ++sim_tick;
        lab_preview_poll(app);
        check(
            app->range_index == 1 && lab_byte_value(app, true, 0, 1) == 0x20 &&
                lab_byte_value(app, true, 0, 2) == 0xD0,
            "Preview advances all signed byte rules together");
        sim_press(app, InputKeyOk, InputTypeShort);
        check(
            app->preview_paused && sim_tx_count == before,
            "Preview OK freezes the candidate with no physical IR");
        sim_snapshot(app, "Optional byte-step preview (NO IR)");
        sim_press(app, InputKeyBack, InputTypeShort);
        fuzz_options(app, 2);
        sim_press(app, InputKeyOk, InputTypeLong);
        choose(app, 0);
        sim_tick += 10000;
        sim_press(app, InputKeyOk, InputTypeLong);
        InfraredMessage expected = {
            .protocol = InfraredProtocolNECext, .address = 0x2012, .command = 0x20D0};
        check(
            sim_tx_count == before + 2 && received_frames(&sim_last_tx, expected) == 1,
            "Optional manual mode sends the same synchronized second packet after cooldown");
        sim_free(app);
    }
}

static IrLabSignal protocol_toto_signal(unsigned first, unsigned second, unsigned frames) {
    IrLabSignal s = {.valid = true, .frequency = 38000, .duty_cycle = .33f};
    uint8_t data[3] = {first, second, first ^ second};
    for(unsigned f = 0; f < frames; ++f) {
        s.timings[s.count++] = 6197;
        s.timings[s.count++] = 2754;
        for(unsigned b = 0; b < 39; ++b) {
            bool bit = b < 15 ? (0x0802 >> b) & 1 : (data[(b - 15) / 8] >> ((b - 15) % 8)) & 1;
            s.timings[s.count++] = 600;
            s.timings[s.count++] = bit ? 1634 : 516;
        }
        s.timings[s.count++] = 600;
        if(f + 1 < frames) s.timings[s.count++] = 38000;
    }
    return s;
}

static void test_protocol_ui(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    app->capture_batch = true;
    for(unsigned n = 0; n < 4; ++n) {
        app->capture_slot = n;
        lab_capture_start(app, false);
        IrLabSignal raw = protocol_toto_signal(0x10 + (n % 3) * 0x10, 0xE0 - (n % 3) * 0x10, 2);
        sim_deliver(app, &raw);
    }
    assert(lab_slot_load(app, 0));
    lab_go(app, LabHex, 0);
    sim_press(app, InputKeyOk, InputTypeShort);
    choose(app, 1);
    check(
        app->screen == LabProtocol && app->protocol_analysis.kind == IrLabProtocolToto &&
            app->protocol_analysis.payload_offset == 15,
        "Analyze protocol identifies TOTO 15-bit prefix and 24-bit data layout");
    check(
        app->protocol_analysis.checks_passed == 2 && app->protocol_analysis.unique_frames == 1,
        "Protocol report checks all frames and counts identical repeats");
    sim_snapshot(app, "Analyze captured TOTO: 15 prefix + 24 payload");
    sim_press(app, InputKeyOk, InputTypeShort);
    choose(app, 1);
    sim_snapshot(app, "Protocol timing model in microseconds");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 2);
    sim_snapshot(app, "Published TOTO XOR check passes all frames");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 4);
    check(
        app->screen == LabMessage && strstr(app->message[1], "0 bits"),
        "Known saved capture matching finds identical data in another slot");
    sim_snapshot(app, "Passive match against other saved captures");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 5);
    check(
        app->screen == LabMessage && strstr(app->message[0], "saved") && sim_tx_count == before,
        "Protocol analysis saves JSON without emitting IR");
    sim_press(app, InputKeyBack, InputTypeShort);
    // Three controlled levels, excluding the duplicate control from the linear sequence.
    app->protocol_view_count = 3;
    choose(app, 3);
    sim_snapshot(app, "Protocol fields: inferred opposite signed steps");
    choose(app, 0);
    sim_snapshot(app, "Observed byte relation and step with unverified semantics");
    check(
        strstr(app->lines[2], "+10") && strstr(app->lines[3], "F0"),
        "Field analysis shows signed step and constant-sum pair candidate");
    sim_press(app, InputKeyOk, InputTypeShort);
    check(
        app->screen == LabRange && app->fuzz_offset == 15 && app->fuzz_keep_check &&
            app->rule_count == 3 && app->rules[0].step == 0x10 && app->rules[1].step == -0x10,
        "Use observed steps opens payload-aligned fuzzer with two opposite rules and XOR kept");
    choose(app, LabRangeSteps);
    sim_snapshot(app, "Aligned payload byte steps with automatic XOR byte");
    choose(app, 2);
    check(app->screen == LabMessage, "Automatic check byte cannot silently become a manual rule");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1000;
    lab_auto_poll(app);
    IrLabBits sent;
    IrLabProtocolAnalysis a;
    check(
        sim_tx_count == before + 2 && ir_lab_extract_bits(&sim_last_tx, &sent) &&
            ir_lab_protocol_analyze(&sim_last_tx, &sent, &a) && a.kind == IrLabProtocolToto &&
            a.checks_passed == 2 && ir_lab_window_value(&sent, NULL, 0, 15, 0) == 0x20 &&
            ir_lab_window_value(&sent, NULL, 0, 15, 1) == 0xD0,
        "Live payload fuzzer emits both aligned edits with valid XOR in every repeat");
    for(unsigned f = 0; f < sent.frames; ++f)
        for(unsigned b = 0; b < 15; ++b)
            assert(
                ir_lab_bit_get(sent.data, sent.start[f] + b) ==
                ir_lab_bit_get(app->bits.data, app->bits.start[f] + b));
    sim_press(app, InputKeyOk, InputTypeShort);
    check_auto_saved(app, &sim_last_tx, 0x20);
    check(
        app->mutation.index == 15 && app->mutation.width == 8,
        "Pause metadata retains payload bit offset instead of raw byte boundary");
    sim_snapshot(app, "Protocol-guided fuzzer paused: aligned candidate saved");
    char path[180], note[3000] = {0};
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, app->auto_saved_name);
    FILE* file = fopen(path, "r");
    assert(file);
    fread(note, 1, sizeof(note) - 1, file);
    fclose(file);
    check(
        strstr(note, "\"payload_offset_bits\":15") && strstr(note, "TOTO XOR8") &&
            strstr(note, "\"derived\":true"),
        "Saved fuzzer note identifies alignment and derived checksum bytes");
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        app->screen == LabHex && app->byte == 1,
        "Leaving payload run returns to the correct original raw byte view");
    sim_free(app);
}

static void test_protocol_prefix_and_failure(void) {
    IrLabApp* app = sim_app();
    IrLabSignal raw = protocol_toto_signal(0x10, 0xE0, 2);
    lab_capture_start(app, false);
    sim_deliver(app, &raw);
    ir_lab_toggle_linked(&app->bits, app->changes, 0);
    lab_open_fuzzer_mode(app, 15);
    check(
        !ir_lab_bit_get(app->changes, 0) && !ir_lab_bit_get(app->changes, app->bits.start[1]),
        "Using payload layout restores captured prefix after earlier raw edits");
    // An invalid prepared prefix must leave the previous mask intact and never emit.
    ir_lab_toggle_linked(&app->bits, app->changes, 0);
    uint8_t saved[IR_LAB_MASK_BYTES];
    memcpy(saved, app->changes, sizeof(saved));
    unsigned before = sim_tx_count;
    choose(app, LabRangeStart);
    check(
        app->screen == LabMessage && !app->range_live && sim_tx_count == before &&
            !memcmp(saved, app->changes, sizeof(saved)),
        "Failed integrity preparation does not commit a partial candidate or start transmission");
    lab_open_fuzzer_mode(app, 15);
    fuzz_options(app, 3);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(!app->fuzz_keep_check, "Automatic TOTO XOR can be explicitly disabled");
    edit_rule_ui(app, 2, 2, 1);
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1000;
    lab_auto_poll(app);
    IrLabBits bits;
    IrLabProtocolAnalysis a;
    check(
        ir_lab_extract_bits(&sim_last_tx, &bits) &&
            ir_lab_protocol_analyze(&sim_last_tx, &bits, &a) && a.checks_passed == 0,
        "Manual integrity mode preserves explicit checksum mutation instead of silently repairing it");
    sim_press(app, InputKeyOk, InputTypeShort);
    sim_free(app);
}

static IrLabSignal protocol_inax_signal(unsigned first, unsigned command, unsigned frames) {
    IrLabSignal s = {.valid = true, .frequency = 38000, .duty_cycle = .33f};
    uint8_t data[] = {first, command, command ^ 255};
    for(unsigned f = 0; f < frames; ++f) {
        s.timings[s.count++] = 9000;
        s.timings[s.count++] = 4500;
        for(unsigned b = 0; b < 24; ++b) {
            s.timings[s.count++] = 560;
            s.timings[s.count++] = (data[b / 8] >> (b % 8)) & 1 ? 1675 : 560;
        }
        s.timings[s.count++] = 560;
        if(f + 1 < frames) s.timings[s.count++] = 40000;
    }
    return s;
}

static void test_fuzzer_presets(void) {
    IrLabApp* app = sim_app();
    unsigned before = sim_tx_count;
    IrLabSignal raw = protocol_inax_signal(0x3A, 0x10, 2);
    app->capture_batch = true;
    for(unsigned n = 0; n < 3; ++n) {
        app->capture_slot = n;
        lab_capture_start(app, false);
        IrLabSignal capture = protocol_inax_signal(n == 2 ? 0x4A : 0x3A, 0x10 + n * 0x10, 2);
        sim_deliver(app, &capture);
    }
    assert(lab_slot_load(app, 0));
    lab_open_fuzzer(app);
    choose(app, LabRangePreset);
    check(app->screen == LabFuzzPresets, "Preset picker is one action from the fuzzer");
    sim_snapshot(app, "Simple preset picker: captured bytes, TOTO, INAX");
    choose(app, 1);
    check(
        app->screen == LabMessage && app->fuzz_preset == IrLabProtocolUnknown &&
            app->rule_count == 3 && sim_tx_count == before,
        "Incompatible TOTO preset preserves the INAX seed and existing rules without TX");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 2);
    check(
        app->screen == LabRange && app->fuzz_preset == IrLabProtocolInax && app->fuzz_keep_check &&
            app->rule_count == 2 && app->rules[0].byte == 1,
        "INAX preset fixes first byte and exposes command plus automatic inverse");
    sim_snapshot(app, "INAX preset in the existing fuzzer");
    choose(app, LabRangeTo);
    choose(app, 3);
    check(
        app->screen == LabMessage && !app->end_seed_id,
        "INAX End capture with different fixed first byte is rejected");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 2);
    check(
        app->screen == LabRange && app->rules[0].to == 0x20 && app->rules[0].step == 1 &&
            !app->rules[1].step,
        "INAX matching End capture configures only the independent command range");
    edit_rule_ui(app, 0, 2, 0x10);
    choose(app, LabRangeSteps);
    sim_snapshot(app, "INAX byte steps with automatic inverse");
    choose(app, 1);
    check(app->screen == LabMessage, "INAX automatic inverse cannot be edited while enabled");
    sim_press(app, InputKeyBack, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1000;
    lab_auto_poll(app);
    IrLabBits sent;
    IrLabProtocolAnalysis analysis;
    check(
        sim_tx_count == before + 2 && ir_lab_extract_bits(&sim_last_tx, &sent) &&
            ir_lab_protocol_analyze(&sim_last_tx, &sent, &analysis) &&
            analysis.kind == IrLabProtocolInax && analysis.checks_passed == 2 &&
            ir_lab_byte_value(&sent, NULL, 0, 0) == 0x3A &&
            ir_lab_byte_value(&sent, NULL, 0, 1) == 0x20,
        "INAX preset emits the selected command and repaired inverse in every repeat");
    for(unsigned t = 0; t < raw.count; ++t) {
        bool data_space = false;
        for(unsigned b = 0; b < app->bits.count; ++b)
            data_space |= app->bits.timing[b] == t;
        if(!data_space) assert(raw.timings[t] == sim_last_tx.timings[t]);
    }
    sim_press(app, InputKeyOk, InputTypeShort);
    check_auto_saved(app, &sim_last_tx, 0x20);
    char path[180], note[3000] = {0};
    snprintf(path, sizeof(path), "%s/%s.json", app->directory, app->auto_saved_name);
    FILE* file = fopen(path, "r");
    assert(file);
    fread(note, 1, sizeof(note) - 1, file);
    fclose(file);
    check(
        strstr(note, "\"preset\":\"INAX\"") && strstr(note, "INAX inverse8") &&
            strstr(note, "\"derived\":true"),
        "Saved INAX note records preset and automatic inverse");
    sim_press(app, InputKeyBack, InputTypeShort);
    lab_go(app, LabResult, 0);
    choose(app, 0);
    lab_open_fuzzer_preset(app, IrLabProtocolInax);
    fuzz_options(app, 3);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(!app->fuzz_keep_check, "INAX inverse repair can be explicitly disabled");
    edit_rule_ui(app, 1, 2, 1);
    sim_tick += 1000; // Complete the gap from the preceding paused transmission.
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    sim_tick += 1000;
    lab_auto_poll(app);
    check(
        ir_lab_extract_bits(&sim_last_tx, &sent) &&
            ir_lab_protocol_analyze(&sim_last_tx, &sent, &analysis) && !analysis.checks_passed,
        "INAX manual check mutation remains available when repair is off");
    sim_press(app, InputKeyOk, InputTypeShort);
    sim_press(app, InputKeyBack, InputTypeShort);
    lab_open_fuzzer_preset(app, IrLabProtocolInax);
    choose(app, LabRangePreset);
    choose(app, 0);
    check(
        app->rule_count == 3 && !app->fuzz_keep_check && !app->fuzz_offset &&
            app->fuzz_preset == IrLabProtocolUnknown,
        "Captured bytes preset restores the full manual byte editor");
    // A 24-bit timing match alone does not establish an inverse-pair invariant.
    assert(lab_slot_load(app, 0));
    ir_lab_toggle_linked(&app->bits, app->changes, 16);
    assert(lab_candidate(app));
    IrLabSignal bad = app->candidate;
    lab_capture_start(app, false);
    sim_deliver(app, &bad);
    lab_open_fuzzer_preset(app, IrLabProtocolInax);
    check(!app->fuzz_keep_check, "INAX framing preset does not invent a missing inverse relation");
    fuzz_options(app, 3);
    check(
        app->screen == LabMessage && !app->fuzz_keep_check,
        "Inverse repair cannot be enabled when it fails on the original capture");
    // Direct TOTO selection follows the same small preset picker.
    IrLabSignal toto = protocol_toto_signal(0x10, 0xE0, 2);
    lab_capture_start(app, false);
    sim_deliver(app, &toto);
    lab_open_fuzzer(app);
    before = sim_tx_count;
    choose(app, LabRangePreset);
    choose(app, 1);
    check(
        app->fuzz_preset == IrLabProtocolToto && app->fuzz_offset == 15 && app->fuzz_keep_check &&
            app->rule_count == 3 && sim_tx_count == before,
        "TOTO preset selects payload alignment and XOR without entering analysis or transmitting");
    sim_snapshot(app, "TOTO preset in the existing fuzzer");
    sim_free(app);
}

static unsigned profile_row(IrLabApp* app, unsigned id) {
    for(unsigned n = 0; n < app->profile_count; ++n)
        if(app->profile_ids[n] == id) return n;
    assert(false);
    return 0;
}

static void test_sniff_profiles(void) {
    IrLabApp* app = sim_app();
    IrLabSignal oshiri = protocol_inax_signal(0x3A, 0xE7, 2);
    IrLabSignal massage = protocol_inax_signal(0x3A, 0x81, 2);
    unsigned before = sim_tx_count;
    lab_go(app, LabHome, 0);
    choose(app, LabHomeSniff);
    check(
        app->screen == LabSniff && !app->profile.count, "Sniff mode opens a new labeled profile");
    choose(app, 0);
    sim_snapshot(app, "Sniff mode: capture one remote button");
    sim_deliver(app, &oshiri);
    check(
        app->screen == LabSniffLabel && app->sniff_guess == IrLabProtocolInax && !app->seed.valid,
        "Sniffing guesses INAX without replacing the current seed or transmitting");
    sim_snapshot(app, "Sniff mode: what is this with protocol guess");
    choose(app, IrLabActionRearWash);
    check(
        app->screen == LabSniff && app->profile.count == 1 && app->profile.id &&
            app->profile.entries[0].action == IrLabActionRearWash,
        "Choosing Oshiri labels and saves the whole raw capture on SD");
    unsigned id = app->profile.id;
    choose(app, 0);
    sim_deliver(app, &massage);
    choose(app, IrLabActionMassage);
    check(
        app->profile.count == 2 && app->profile.entries[1].action == IrLabActionMassage &&
            app->profile.entries[1].protocol == IrLabProtocolInax && sim_tx_count == before,
        "Massage is a first-class capture label; sniffing and saving never emit IR");
    sim_snapshot(app, "Sniff profile saved: capture next or browse signals");
    choose(app, 0);
    sim_tick += 15000;
    lab_capture_poll(app);
    check(
        app->screen == LabMessage && app->message_return == LabSniff && app->profile.count == 2,
        "Sniff timeout preserves saved entries and returns to sniff mode");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 0);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        app->screen == LabSniff && !app->rx_running,
        "Back cancels sniff reception without losing the profile");
    choose(app, 0);
    sim_deliver(app, &massage);
    sim_press(app, InputKeyBack, InputTypeShort);
    check(app->profile.count == 2, "Canceling a label does not save an unlabeled capture");
    choose(app, 0);
    sim_deliver(app, &massage);
    sim_sync_fail = true;
    choose(app, IrLabActionMusicStart);
    check(
        app->screen == LabMessage && app->profile.count == 2 && app->received.valid,
        "Failed profile sync retains the pending capture and all earlier entries");
    sim_sync_fail = false;
    IrLabProfile recovered;
    check(
        ir_lab_profile_load(app->storage, LAB_PROFILES, id, &recovered) && recovered.count == 2,
        "A fresh index recovers only committed signals after a failed write");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, IrLabActionMusicStart);
    check(
        app->profile.count == 3 && app->screen == LabSniff,
        "The same labeled capture can be retried after SD failure");
    choose(app, 2);
    check(app->screen == LabProfileMaker, "Save and close asks for the maker before finishing");
    sim_snapshot(app, "Save profile: choose the maker");
    choose(app, IrLabMakerLixil);
    check(
        app->screen == LabHome && !app->profile_open, "Save and close finishes the sniff profile");
    sim_free(app);

    app = sim_app();
    IrLabSignal bank_original = protocol_toto_signal(0x10, 0xE0, 2);
    lab_capture_start(app, false);
    sim_deliver(app, &bank_original);
    lab_go(app, LabHome, 0);
    choose(app, LabHomeProfiles);
    check(app->screen == LabProfiles, "Load profile finds saved profiles in a new app session");
    choose(app, profile_row(app, id));
    check(
        app->screen == LabProfileSignals && app->profile.count == 3 &&
            app->profile.entries[1].action == IrLabActionMassage &&
            app->profile.maker == IrLabMakerLixil,
        "Restart recovery restores labels, order and protocol candidates");
    sim_snapshot(app, "Loaded profile: named remote actions after restart");
    choose(app, 1);
    check(
        app->screen == LabProfileSignal && !memcmp(&app->seed, &massage, sizeof(massage)),
        "Selecting saved Massage restores exact original timing, repeats and carrier metadata");
    IrLabSignal bank_check;
    check(
        lab_slot_read(app, &app->slots[0], &bank_check) &&
            !memcmp(&bank_check, &bank_original, sizeof(bank_original)),
        "Loading profiles preserves existing comparison-bank captures");
    sim_snapshot(app, "Saved Massage: fuzzer, analyze, replay or view hex");
    choose(app, 1);
    check(
        app->screen == LabProtocol && app->protocol_analysis.kind == IrLabProtocolInax &&
            app->protocol_analysis.checks_passed == 2,
        "Loaded profile signals use the normal protocol analyzer");
    lab_go(app, LabProfileSignals, 1);
    choose(app, 1);
    choose(app, 2);
    check(
        app->screen == LabSend && sim_tx_count == before,
        "Loaded original replay still waits for hold OK");
    sim_press(app, InputKeyOk, InputTypeLong);
    check(
        sim_tx_count == before + 1 && !memcmp(&sim_last_tx, &massage, sizeof(massage)),
        "Replay emits the exact saved Massage waveform once");
    choose(app, 0);
    lab_go(app, LabProfileSignals, 0);
    choose(app, 0);
    choose(app, 0);
    check(
        app->screen == LabRange && app->fuzz_preset == IrLabProtocolInax && app->fuzz_keep_check,
        "Sending a loaded Oshiri capture to the fuzzer selects its supported preset");
    unsigned tx = sim_tx_count;
    fuzz_options(app, 4);
    check(
        app->screen == LabRange && app->rules[0].from == 0x18 && app->rules[1].from == 0xE7 &&
            sim_tx_count == tx,
        "Invert command swaps E7/18 while retaining 3A and never sends automatically");
    fuzz_options(app, 4);
    check(
        app->rules[0].from == 0xE7 && app->rules[1].from == 0x18,
        "Inverting twice restores the original command plan");
    edit_rule_ui(app, 0, 1, 0xF7);
    edit_rule_ui(app, 0, 2, 0x10);
    fuzz_options(app, 4);
    check(
        app->rules[0].from == 0x18 && app->rules[0].to == 0x08 && app->rules[0].step == -0x10 &&
            lab_range_count(app) == 2,
        "Complementing a command range reverses its signed step and preserves candidate count");
    sim_snapshot(app, "Invert command: complemented range in the existing editor");
    sim_tick += 10000;
    choose(app, LabRangeStart);
    sim_press(app, InputKeyOk, InputTypeLong);
    IrLabBits emitted;
    check(
        ir_lab_extract_bits(&sim_last_tx, &emitted) &&
            ir_lab_byte_value(&emitted, NULL, 0, 0) == 0x3A &&
            ir_lab_byte_value(&emitted, NULL, 0, 1) == 0x18 &&
            ir_lab_byte_value(&emitted, NULL, 0, 2) == 0xE7,
        "Explicit start emits the inverted command and correct inverse pair in every repeat");
    sim_press(app, InputKeyOk, InputTypeShort);
    IrLabSignal original;
    check(
        ir_lab_profile_read(app->storage, LAB_PROFILES, &app->profile, 0, &original) &&
            !memcmp(&original, &oshiri, sizeof(oshiri)),
        "Fuzzing and inversion leave the reusable profile original unchanged");
    char evidence[180], note[4000] = {0};
    snprintf(evidence, sizeof(evidence), "%s/%s.json", app->directory, app->auto_saved_name);
    FILE* saved_note = fopen(evidence, "r");
    assert(saved_note);
    fread(note, 1, sizeof(note) - 1, saved_note);
    fclose(saved_note);
    check(
        strstr(note, "\"profile_action\":\"Oshiri / rear wash\"") &&
            strstr(note, "\"profile_signal\":1") &&
            strstr(note, "\"profile_maker\":\"LIXIL / INAX\""),
        "Paused fuzz evidence retains the source profile and original action label");

    // Reject a damaged entry atomically, with the last successfully loaded profile intact.
    char path[180];
    snprintf(path, sizeof(path), "%s/profile-%03u/signal-002.ir", LAB_PROFILES, id);
    FILE* damaged = fopen(path, "w");
    assert(damaged);
    fputs("Filetype: IR signals file\nVersion: 1\nprofile_version: 1\naction_id: 999\n", damaged);
    fclose(damaged);
    IrLabProfile previous = app->profile;
    check(
        !ir_lab_profile_load(app->storage, LAB_PROFILES, id, &app->profile) &&
            !memcmp(&previous, &app->profile, sizeof(previous)),
        "Damaged profile labels cannot partially overwrite the active profile");
    for(unsigned index = 1; index <= 3; ++index) {
        snprintf(path, sizeof(path), "%s/profile-%03u/signal-%03u.ir", LAB_PROFILES, id, index);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/profile-%03u/profile.meta", LAB_PROFILES, id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/profile-%03u", LAB_PROFILES, id);
    rmdir(path);
    sim_free(app);
}

static void test_profile_storage_limits(void) {
    IrLabApp* app = sim_app();
    char root[160];
    snprintf(root, sizeof(root), "%s/profile-tests", app->directory);
    IrLabProfile profile, loaded;
    check(
        ir_lab_profile_create(app->storage, root, &profile),
        "Isolated profile storage initializes");
    IrLabSignal raw = {
        .valid = true, .frequency = 38000, .duty_cycle = .33f, .count = IR_LAB_MAX_TIMINGS};
    for(unsigned n = 0; n < raw.count; ++n)
        raw.timings[n] = 100;
    sim_rename_fail = true;
    check(
        !ir_lab_profile_append(app->storage, root, &profile, &raw, IrLabActionMassage) &&
            !profile.count,
        "Failed publication never exposes a partly saved profile entry");
    sim_rename_fail = false;
    for(unsigned n = 0; n < IR_LAB_PROFILE_SIGNALS; ++n)
        assert(ir_lab_profile_append(app->storage, root, &profile, &raw, n % IrLabActionCount));
    check(
        profile.count == IR_LAB_PROFILE_SIGNALS &&
            !ir_lab_profile_append(app->storage, root, &profile, &raw, IrLabActionOther),
        "Profile stores 64 maximum-sized signals and rejects overflow without overwriting");
    check(
        ir_lab_profile_load(app->storage, root, profile.id, &loaded) && loaded.count == 64,
        "Full profiles reload without retaining waveforms in RAM");
    IrLabSignal copy;
    check(
        ir_lab_profile_read(app->storage, root, &loaded, 63, &copy) &&
            !memcmp(&copy, &raw, sizeof(raw)),
        "A maximum-sized unsupported waveform remains exact for original replay");
    sim_sd_fail = true;
    IrLabProfile previous = loaded;
    check(
        !ir_lab_profile_load(app->storage, root, profile.id, &loaded) &&
            !memcmp(&loaded, &previous, sizeof(loaded)),
        "SD read failure leaves the previous profile index intact");
    sim_sd_fail = false;
    char path[240];
    snprintf(path, sizeof(path), "%s/profile-%03u/signal-032.ir", root, profile.id);
    unlink(path);
    check(
        !ir_lab_profile_load(app->storage, root, profile.id, &loaded),
        "A missing middle capture is rejected instead of silently hiding later signals");
    sim_free(app);
}

static void test_profile_makers_and_duplicates(void) {
    IrLabApp* app = sim_app();
    unsigned tx = sim_tx_count;
    char root[160], path[240];
    snprintf(root, sizeof(root), "%s/dedup-tests", app->directory);
    IrLabProfile p, loaded;
    assert(ir_lab_profile_create(app->storage, root, &p));
    IrLabSignal raw = protocol_inax_signal(0x3A, 0xE7, 2), variant = raw;
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionRearWash));
    for(unsigned t = 0; t < variant.count; ++t)
        variant.timings[t] += variant.timings[t] / 20;
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionRearWash));
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionMassage));
    variant = protocol_inax_signal(0x3A, 0x81, 2);
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    variant = protocol_inax_signal(0x3A, 0xE7, 3);
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    variant = raw;
    variant.frequency = 40000;
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    variant = raw;
    variant.duty_cycle = .25f;
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    variant = raw;
    for(unsigned t = 0; t < variant.count; ++t)
        variant.timings[t] += variant.timings[t] / 5;
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    variant = protocol_toto_signal(0x10, 0xE0, 2);
    assert(ir_lab_profile_append(app->storage, root, &p, &variant, IrLabActionRearWash));
    IrLabSignal unknown = {.valid = true, .frequency = 38000, .duty_cycle = .33f, .count = 1024};
    for(unsigned t = 0; t < unknown.count; ++t)
        unknown.timings[t] = 100;
    assert(ir_lab_profile_append(app->storage, root, &p, &unknown, IrLabActionOther));
    assert(ir_lab_profile_append(app->storage, root, &p, &unknown, IrLabActionOther));
    unknown.timings[0] = 105;
    assert(ir_lab_profile_append(app->storage, root, &p, &unknown, IrLabActionOther));
    check(
        p.count == 13 && p.entries[0].signature == p.entries[1].signature,
        "Duplicate indexing tolerates small capture jitter while retaining original files");
    // The hash is only an index: a collision must still compare full data.
    p.entries[4].signature = p.entries[0].signature;
    IrLabProfile before = p;
    sim_rename_fail = true;
    unsigned removed = 99;
    bool pending = false;
    check(
        !ir_lab_profile_deduplicate(app->storage, root, &p, &removed, &pending) && !removed &&
            !memcmp(&p, &before, sizeof(p)),
        "A failed duplicate-index publication leaves the current profile unchanged");
    sim_rename_fail = false;
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 13,
        "Restart after failed cleanup still loads every original signal");
    IrLabSignal check_signal;
    check(
        ir_lab_profile_read(app->storage, root, &p, 1, &check_signal),
        "Duplicate raw files are not deleted before the index commits");
    sim_remove_fail = true;
    check(
        ir_lab_profile_deduplicate(app->storage, root, &p, &removed, &pending) && removed == 3 &&
            pending && p.count == 10 && p.retired_count == 3,
        "Deduplication commits retained signals before reporting deferred SD cleanup");
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 10 &&
            loaded.retired_count == 3,
        "Pending duplicate-file cleanup survives restarting the app");
    sim_remove_fail = false;
    p = loaded;
    check(
        ir_lab_profile_deduplicate(app->storage, root, &p, &removed, &pending) && !removed &&
            !pending && !p.retired_count && p.count == 10,
        "Retry finishes deferred file deletion without changing the retained signal list");
    check(
        p.entries[0].file_id == 1 && p.entries[1].file_id == 4 &&
            p.entries[1].action == IrLabActionMassage,
        "Cleanup preserves first occurrence, order and distinct labels with stable file IDs");
    unsigned retained[] = {1, 4, 5, 6, 7, 8, 9, 10, 11, 13};
    for(unsigned i = 0; i < 10; ++i)
        assert(p.entries[i].file_id == retained[i]);
    for(unsigned i = 0; i < 3; ++i) {
        unsigned file_id = i == 0 ? 2 : i == 1 ? 3 : 12;
        snprintf(path, sizeof(path), "%s/profile-%03u/signal-%03u.ir", root, p.id, file_id);
        assert(storage_common_stat(app->storage, path, NULL) == FSE_NOT_EXIST);
    }
    check(true, "Redundant saved IR files are removed after successful publication");
    check(
        ir_lab_profile_read(app->storage, root, &p, 0, &check_signal) &&
            !memcmp(&check_signal, &raw, sizeof(raw)),
        "Duplicate cleanup keeps the first capture byte-for-byte unchanged");
    uint32_t next_id = p.next_id;
    before = p;
    sim_meta_rename_fail_after = 0;
    check(
        !ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionMusicStart) &&
            !memcmp(&p, &before, sizeof(p)),
        "A failed index commit after writing IR preserves the previous profile");
    sim_meta_rename_fail_after = -1;
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 10,
        "Restart ignores an uncommitted capture after metadata failure");
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionMusicStart));
    check(
        p.entries[10].file_id == next_id && p.next_id == next_id + 1 &&
            ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 11,
        "New captures append safely after deduplication without reusing retired signal IDs");
    check(
        ir_lab_profile_set_maker(app->storage, root, &p, IrLabMakerPanasonic) &&
            ir_lab_profile_load(app->storage, root, p.id, &loaded) &&
            loaded.maker == IrLabMakerPanasonic && loaded.entries[0].protocol == IrLabProtocolInax,
        "Maker labels persist independently of protocol detection");
    before = p;
    sim_sync_fail = true;
    check(
        !ir_lab_profile_set_maker(app->storage, root, &p, IrLabMakerToto) &&
            !memcmp(&p, &before, sizeof(p)),
        "A failed maker save preserves the previous maker and signals");
    sim_sync_fail = false;

    // Exercise the new profile menu and maker prompt through actual inputs.
    assert(ir_lab_profile_create(app->storage, LAB_PROFILES, &app->profile));
    assert(ir_lab_profile_append(
        app->storage, LAB_PROFILES, &app->profile, &raw, IrLabActionRearWash));
    assert(ir_lab_profile_append(
        app->storage, LAB_PROFILES, &app->profile, &raw, IrLabActionRearWash));
    app->profile_open = true;
    lab_go(app, LabSniff, 4);
    sim_snapshot(app, "Sniff profile: remove duplicates action");
    sim_press(app, InputKeyOk, InputTypeShort);
    check(
        app->screen == LabMessage && app->profile.count == 1 &&
            strstr(app->message[0], "Removed 1"),
        "Remove duplicates is a single explicit action on the sniff profile");
    sim_snapshot(app, "Duplicate cleanup result");
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, 2);
    check(app->screen == LabProfileMaker, "Saving a profile opens the maker picker");
    sim_snapshot(app, "Maker picker: LIXIL TOTO Panasonic and Unknown");
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        app->screen == LabSniff && app->profile.maker == IrLabMakerUnknown,
        "Back cancels maker selection without changing the stored label");
    choose(app, 2);
    sim_rename_fail = true;
    choose(app, IrLabMakerLixil);
    check(
        app->screen == LabMessage && app->profile_open && app->profile.maker == IrLabMakerUnknown,
        "Maker publication failure keeps the profile open for retry");
    sim_rename_fail = false;
    sim_press(app, InputKeyBack, InputTypeShort);
    choose(app, IrLabMakerLixil);
    check(
        app->screen == LabHome && !app->profile_open && app->profile.maker == IrLabMakerLixil &&
            sim_tx_count == tx,
        "Choosing LIXIL saves and closes without transmitting");
    lab_profiles_open(app);
    check(
        app->profile_makers[profile_row(app, app->profile.id)] == IrLabMakerLixil,
        "Saved-profile browser includes the user-selected maker");
    char display[40], expected[40];
    lab_menu_item(app, profile_row(app, app->profile.id), display);
    snprintf(expected, sizeof(expected), "LIXIL / INAX %03u", app->profile.id);
    check(!strcmp(display, expected), "Selected maker is followed by a stable automatic number");
    sim_snapshot(app, "Saved profile browser with maker labels");
    for(unsigned i = 0; i < app->profile.count; ++i) {
        snprintf(
            path,
            sizeof(path),
            "%s/profile-%03u/signal-%03lu.ir",
            LAB_PROFILES,
            app->profile.id,
            (unsigned long)app->profile.entries[i].file_id);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/profile-%03u/profile.meta", LAB_PROFILES, app->profile.id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/profile-%03u", LAB_PROFILES, app->profile.id);
    rmdir(path);
    sim_free(app);
}

static void test_diff_profiles(void) {
    IrLabApp* app = sim_app();
    unsigned tx = sim_tx_count;
    IrLabSignal original = set_reference(3, 0);
    assert(ir_lab_profile_create(app->storage, LAB_PROFILES, &app->profile));
    assert(ir_lab_profile_append(
        app->storage, LAB_PROFILES, &app->profile, &original, IrLabActionMassage));
    assert(ir_lab_profile_set_maker(app->storage, LAB_PROFILES, &app->profile, IrLabMakerLixil));
    app->profile_open = true;
    IrLabProfile sniff = app->profile;
    lab_go(app, LabSlots, 0);
    choose(app, lab_set_capacity(app->series));
    check(
        app->screen == LabMessage && strstr(app->message[0], "No captures"),
        "Save profile on an empty capture set does not create a profile");
    sim_press(app, InputKeyBack, InputTypeShort);
    unsigned first_id = 0;
    char first_name[40] = {0};
    for(unsigned series = 0; series < LAB_SETS; ++series) {
        open_set_from_home(app, series);
        unsigned capacity = lab_set_capacity(series);
        for(unsigned slot = 0; slot < capacity; ++slot) {
            choose(app, slot);
            IrLabSignal raw = set_reference(series, slot);
            sim_deliver(app, &raw);
        }
        app->changes[0] ^= 1;
        lab_slot_sync(app);
        LabCaptureSet before = app->sets[series];
        IrLabSignal seed = app->seed;
        uint8_t ids[IR_LAB_MAX_PROFILES], makers[IR_LAB_MAX_PROFILES];
        unsigned previous = ir_lab_profile_list(app->storage, LAB_PROFILES, ids, makers);
        sim_press(app, InputKeyOk, InputTypeShort);
        app->row = 5;
        sim_snapshot(app, series == 0 ? "Diff actions: Save profile" : "Diff profile save action");
        sim_press(app, InputKeyOk, InputTypeShort);
        check(
            app->screen == LabProfileMaker && app->profile_maker_return == LabCompareAction,
            "Diff Save profile opens the existing maker picker for the whole set");
        if(series == 0) {
            sim_press(app, InputKeyBack, InputTypeShort);
            check(
                app->screen == LabCompareAction &&
                    ir_lab_profile_list(app->storage, LAB_PROFILES, ids, makers) == previous &&
                    !memcmp(&app->sets[series], &before, sizeof(before)),
                "Canceling a diff save returns to comparison without changing captures or profiles");
            choose(app, 5);
            sim_sd_fail = true;
            choose(app, IrLabMakerToto);
            check(
                app->screen == LabMessage && !memcmp(&app->profile, &sniff, sizeof(sniff)) &&
                    !memcmp(&app->sets[series], &before, sizeof(before)),
                "An SD failure retains both the comparison bank and an open sniff profile");
            sim_sd_fail = false;
            sim_press(app, InputKeyBack, InputTypeShort);
        }
        choose(app, IrLabMakerToto);
        check(
            app->screen == LabMessage && strstr(app->message[0], "Saved TOTO ") &&
                !memcmp(&app->sets[series], &before, sizeof(before)) &&
                !memcmp(&app->seed, &seed, sizeof(seed)) &&
                !memcmp(&app->profile, &sniff, sizeof(sniff)) && app->profile_open,
            "Saving comparison originals preserves edits, active signal and the open sniff profile");
        sim_snapshot(
            app, series == 0 ? "Diff saved as TOTO plus automatic number" : "Diff profile saved");
        uint8_t saved_ids[IR_LAB_MAX_PROFILES];
        unsigned count = ir_lab_profile_list(app->storage, LAB_PROFILES, saved_ids, makers);
        check(count == previous + 1, "A completed diff save publishes exactly one new profile");
        unsigned id = 0;
        for(unsigned i = 0; i < count; ++i) {
            bool known = false;
            for(unsigned j = 0; j < previous; ++j)
                known |= ids[j] == saved_ids[i];
            if(!known) id = saved_ids[i];
        }
        IrLabProfile saved;
        check(
            ir_lab_profile_load(app->storage, LAB_PROFILES, id, &saved) &&
                saved.count == capacity && saved.maker == IrLabMakerToto,
            "Every comparison set reloads as a persistent numbered maker profile");
        for(unsigned slot = 0; slot < capacity; ++slot) {
            IrLabSignal raw, expected = set_reference(series, slot);
            unsigned action = series == 0 ? IrLabActionPressure :
                              series == 1 ? IrLabActionWaterTemp :
                              series == 2 ? IrLabActionSeatTemp :
                              series == 4 ? (slot ? IrLabActionMusicStop : IrLabActionMusicStart) :
                              series == 5 ? (slot ? IrLabActionFlushSmall : IrLabActionFlushBig) :
                                            IrLabActionOther;
            check(
                ir_lab_profile_read(app->storage, LAB_PROFILES, &saved, slot, &raw) &&
                    !memcmp(&raw, &expected, sizeof(raw)) && saved.entries[slot].action == action,
                "Diff export retains original waveforms, order, carrier and capture-set labels");
        }
        char name[40];
        lab_profile_name(name, sizeof(name), saved.maker, id);
        if(!first_id) {
            first_id = id;
            snprintf(first_name, sizeof(first_name), "%s", name);
        } else
            check(
                strcmp(name, first_name) != 0,
                "Same-maker profiles receive different numeric suffixes");
        sim_press(app, InputKeyBack, InputTypeShort);
        check(
            app->screen == LabCompareAction, "Saved result returns directly to the diff actions");
    }
    check(sim_tx_count == tx, "Diff saving, maker selection and retries never transmit IR");
    sim_free(app);
    app = sim_app();
    lab_profiles_open(app);
    unsigned row = profile_row(app, first_id);
    char name[40];
    lab_menu_item(app, row, name);
    check(!strcmp(name, first_name), "Maker suffix remains stable after restarting the app");
    choose(app, row);
    check(!strcmp(app->lines[0], first_name), "Loaded profile header uses the same maker suffix");
    choose(app, 0);
    choose(app, 0);
    check(
        app->screen == LabRange && app->seed_from_profile && sim_tx_count == tx,
        "A diff-created profile loads into the existing fuzzer without transmitting");
    sim_free(app);
    app = sim_app();
    open_set_from_home(app, 3);
    choose(app, 1);
    IrLabSignal unknown = {
        .valid = true,
        .count = 3,
        .frequency = 38000,
        .duty_cycle = 0.33f,
        .timings = {111, 222, 333}};
    sim_deliver(app, &unknown);
    choose(app, 4);
    IrLabSignal sparse = set_reference(3, 4);
    sim_deliver(app, &sparse);
    uint8_t previous_ids[IR_LAB_MAX_PROFILES], ids[IR_LAB_MAX_PROFILES];
    unsigned previous = ir_lab_profile_list(app->storage, LAB_PROFILES, previous_ids, NULL);
    lab_go(app, LabSlots, 0);
    choose(app, 5);
    check(
        app->screen == LabProfileMaker,
        "Sparse and unmatched raw captures can be saved from the slot list without opening diff");
    sim_snapshot(app, "Save capture set: existing maker picker");
    choose(app, IrLabMakerOther);
    unsigned count = ir_lab_profile_list(app->storage, LAB_PROFILES, ids, NULL), id = 0;
    for(unsigned i = 0; i < count; ++i) {
        bool known = false;
        for(unsigned j = 0; j < previous; ++j)
            known |= previous_ids[j] == ids[i];
        if(!known) id = ids[i];
    }
    IrLabProfile saved;
    IrLabSignal copy;
    check(
        ir_lab_profile_load(app->storage, LAB_PROFILES, id, &saved) && saved.count == 2 &&
            saved.entries[0].action == IrLabActionOther &&
            ir_lab_profile_read(app->storage, LAB_PROFILES, &saved, 0, &copy) &&
            !memcmp(&copy, &unknown, sizeof(copy)) &&
            ir_lab_profile_read(app->storage, LAB_PROFILES, &saved, 1, &copy) &&
            !memcmp(&copy, &sparse, sizeof(copy)),
        "Saving a sparse set skips empty slots and preserves undecodable originals in slot order");
    sim_press(app, InputKeyBack, InputTypeShort);
    check(
        app->screen == LabSlots && sim_tx_count == tx,
        "Saving from slots returns there without IR");
    sim_free(app);
}

typedef struct {
    IrLabSignal raw;
    int fail_at;
    bool fail_cleanup;
} ProfileImportFixture;

static bool
    profile_import_source(void* context, unsigned index, IrLabSignal* signal, unsigned* action) {
    ProfileImportFixture* fixture = context;
    if((int)index == fixture->fail_at) {
        if(fixture->fail_cleanup) sim_remove_fail = true;
        return false;
    }
    *signal = fixture->raw;
    *action = IrLabActionPressure;
    return true;
}

static void test_profile_import_failures(void) {
    IrLabApp* app = sim_app();
    char root[180];
    snprintf(root, sizeof(root), "%s/import-tests", app->directory);
    ProfileImportFixture fixture = {
        .raw =
            {.valid = true, .count = IR_LAB_MAX_TIMINGS, .frequency = 40000, .duty_cycle = 0.25f},
        .fail_at = 1};
    for(unsigned i = 0; i < fixture.raw.count; ++i)
        fixture.raw.timings[i] = 600;
    IrLabProfile saved = {.id = 63, .maker = IrLabMakerPanasonic};
    IrLabProfile before = saved;
    uint8_t ids[IR_LAB_MAX_PROFILES];
    check(
        !ir_lab_profile_import(
            app->storage, root, 0, IrLabMakerToto, profile_import_source, &fixture, &saved) &&
            !ir_lab_profile_import(
                app->storage, root, 65, IrLabMakerToto, profile_import_source, &fixture, &saved) &&
            !memcmp(&saved, &before, sizeof(saved)),
        "Empty and oversized batch imports leave the existing profile untouched");
    check(
        !ir_lab_profile_import(
            app->storage, root, 3, IrLabMakerToto, profile_import_source, &fixture, &saved) &&
            !memcmp(&saved, &before, sizeof(saved)) &&
            !ir_lab_profile_list(app->storage, root, ids, NULL),
        "A source read failure after one saved signal publishes no partial profile");
    fixture.fail_at = -1;
    sim_meta_rename_fail_after = 1;
    check(
        !ir_lab_profile_import(
            app->storage, root, 3, IrLabMakerToto, profile_import_source, &fixture, &saved) &&
            !memcmp(&saved, &before, sizeof(saved)) &&
            !ir_lab_profile_list(app->storage, root, ids, NULL),
        "Final index publication failure never exposes a partly exported comparison");
    sim_meta_rename_fail_after = -1;
    fixture.fail_at = 1;
    fixture.fail_cleanup = true;
    check(
        !ir_lab_profile_import(
            app->storage, root, 3, IrLabMakerToto, profile_import_source, &fixture, &saved),
        "Interrupted raw cleanup reports the import as failed");
    sim_remove_fail = false;
    check(
        !ir_lab_profile_list(app->storage, root, ids, NULL) &&
            !ir_lab_profile_load(app->storage, root, 1, &saved) &&
            !memcmp(&saved, &before, sizeof(saved)),
        "An unfinished import stays hidden and unloadable even when cleanup was interrupted");
    fixture.fail_at = -1;
    fixture.fail_cleanup = false;
    check(
        ir_lab_profile_import(
            app->storage, root, 3, IrLabMakerToto, profile_import_source, &fixture, &saved) &&
            saved.id == 2 && saved.count == 3 &&
            ir_lab_profile_list(app->storage, root, ids, NULL) == 1,
        "Retry publishes one complete profile without an unfinished import becoming visible");
    IrLabSignal raw;
    check(
        ir_lab_profile_read(app->storage, root, &saved, 2, &raw) &&
            !memcmp(&raw, &fixture.raw, sizeof(raw)),
        "Batch export preserves maximum-sized undecoded raw signals and carrier metadata");
    sim_free(app);
}

static void test_legacy_profile_migration(void) {
    IrLabApp* app = sim_app();
    char root[160], path[240];
    snprintf(root, sizeof(root), "%s/legacy-tests", app->directory);
    IrLabProfile p, loaded;
    IrLabSignal raw = protocol_inax_signal(0x3A, 0x81, 2), copy;
    assert(ir_lab_profile_create(app->storage, root, &p));
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionMassage));
    assert(ir_lab_profile_append(app->storage, root, &p, &raw, IrLabActionMassage));
    snprintf(path, sizeof(path), "%s/profile-%03u/profile.meta", root, p.id);
    assert(unlink(path) == 0); // Exactly the v0.15 on-disk layout.
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 2 &&
            loaded.maker == IrLabMakerUnknown && loaded.next_id == 3,
        "Existing v0.15 profiles load without migration or an assumed maker");
    sim_meta_rename_fail_after = 1; // Freeze legacy index, then fail new-entry commit.
    check(
        !ir_lab_profile_append(app->storage, root, &loaded, &raw, IrLabActionOther),
        "Failed legacy append does not publish a capture through legacy discovery");
    sim_meta_rename_fail_after = -1;
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 2,
        "A failed migration append leaves the original two signals reloadable");
    check(
        ir_lab_profile_set_maker(app->storage, root, &loaded, IrLabMakerToto) &&
            ir_lab_profile_read(app->storage, root, &loaded, 0, &copy) &&
            !memcmp(&copy, &raw, sizeof(raw)),
        "Adding a maker migrates metadata without rewriting captured IR");
    unsigned removed;
    bool pending;
    check(
        ir_lab_profile_deduplicate(app->storage, root, &loaded, &removed, &pending) &&
            removed == 1 && !pending && loaded.count == 1 && loaded.maker == IrLabMakerToto,
        "Legacy profiles support persistent duplicate removal after migration");
    assert(ir_lab_profile_append(app->storage, root, &loaded, &raw, IrLabActionOther));
    check(
        ir_lab_profile_load(app->storage, root, p.id, &loaded) && loaded.count == 2 &&
            loaded.entries[0].file_id == 1 && loaded.entries[1].file_id == 3,
        "Migrated profiles reload noncontiguous stable signal IDs");
    IrLabProfile before = loaded;
    FILE* bad = fopen(path, "w");
    assert(bad);
    fputs("Filetype: IR lab profile\nVersion: 1\nmaker_id: 999\nsignal_count: 0\n", bad);
    fclose(bad);
    check(
        !ir_lab_profile_load(app->storage, root, p.id, &loaded) &&
            !memcmp(&loaded, &before, sizeof(loaded)),
        "Invalid maker metadata cannot partially replace a loaded profile");
    sim_free(app);
}

static void interactive(void) {
    IrLabApp* app = sim_app();
    char command[32];
    puts(
        "HOST SIMULATION: actual C controller, no physical IR.\nKeys: u d l r ok holdok back holdback | n synthetic NECext | wait (+10s) | quit");
    while(app->running) {
        lab_preview_poll(app);
        lab_auto_poll(app);
        lab_refresh(app);
        for(unsigned i = 0; i < LAB_LINES; ++i)
            printf("| %-21s |\n", app->lines[i]);
        printf(
            "emissions=%u; pending=%d; locked=%d\n> ",
            sim_tx_count,
            app->gate.pending_observation,
            app->gate.locked);
        fflush(stdout);
        if(!fgets(command, sizeof(command), stdin)) break;
        command[strcspn(command, "\r\n")] = 0;
        if(strcmp(command, "quit") == 0) break;
        if(strcmp(command, "tick") == 0) {
            sim_tick += 1000;
            continue;
        }
        if(strcmp(command, "wait") == 0) {
            sim_tick += 10000;
            continue;
        }
        if(strcmp(command, "n") == 0 && app->rx_running) {
            InfraredMessage message = {
                .protocol = InfraredProtocolNECext,
                .address = 0x1234,
                .command = app->capture_stop  ? 0 :
                           app->capture_batch ? 0x5610 + app->capture_slot :
                                                0x5678};
            IrLabSignal signal = reference_press(message, 1);
            sim_deliver(app, &signal);
            continue;
        }
        struct {
            const char* name;
            InputKey key;
            InputType type;
        } inputs[] = {
            {"u", InputKeyUp, InputTypeShort},
            {"d", InputKeyDown, InputTypeShort},
            {"l", InputKeyLeft, InputTypeShort},
            {"r", InputKeyRight, InputTypeShort},
            {"ok", InputKeyOk, InputTypeShort},
            {"holdok", InputKeyOk, InputTypeLong},
            {"back", InputKeyBack, InputTypeShort},
            {"holdback", InputKeyBack, InputTypeLong}};
        for(unsigned i = 0; i < COUNT_OF(inputs); ++i)
            if(strcmp(command, inputs[i].name) == 0) sim_press(app, inputs[i].key, inputs[i].type);
    }
    sim_free(app);
}

int main(int argc, char** argv) {
    if(argc > 1 && strcmp(argv[1], "--interactive") == 0) {
        interactive();
        return 0;
    }
    sim_report = fopen(argc > 1 ? argv[1] : "/tmp/ir-lab-simulation-report.txt", "w");
    assert(sim_report);
    fprintf(
        sim_report,
        "Host simulation: actual app C + Unleashed 084 codecs. IR, worker timing, SD and RTOS are modeled.\n");
    test_protocol_ui();
    test_protocol_prefix_and_failure();
    test_fuzzer_presets();
    test_sniff_profiles();
    test_profile_storage_limits();
    test_profile_makers_and_duplicates();
    test_legacy_profile_migration();
    test_diff_profiles();
    test_profile_import_failures();
    test_simple_byte_ui();
    test_fuzz_end_captures();
    test_many_byte_rules();
    test_simple_preview_manual();
    test_live_auto_range(1, false);
    test_live_auto_range(2, true);
    test_auto_pause_and_resume();
    test_auto_pause_races();
    test_auto_failures_and_stop();
    test_command_pair_sets();
    test_long_auto_ranges();
    test_compare_to_fuzzer();
    test_capture_bank();
    test_independent_capture_sets();
    test_maximum_capture_sets();
    test_compare_second_frame();
    test_original_without_stop();
    test_hex_flow();
    test_nec_repeats();
    test_width_edit(InfraredProtocolSIRC, 12);
    test_width_edit(InfraredProtocolSIRC15, 15);
    test_width_edit(InfraredProtocolSIRC20, 20);
    test_original_replay(InfraredProtocolNEC);
    test_original_replay(InfraredProtocolSIRC);
    test_original_replay(InfraredProtocolRC5);
    test_original_replay(InfraredProtocolRC6);
    test_faults();
    test_frame_navigation();
    test_unsupported();
    test_reference_file("tests/fixtures/BioBidet_BB-2000.ir", 71);
    test_reference_file("tests/fixtures/Toto.ir", 39);
    test_reference_file("tests/fixtures/Lixil.ir", 24);
    test_reference_file("tests/fixtures/TotoWashlet.ir", 39);
    test_parsed_references();
    fprintf(
        sim_report,
        "\n%u checks passed; %u reference buttons modeled. No physical IR emitted.\n",
        sim_checks,
        fixture_count);
    fclose(sim_report);
    printf("Host simulation: %u checks passed; %u reference buttons\n", sim_checks, fixture_count);
    return 0;
}
