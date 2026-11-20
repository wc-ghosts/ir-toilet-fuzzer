#include "ir_lab_profile.h"
#ifndef IR_LAB_HOST_SIM
#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* ir_lab_action_name(unsigned action) {
    static const char* const names[] = {
        "Flush - big",
        "Flush - small",
        "Music start",
        "Music stop",
        "Oshiri / rear wash",
        "Bidet / front wash",
        "Wash stop",
        "Massage",
        "Pressure up",
        "Pressure down",
        "Water temperature",
        "Seat temperature",
        "Nozzle cleaning",
        "Position forward",
        "Position back",
        "Oscillate",
        "Dry start",
        "Dry stop",
        "Eco",
        "Other",
        "Pressure"};
    return action < IrLabActionCount ? names[action] : "Unknown label";
}

const char* ir_lab_maker_name(unsigned maker) {
    static const char* const names[] = {
        "Unknown", "LIXIL / INAX", "TOTO", "Panasonic", "BioBidet", "Toshiba", "Other"};
    return maker < IrLabMakerCount ? names[maker] : "Unknown";
}

static void profile_path(
    char* path,
    size_t size,
    const char* root,
    unsigned id,
    unsigned index,
    bool temporary) {
    snprintf(
        path, size, "%s/profile-%03u/signal-%03u.%s", root, id, index, temporary ? "tmp" : "ir");
}

static uint32_t profile_hash(const IrLabSignal* signal, const IrLabBits* bits) {
    uint32_t hash = 2166136261U;
    if(bits->valid) {
        for(unsigned f = 0; f < bits->frames; ++f) {
            hash = (hash ^ bits->length[f]) * 16777619U;
        }
        hash = (hash ^ bits->frames) * 16777619U;
        hash = (hash ^ bits->repeats) * 16777619U;
        hash = (hash ^ bits->pulse_width) * 16777619U;
        for(unsigned b = 0; b < (bits->count + 7U) / 8U; ++b)
            hash = (hash ^ bits->data[b]) * 16777619U;
    } else {
        for(unsigned t = 0; t < signal->count; ++t)
            hash = (hash ^ signal->timings[t]) * 16777619U;
    }
    return hash;
}

static bool profile_read_entry(
    Storage* storage,
    const char* root,
    unsigned id,
    unsigned index,
    IrLabProfileEntry* entry,
    IrLabSignal* signal,
    bool temporary) {
    char path[180];
    profile_path(path, sizeof(path), root, id, index, temporary);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    uint32_t action = IrLabActionCount, count = 0, values = 0, version = 0;
    memset(signal, 0, sizeof(*signal));
    bool ok = flipper_format_file_open_existing(ff, path) &&
              flipper_format_read_uint32(ff, "profile_version", &version, 1) && version == 1 &&
              flipper_format_read_uint32(ff, "action_id", &action, 1) &&
              action < IrLabActionCount &&
              flipper_format_read_uint32(ff, "timings_count", &count, 1) && count > 0 &&
              count <= IR_LAB_MAX_TIMINGS &&
              flipper_format_read_uint32(ff, "frequency", &signal->frequency, 1) &&
              flipper_format_read_float(ff, "duty_cycle", &signal->duty_cycle, 1) &&
              flipper_format_get_value_count(ff, "data", &values) && values == count &&
              flipper_format_rewind(ff) &&
              flipper_format_read_uint32(ff, "data", signal->timings, count);
    bool closed = flipper_format_file_close(ff);
    flipper_format_free(ff);
    signal->valid = true;
    signal->count = count;
    ok = ok && closed && ir_lab_signal_valid(signal);
    if(!ok) {
        memset(signal, 0, sizeof(*signal));
        return false;
    }
    IrLabBits* bits = malloc(sizeof(*bits));
    if(!bits) return false;
    IrLabProtocolAnalysis analysis = {0};
    if(ir_lab_extract_bits(signal, bits)) ir_lab_protocol_analyze(signal, bits, &analysis);
    uint32_t signature = profile_hash(signal, bits);
    free(bits);
    *entry = (IrLabProfileEntry){
        .file_id = index,
        .signature = signature,
        .action = action,
        .protocol = analysis.kind,
        .timings = count,
        .frequency = signal->frequency,
        .duty_cycle = signal->duty_cycle};
    return true;
}

