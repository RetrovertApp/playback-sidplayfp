///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// libsidplayfp Playback Plugin
//
// Implements RVPlaybackPlugin interface for Commodore 64 SID music formats.
// Based on libsidplayfp by Leandro Nini.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
#include <retrovert/settings.h>

#include <builders/residfp-builder/residfp.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define FREQ 48000

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

// Frames a single play() batch can generate. play() caps itself at 20000 cycles and the
// slowest supported clock (PAL, 985248 Hz) needs ~20.5 cycles per frame at 48 kHz, so a
// batch never exceeds ~976 frames; the rest is headroom.
#define PENDING_FRAMES_MAX 2048

struct SidPlayData {
    sidplayfp* engine;
    SidTune* tune;
    ReSIDfpBuilder* builder;
    uint8_t* song_data;
    uint32_t song_data_size;
    int sid_count; // Number of SID chips used by current tune (1-3)
    // A play() batch rounds up to whole cycles and so can generate a frame or two beyond
    // what the host asked for. play() resets the chip buffer, so the surplus is staged
    // here and handed out on the following reads instead of being returned over the
    // request (an ABI violation) or dropped (a click).
    int16_t pending[PENDING_FRAMES_MAX * 2];
    uint32_t pending_frames;
    uint32_t pending_read; // frames already handed out from the front of `pending`
    bool finished;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* sidplayfp_supported_extensions(void) {
    return "sid,psid,rsid,mus,str,prg,p00,c64";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* sidplayfp_create(const RVService* service_api) {
    (void)service_api;

    SidPlayData* data = new SidPlayData();
    memset(data, 0, sizeof(SidPlayData));

    data->engine = new sidplayfp();
    data->builder = new ReSIDfpBuilder("ReSIDfp");

    // Create SID emulators (support up to 3 SIDs for multi-SID tunes)
    unsigned int maxsids = data->engine->info().maxsids();
    data->builder->create(maxsids);

    if (!data->builder->getStatus()) {
        rv_error("Failed to create SID builder: %s", data->builder->error());
    }

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int sidplayfp_destroy(void* user_data) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);

    delete[] data->song_data;
    delete data->tune;
    delete data->builder;
    delete data->engine;
    delete data;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Check if a file is an Atari ST executable (TOS program).
// These share the .prg extension with C64 programs but contain 68000 code.
static bool is_atari_st_executable(const char* url) {
    FILE* f = fopen(url, "rb");
    if (!f) {
        return false;
    }
    uint8_t header[2];
    bool is_st = (fread(header, 1, 2, f) == 2 && header[0] == 0x60 && header[1] == 0x1a);
    fclose(f);
    return is_st;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int sidplayfp_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)service_api;

    // Reject Atari ST executables which share the .prg extension with C64 programs
    if (is_atari_st_executable(url)) {
        rv_error("Not a C64 file (Atari ST executable)");
        return -1;
    }

    SidPlayData* data = static_cast<SidPlayData*>(user_data);

    // Free previous tune if any
    if (data->tune) {
        data->engine->load(nullptr);
        delete data->tune;
        data->tune = nullptr;
    }

    // Free previous song data
    delete[] data->song_data;
    data->song_data = nullptr;
    data->song_data_size = 0;

    data->pending_frames = 0;
    data->pending_read = 0;
    data->finished = false;

    // Load tune from file. Using the filename-based constructor enables format
    // detection for PRG, P00, and C64 files (the buffer-based constructor only
    // supports PSID and MUS formats).
    data->tune = new SidTune(url);

    if (!data->tune->getStatus()) {
        rv_error("Failed to load SID tune: %s", data->tune->statusString());
        delete data->tune;
        data->tune = nullptr;
        return -1;
    }

    // Select subsong (0 = default starting song)
    data->tune->selectSong(subsong);

    // Configure the engine
    SidConfig cfg;
    cfg.frequency = FREQ;
    cfg.samplingMethod = SidConfig::INTERPOLATE;
    cfg.fastSampling = false;
    cfg.playback = SidConfig::STEREO;
    cfg.sidEmulation = data->builder;

    // Set default SID model based on tune info, or default to 6581
    const SidTuneInfo* info = data->tune->getInfo();
    if (info) {
        cfg.defaultSidModel
            = (info->sidModel(0) == SidTuneInfo::SIDMODEL_8580) ? SidConfig::MOS8580 : SidConfig::MOS6581;
        cfg.defaultC64Model = (info->clockSpeed() == SidTuneInfo::CLOCK_NTSC) ? SidConfig::NTSC : SidConfig::PAL;
    }

    if (!data->engine->config(cfg)) {
        rv_error("Failed to configure sidplayfp: %s", data->engine->error());
        delete data->tune;
        data->tune = nullptr;
        return -1;
    }

    // Load tune into engine
    if (!data->engine->load(data->tune)) {
        rv_error("Failed to load tune into engine: %s", data->engine->error());
        delete data->tune;
        data->tune = nullptr;
        return -1;
    }

    // Initialize mixer for stereo output
    data->engine->initMixer(true);

    // Detect number of SID chips used by this tune
    data->sid_count = info ? info->sidChips() : 1;
    if (data->sid_count < 1)
        data->sid_count = 1;
    if (data->sid_count > 3)
        data->sid_count = 3;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sidplayfp_close(void* user_data) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);