static void
    profile_meta_path(char* path, size_t size, const char* root, unsigned id, bool temporary) {
    snprintf(path, size, "%s/profile-%03u/profile.%s", root, id, temporary ? "tmp" : "meta");
}

static bool profile_read_ids(FlipperFormat* ff, const char* key, uint32_t* ids, unsigned count) {
    if(!count) return true;
    uint32_t actual = 0;
    return flipper_format_get_value_count(ff, key, &actual) && actual == count &&
           flipper_format_rewind(ff) && flipper_format_read_uint32(ff, key, ids, count);
}

static bool
    profile_read_meta(Storage* storage, const char* root, unsigned id, IrLabProfile* output) {
    char path[180];
    profile_meta_path(path, sizeof(path), root, id, false);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    uint32_t version = 0, maker = 0, count = 0, retired = 0, ids[IR_LAB_PROFILE_SIGNALS];
    memset(output, 0, sizeof(*output));
    output->id = id;
    bool ok = flipper_format_file_open_existing(ff, path) &&
              flipper_format_read_uint32(ff, "Version", &version, 1) && version == 1 &&
              flipper_format_read_uint32(ff, "maker_id", &maker, 1) && maker < IrLabMakerCount &&
              flipper_format_read_uint32(ff, "signal_count", &count, 1) &&
              count <= IR_LAB_PROFILE_SIGNALS &&
              flipper_format_read_uint32(ff, "next_signal_id", &output->next_id, 1) &&
              output->next_id && profile_read_ids(ff, "signal_ids", ids, count) &&
              flipper_format_read_uint32(ff, "retired_count", &retired, 1) &&
              retired <= IR_LAB_PROFILE_SIGNALS &&
              profile_read_ids(ff, "retired_ids", output->retired, retired);
    bool closed = flipper_format_file_close(ff);
    flipper_format_free(ff);
    ok = ok && closed;
    output->maker = maker;
    output->count = count;
    output->retired_count = retired;
    for(unsigned i = 0; i < count && ok; ++i) {
        ok = ids[i] && ids[i] < output->next_id;
        for(unsigned j = 0; j < i; ++j)
            ok &= ids[i] != ids[j];
        output->entries[i].file_id = ids[i];
    }
    for(unsigned i = 0; i < retired && ok; ++i) {
        ok = output->retired[i] && output->retired[i] < output->next_id;
        for(unsigned j = 0; j < count; ++j)
            ok &= output->retired[i] != ids[j];
        for(unsigned j = 0; j < i; ++j)
            ok &= output->retired[i] != output->retired[j];
    }
    return ok;
}

static bool profile_write_ids(File* file, const char* key, const uint32_t* ids, unsigned count) {
    char text[80];
    int n = snprintf(text, sizeof(text), "%s:", key);
    bool ok = storage_file_write(file, text, n) == (size_t)n;
    for(unsigned i = 0; i < count && ok; ++i) {
        n = snprintf(text, sizeof(text), " %lu", (unsigned long)ids[i]);
        ok = storage_file_write(file, text, n) == (size_t)n;
    }
    return ok && storage_file_write(file, "\n", 1) == 1;
}

static bool profile_store_meta(Storage* storage, const char* root, const IrLabProfile* profile) {
    char path[180], temporary[180], text[256];
    profile_meta_path(path, sizeof(path), root, profile->id, false);
    profile_meta_path(temporary, sizeof(temporary), root, profile->id, true);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, temporary, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    int n = snprintf(
        text,
        sizeof(text),
        "Filetype: IR lab profile\nVersion: 1\nmaker: %s\nmaker_id: %u\nsignal_count: %u\nnext_signal_id: %lu\n",
        ir_lab_maker_name(profile->maker),
        profile->maker,
        profile->count,
        (unsigned long)profile->next_id);
    ok = ok && n > 0 && (size_t)n < sizeof(text) && storage_file_write(file, text, n) == (size_t)n;
    uint32_t ids[IR_LAB_PROFILE_SIGNALS];
    for(unsigned i = 0; i < profile->count; ++i)
        ids[i] = profile->entries[i].file_id;
    ok = ok && profile_write_ids(file, "signal_ids", ids, profile->count);
    n = snprintf(text, sizeof(text), "retired_count: %u\n", profile->retired_count);
    ok = ok && storage_file_write(file, text, n) == (size_t)n &&
         profile_write_ids(file, "retired_ids", profile->retired, profile->retired_count) &&
         storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    ok = ok && closed && storage_common_rename(storage, temporary, path) == FSE_OK;
    if(!ok) storage_common_remove(storage, temporary);
    return ok;
}

static bool profile_cleanup(Storage* storage, const char* root, IrLabProfile* profile) {
    if(!profile->retired_count) return true;
    bool ok = true;
    for(unsigned i = 0; i < profile->retired_count; ++i) {
        char path[180];
        profile_path(path, sizeof(path), root, profile->id, profile->retired[i], false);
        FS_Error status = storage_common_stat(storage, path, NULL);
        if(status == FSE_OK)
            ok &= storage_common_remove(storage, path) == FSE_OK;
        else
            ok &= status == FSE_NOT_EXIST;
    }
    if(!ok) return false;
    IrLabProfile* next = malloc(sizeof(*next));
    if(!next) return false;
    *next = *profile;
    next->retired_count = 0;
    ok = profile_store_meta(storage, root, next);
    if(ok) *profile = *next;
    free(next);
    return ok;
}

static bool profile_write_signal(
    Storage* storage,
    const char* root,
    unsigned id,
    uint32_t file_id,
    const IrLabSignal* signal,
    unsigned action,
    IrLabProfileEntry* output) {
    char temporary[180], final[180], chunk[240];
    profile_path(temporary, sizeof(temporary), root, id, file_id, true);
    profile_path(final, sizeof(final), root, id, file_id, false);
    if(storage_common_stat(storage, final, NULL) == FSE_OK) return false;
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, temporary, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    int n = snprintf(
        chunk,
        sizeof(chunk),
        "Filetype: IR signals file\nVersion: 1\nname: %s\ntype: raw\n"
        "profile_version: 1\naction_id: %u\ntimings_count: %u\n"
        "frequency: %lu\nduty_cycle: %.9g\ndata:",
        ir_lab_action_name(action),
        action,
        (unsigned)signal->count,
        (unsigned long)signal->frequency,
        (double)signal->duty_cycle);
    ok = ok && n > 0 && (size_t)n < sizeof(chunk) &&
         storage_file_write(file, chunk, n) == (size_t)n;
    unsigned used = 0;
    for(unsigned i = 0; i < signal->count && ok; ++i) {
        if(sizeof(chunk) - used < 14) {
            ok = storage_file_write(file, chunk, used) == used;
            used = 0;
        }
        n = snprintf(
            chunk + used, sizeof(chunk) - used, " %lu", (unsigned long)signal->timings[i]);
        used += n;
    }
    ok = ok && storage_file_write(file, chunk, used) == used;
    ok = ok && storage_file_write(file, "\n", 1) == 1 && storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    ok = ok && closed;
    IrLabSignal* check = malloc(sizeof(*check));
    IrLabProfileEntry entry;
    ok = ok && check && profile_read_entry(storage, root, id, file_id, &entry, check, true);
    free(check);
    if(ok) ok = storage_common_rename(storage, temporary, final) == FSE_OK;
    if(!ok) {
        storage_common_remove(storage, temporary);
        return false;
    }
    *output = entry;
    return true;
}

bool ir_lab_profile_create(Storage* storage, const char* root, IrLabProfile* output) {
    if(!storage_simply_mkdir(storage, root)) return false;
    IrLabProfile* next = calloc(1, sizeof(*next));
    if(!next) return false;
    bool ok = false;
    for(unsigned id = 1; id <= IR_LAB_MAX_PROFILES; ++id) {
        char path[180];
        snprintf(path, sizeof(path), "%s/profile-%03u", root, id);
        FS_Error result = storage_common_mkdir(storage, path);
        if(result == FSE_EXIST) continue;
        if(result != FSE_OK) break;
        next->id = id;
        next->next_id = 1;
        ok = profile_store_meta(storage, root, next);
        if(ok)
            *output = *next;
        else
            storage_common_remove(storage, path);
        break;
    }
    free(next);
    return ok;
}