    if (data->tune) {
        data->engine->load(nullptr);
        delete data->tune;
        data->tune = nullptr;
    }

    delete[] data->song_data;
    data->song_data = nullptr;
    data->song_data_size = 0;

    data->pending_frames = 0;
    data->pending_read = 0;
    data->finished = false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult sidplayfp_probe_can_play(uint8_t* d, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)total_size;

    if (data_size < 4) {
        return RVProbeResult_Unsupported;
    }

    // Check for PSID/RSID header
    if ((d[0] == 'P' || d[0] == 'R') && d[1] == 'S' && d[2] == 'I' && d[3] == 'D') {
        return RVProbeResult_Supported;
    }

    // Reject Atari ST executables (TOS magic 0x601a) which share .prg extension
    if (data_size >= 2 && d[0] == 0x60 && d[1] == 0x1a) {
        return RVProbeResult_Unsupported;
    }

    // For .prg/.p00/.c64 files, return Unsure so the file-based loader can try them
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr) {
            dot++;
            char lower[4] = { 0 };
            for (int i = 0; i < 3 && dot[i]; i++) {
                lower[i] = (char)tolower((unsigned char)dot[i]);
            }
            if (strcmp(lower, "prg") == 0 || strcmp(lower, "p00") == 0 || strcmp(lower, "c64") == 0) {
                return RVProbeResult_Unsure;
            }
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Hands the host up to `max_frames` staged frames and keeps whatever is left.
static uint32_t drain_pending(SidPlayData* data, int16_t* output, uint32_t max_frames) {
    uint32_t available = data->pending_frames - data->pending_read;
    uint32_t taken = available < max_frames ? available : max_frames;

    memcpy(output, data->pending + data->pending_read * 2, taken * 2 * sizeof(int16_t));
    data->pending_read += taken;
    if (data->pending_read == data->pending_frames) {
        data->pending_frames = 0;
        data->pending_read = 0;
    }

    return taken;
}

static RVReadInfo sidplayfp_read_data(void* user_data, RVReadData dest) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, FREQ };

    if (!data->tune) {
        return RVReadInfo { format, 0, RVReadStatus_Error };
    }

    // The host advertises a byte capacity that may exceed what the requested frames
    // occupy; neither bound may be crossed.
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
    auto* output = static_cast<int16_t*>(dest.channels_output);

    if (max_frames == 0) {
        return RVReadInfo { format, 0, RVReadStatus_Ok };
    }

    // Surplus from an earlier batch comes first, so frames stay in order.
    if (data->pending_frames > data->pending_read) {
        uint32_t taken = drain_pending(data, output, max_frames);
        return RVReadInfo { format, taken, RVReadStatus_Ok };
    }

    if (data->finished) {
        return RVReadInfo { format, 0, RVReadStatus_Finished };
    }

    // Run emulator for one batch (play() caps at 20000 cycles internally)
    unsigned int cycles = max_frames * 21; // ~20.5 cycles/sample
    int samples = data->engine->play(cycles);

    if (samples < 0) {
        rv_error("sidplayfp playback error: %s", data->engine->error());
        return RVReadInfo { format, 0, RVReadStatus_Error };
    }

    if (samples == 0) {
        data->finished = true;
        return RVReadInfo { format, 0, RVReadStatus_Finished };
    }

    // play() resets the chip buffer, so the whole batch has to be mixed now even when it
    // overshoots the request. Mix into the staging buffer and hand out the request's share.
    if (static_cast<unsigned int>(samples) > PENDING_FRAMES_MAX) {
        samples = PENDING_FRAMES_MAX;
    }
    unsigned int mixed = data->engine->mix(data->pending, static_cast<unsigned int>(samples));
    data->pending_frames = mixed / 2;
    data->pending_read = 0;

    uint32_t taken = drain_pending(data, output, max_frames);

    return RVReadInfo { format, taken, RVReadStatus_Ok };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t sidplayfp_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    // Seeking not supported for SID files
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int sidplayfp_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    // Use filename-based constructor for PRG/P00/C64 format detection
    SidTune tune(url);

    if (!tune.getStatus()) {
        return -1;
    }

    const SidTuneInfo* info = tune.getInfo();
    if (!info) {
        return -1;
    }

    RVMetadataId index = rv_metadata_create_url(url);

    // Info strings: 0=title, 1=author, 2=released
    if (info->numberOfInfoStrings() > 0) {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, info->infoString(0));
    }
    if (info->numberOfInfoStrings() > 1) {
        rv_metadata_set_tag(index, RV_METADATA_ARTIST_TAG, info->infoString(1));
    }
    if (info->numberOfInfoStrings() > 2) {
        rv_metadata_set_tag(index, RV_METADATA_DATE_TAG, info->infoString(2));
    }

    // Format string (PSID, RSID, etc.)
    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, info->formatString());

    // SID model info - include in message
    const char* sid_model = "Unknown";
    switch (info->sidModel(0)) {
        case SidTuneInfo::SIDMODEL_6581:
            sid_model = "MOS 6581";
            break;
        case SidTuneInfo::SIDMODEL_8580:
            sid_model = "MOS 8580";
            break;
        case SidTuneInfo::SIDMODEL_ANY:
            sid_model = "Any SID";
            break;
        default:
            break;
    }

    rv_metadata_set_tag(index, RV_METADATA_MESSAGE_TAG, sid_model);

    // Length is not available in SID files themselves (would need HVSC Songlengths.md5)
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, 0.0);

    // Add subsongs if more than one
    unsigned int songs = info->songs();
    if (songs > 1) {
        for (unsigned int i = 1; i <= songs; i++) {
            tune.selectSong(i);
            const SidTuneInfo* sub_info = tune.getInfo();
            const char* sub_title = "";
            if (sub_info && sub_info->numberOfInfoStrings() > 0) {
                sub_title = sub_info->infoString(0);
            }
            rv_metadata_add_subsong(index, i, sub_title, 0.0f);
        }
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sidplayfp_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
    // Event reporting not implemented for SID
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sidplayfp_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Visualization: metadata-only model. sidplayfp exposes a per-voice scope but no
// pattern grid, so it advertises Scope only and leaves the pattern getters NULL.
// The scope channel count is dynamic (3 voices per SID) and exceeds the old
// RV_MAX_CHANNELS=8 cap for multi-SID tunes (up to 3 SIDs * 3 = 9 voices).

static bool sidplayfp_get_structure(void* user_data, RVVizInfo* out) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    if (data == nullptr || data->tune == nullptr || out == nullptr) {
        return false;
    }

    out->caps = RVVizCaps_Scope;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = 0;
    out->scope_channel_count = static_cast<uint32_t>(data->sid_count) * 3;
    out->column_count = 0;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t sidplayfp_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    if (out == nullptr) {
        return 0;
    }

    // Single SID: "Voice 1".."Voice 3"; multi-SID: "SID n Vm".
    static const char* s_single_names[] = { "Voice 1", "Voice 2", "Voice 3" };
    static const char* s_multi_names[] = {
        "SID 1 V1", "SID 1 V2", "SID 1 V3", "SID 2 V1", "SID 2 V2", "SID 2 V3", "SID 3 V1", "SID 3 V2", "SID 3 V3",
    };

    int sid_count = (data != nullptr) ? data->sid_count : 1;
    if (sid_count < 1)
        sid_count = 1;

    const char** src = (sid_count > 1) ? s_multi_names : s_single_names;
    uint32_t count = static_cast<uint32_t>(sid_count) * 3;
    if (count > cap)
        count = cap;

    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        strncpy(reinterpret_cast<char*>(out[i].name), src[i], sizeof(out[i].name) - 1);
        out[i].scope_width = 0;
    }

    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sidplayfp_set_scope_enabled(void* user_data, bool on) {
    // ReSIDfp captures per-voice buffers unconditionally; nothing to toggle.
    (void)user_data;
    (void)on;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t sidplayfp_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    if (data == nullptr || data->builder == nullptr || out == nullptr) {
        return 0;
    }

    return data->builder->getScopeData(channel, out, cap);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_sidplayfp_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "sidplayfp",
    "0.0.1",
    "libsidplayfp 2.16.0",
    sidplayfp_probe_can_play,
    sidplayfp_supported_extensions,
    sidplayfp_create,
    sidplayfp_destroy,
    sidplayfp_event,
    sidplayfp_open,
    sidplayfp_close,
    sidplayfp_read_data,
    sidplayfp_seek,
    sidplayfp_metadata,
    sidplayfp_static_init,
    nullptr, // settings_updated
    nullptr, // static_destroy

    // Visualization: metadata-only + scope (no pattern grid).
    sidplayfp_get_structure,
    nullptr, // get_columns
    nullptr, // get_pattern_channels
    sidplayfp_get_scope_channels,
    nullptr, // get_position
    nullptr, // get_channel_rows
    nullptr, // get_cells
    sidplayfp_set_scope_enabled,
    sidplayfp_get_scope_samples,
    nullptr, // get_vu
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_sidplayfp_plugin;
}