static bool profile_remove_if_present(Storage* storage, const char* path) {
    FS_Error status = storage_common_stat(storage, path, NULL);
    return status == FSE_NOT_EXIST ||
           (status == FSE_OK && storage_common_remove(storage, path) == FSE_OK);
}

static void
    profile_discard_import(Storage* storage, const char* root, unsigned id, unsigned count) {
    char path[180];
    bool clean = true;
    for(unsigned i = 1; i <= count; ++i) {
        profile_path(path, sizeof(path), root, id, i, false);
        clean &= profile_remove_if_present(storage, path);
        profile_path(path, sizeof(path), root, id, i, true);
        clean &= profile_remove_if_present(storage, path);
    }
    // Keep the empty index if raw cleanup fails: legacy discovery must never
    // mistake unfinished files for a published profile after a restart.
    if(!clean) return;
    profile_meta_path(path, sizeof(path), root, id, true);
    if(!profile_remove_if_present(storage, path)) return;
    profile_meta_path(path, sizeof(path), root, id, false);
    if(!profile_remove_if_present(storage, path)) return;
    snprintf(path, sizeof(path), "%s/profile-%03u", root, id);
    storage_common_remove(storage, path);
}

bool ir_lab_profile_import(
    Storage* storage,
    const char* root,
    unsigned count,
    unsigned maker,
    IrLabProfileSource source,
    void* context,
    IrLabProfile* output) {
    if(!count || count > IR_LAB_PROFILE_SIGNALS || maker >= IrLabMakerCount || !source || !output)
        return false;
    IrLabProfile* next = malloc(sizeof(*next));
    IrLabSignal* signal = malloc(sizeof(*signal));
    if(!next || !signal) {
        free(next);
        free(signal);
        return false;
    }
    bool created = ir_lab_profile_create(storage, root, next);
    bool ok = created;
    if(created) next->maker = maker;
    for(unsigned i = 0; i < count && ok; ++i) {
        unsigned action = IrLabActionCount;
        memset(signal, 0, sizeof(*signal));
        ok = source(context, i, signal, &action) && action < IrLabActionCount &&
             ir_lab_signal_valid(signal) && !signal->decoded &&
             profile_write_signal(
                 storage, root, next->id, i + 1, signal, action, &next->entries[i]);
        if(ok) ++next->count;
    }
    if(ok) {
        next->next_id = count + 1;
        ok = profile_store_meta(storage, root, next);
    }
    if(ok)
        *output = *next;
    else if(created)
        profile_discard_import(storage, root, next->id, count);
    free(signal);
    free(next);
    return ok;
}

unsigned ir_lab_profile_list(Storage* storage, const char* root, uint8_t* ids, uint8_t* makers) {
    IrLabProfile* meta = malloc(sizeof(*meta));
    if(!meta) return 0;
    unsigned count = 0;
    for(unsigned id = 1; id <= IR_LAB_MAX_PROFILES; ++id) {
        char path[180];
        profile_meta_path(path, sizeof(path), root, id, false);
        bool found = false;
        uint8_t maker = IrLabMakerUnknown;
        if(storage_common_stat(storage, path, NULL) == FSE_OK) {
            bool valid = profile_read_meta(storage, root, id, meta);
            found = !valid || meta->count > 0;
            if(valid) maker = meta->maker;
        } else {
            profile_path(path, sizeof(path), root, id, 1, false);
            found = storage_common_stat(storage, path, NULL) == FSE_OK;
        }
        if(found) {
            ids[count] = id;
            if(makers) makers[count] = maker;
            ++count;
        }
    }
    free(meta);
    return count;
}

bool ir_lab_profile_load(Storage* storage, const char* root, unsigned id, IrLabProfile* output) {
    if(!id || id > IR_LAB_MAX_PROFILES) return false;
    IrLabProfile* next = calloc(1, sizeof(*next));
    IrLabSignal* signal = malloc(sizeof(*signal));
    if(!next || !signal) {
        free(next);
        free(signal);
        return false;
    }
    next->id = id;
    next->next_id = 1;
    char path[180];
    profile_meta_path(path, sizeof(path), root, id, false);
    FS_Error status = storage_common_stat(storage, path, NULL);
    bool ok = true;
    if(status == FSE_OK) {
        ok = profile_read_meta(storage, root, id, next);
        for(unsigned i = 0; i < next->count && ok; ++i)
            ok = profile_read_entry(
                storage, root, id, next->entries[i].file_id, &next->entries[i], signal, false);
    } else if(status == FSE_NOT_EXIST) {
        // v0.15 profiles had contiguous files without profile metadata.
        bool gap = false;
        for(unsigned i = 0; i < IR_LAB_PROFILE_SIGNALS && ok; ++i) {
            profile_path(path, sizeof(path), root, id, i + 1, false);
            status = storage_common_stat(storage, path, NULL);
            if(status == FSE_NOT_EXIST) {
                gap = true;
                continue;
            }
            ok = status == FSE_OK && !gap &&
                 profile_read_entry(storage, root, id, i + 1, &next->entries[i], signal, false);
            if(ok) {
                ++next->count;
                next->next_id = i + 2;
            }
        }
    } else
        ok = false;
    ok = ok && next->count > 0;
    if(ok) *output = *next;
    free(signal);
    free(next);
    return ok;
}

bool ir_lab_profile_append(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    const IrLabSignal* signal,
    unsigned action) {
    if(!profile->id || profile->id > IR_LAB_MAX_PROFILES || !profile->next_id ||
       profile->count >= IR_LAB_PROFILE_SIGNALS || action >= IrLabActionCount ||
       !ir_lab_signal_valid(signal) || signal->decoded)
        return false;
    char path[180];
    profile_meta_path(path, sizeof(path), root, profile->id, false);
    FS_Error status = storage_common_stat(storage, path, NULL);
    // Freeze a legacy index before writing a new file, so a failed metadata
    // commit cannot accidentally expose that file through legacy discovery.
    if(status == FSE_NOT_EXIST) {
        if(!profile_store_meta(storage, root, profile)) return false;
    } else if(status != FSE_OK)
        return false;
    IrLabProfile* next = malloc(sizeof(*next));
    if(!next) return false;
    *next = *profile;
    uint32_t file_id = profile->next_id;
    bool available = false;
    for(unsigned tries = 0; tries < IR_LAB_PROFILE_SIGNALS && file_id < UINT32_MAX;
        ++tries, ++file_id) {
        profile_path(path, sizeof(path), root, profile->id, file_id, false);
        status = storage_common_stat(storage, path, NULL);
        if(status == FSE_NOT_EXIST) {
            available = true;
            break;
        }
        if(status != FSE_OK) break;
    }
    bool ok =
        available &&
        profile_write_signal(
            storage, root, profile->id, file_id, signal, action, &next->entries[next->count]);
    if(ok) {
        ++next->count;
        next->next_id = file_id + 1;
        ok = profile_store_meta(storage, root, next);
        if(ok)
            *profile = *next;
        else
            storage_common_remove(storage, path);
    }
    free(next);
    return ok;
}

bool ir_lab_profile_read(
    Storage* storage,
    const char* root,
    const IrLabProfile* profile,
    unsigned index,
    IrLabSignal* signal) {
    if(index >= profile->count || !profile->id || profile->id > IR_LAB_MAX_PROFILES) return false;
    IrLabProfileEntry entry;
    const IrLabProfileEntry* saved = &profile->entries[index];
    bool ok =
        profile_read_entry(storage, root, profile->id, saved->file_id, &entry, signal, false);
    return ok && entry.action == saved->action && entry.timings == saved->timings &&
           entry.frequency == saved->frequency && entry.duty_cycle == saved->duty_cycle;
}

bool ir_lab_profile_set_maker(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    unsigned maker) {
    if(!profile->id || maker >= IrLabMakerCount) return false;
    IrLabProfile* next = malloc(sizeof(*next));
    if(!next) return false;
    *next = *profile;
    next->maker = maker;
    bool ok = profile_store_meta(storage, root, next);
    if(ok) *profile = *next;
    free(next);
    return ok;
}

static bool profile_same_signal(
    const IrLabSignal* a,
    const IrLabSignal* b,
    IrLabBits* bits_a,
    IrLabBits* bits_b) {
    if(a->count != b->count || a->frequency != b->frequency || a->duty_cycle != b->duty_cycle)
        return false;
    if(!memcmp(a->timings, b->timings, a->count * sizeof(uint32_t))) return true;
    if(!ir_lab_extract_bits(a, bits_a) || !ir_lab_extract_bits(b, bits_b) ||
       bits_a->count != bits_b->count || bits_a->frames != bits_b->frames ||
       bits_a->repeats != bits_b->repeats || bits_a->pulse_width != bits_b->pulse_width ||
       memcmp(bits_a->data, bits_b->data, (bits_a->count + 7U) / 8U))
        return false;
    for(unsigned f = 0; f < bits_a->frames; ++f)
        if(bits_a->length[f] != bits_b->length[f]) return false;
    for(unsigned i = 0; i < a->count; ++i) {
        uint32_t high = a->timings[i] > b->timings[i] ? a->timings[i] : b->timings[i];
        uint32_t delta = a->timings[i] > b->timings[i] ? a->timings[i] - b->timings[i] :
                                                         b->timings[i] - a->timings[i];
        if((uint64_t)delta * 100 > (uint64_t)high * 15) return false;
    }
    return true;
}

bool ir_lab_profile_deduplicate(
    Storage* storage,
    const char* root,
    IrLabProfile* profile,
    unsigned* removed,
    bool* cleanup_pending) {
    *removed = 0;
    *cleanup_pending = false;
    if(!profile->id || !profile->count) return false;
    if(!profile_cleanup(storage, root, profile)) {
        *cleanup_pending = true;
        return false;
    }
    IrLabProfile* next = malloc(sizeof(*next));
    IrLabSignal* incoming = malloc(sizeof(*incoming));
    IrLabSignal* retained = malloc(sizeof(*retained));
    IrLabBits* bits_a = malloc(sizeof(*bits_a));
    IrLabBits* bits_b = malloc(sizeof(*bits_b));
    if(!next || !incoming || !retained || !bits_a || !bits_b) {
        free(next);
        free(incoming);
        free(retained);
        free(bits_a);
        free(bits_b);
        return false;
    }
    *next = *profile;
    next->count = 0;
    uint8_t kept[IR_LAB_PROFILE_SIGNALS];
    bool ok = true;
    for(unsigned i = 0; i < profile->count && ok; ++i) {
        ok = ir_lab_profile_read(storage, root, profile, i, incoming);
        bool duplicate = false;
        for(unsigned j = 0; j < next->count && ok && !duplicate; ++j) {
            const IrLabProfileEntry* a = &profile->entries[i];
            const IrLabProfileEntry* b = &next->entries[j];
            if(a->action != b->action || a->signature != b->signature ||
               a->timings != b->timings || a->frequency != b->frequency ||
               a->duty_cycle != b->duty_cycle)
                continue;
            ok = ir_lab_profile_read(storage, root, profile, kept[j], retained);
            if(ok) duplicate = profile_same_signal(incoming, retained, bits_a, bits_b);
        }
        if(!ok) break;
        if(duplicate)
            next->retired[next->retired_count++] = profile->entries[i].file_id;
        else {
            kept[next->count] = i;
            next->entries[next->count++] = profile->entries[i];
        }
    }
    unsigned count = profile->count - next->count;
    if(ok && count) {
        ok = profile_store_meta(storage, root, next);
        if(ok) {
            *profile = *next;
            *removed = count;
            *cleanup_pending = !profile_cleanup(storage, root, profile);
        }
    }
    free(next);
    free(incoming);
    free(retained);
    free(bits_a);
    free(bits_b);
    return ok;
}
