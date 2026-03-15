///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// libsidplayfp Playback Plugin
//
// Implements RVPlaybackPlugin interface for Commodore 64 SID music formats.
// Based on libsidplayfp by Leandro Nini.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <retrovert/io.h>
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

#include <cstdlib>
#include <cstring>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define FREQ 48000

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

struct SidPlayData {
    sidplayfp* engine;
    SidTune* tune;
    ReSIDfpBuilder* builder;
    uint8_t* song_data;
    uint32_t song_data_size;
    int sid_count; // Number of SID chips used by current tune (1-3)
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

static int sidplayfp_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("Failed to load %s to memory", url);
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

    // Keep a copy of the song data (SidTune doesn't copy it)
    data->song_data_size = static_cast<uint32_t>(read_res.data_size);
    data->song_data = new uint8_t[data->song_data_size];
    memcpy(data->song_data, read_res.data, data->song_data_size);

    rv_io_free_url_to_memory(read_res.data);

    // Load tune from memory
    data->tune = new SidTune(data->song_data, data->song_data_size);

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
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult sidplayfp_probe_can_play(uint8_t* d, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)url;
    (void)total_size;

    if (data_size < 4) {
        return RVProbeResult_Unsupported;
    }

    // Check for PSID/RSID header
    if ((d[0] == 'P' || d[0] == 'R') && d[1] == 'S' && d[2] == 'I' && d[3] == 'D') {
        return RVProbeResult_Supported;
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo sidplayfp_read_data(void* user_data, RVReadData dest) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, FREQ };

    if (!data->tune) {
        return RVReadInfo { format, 0, RVReadStatus_Error };
    }

    // Run emulator for one batch (play() caps at 20000 cycles internally)
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    unsigned int cycles = max_frames * 21; // ~20.5 cycles/sample
    int samples = data->engine->play(cycles);

    if (samples < 0) {
        rv_error("sidplayfp playback error: %s", data->engine->error());
        return RVReadInfo { format, 0, RVReadStatus_Error };
    }

    if (samples == 0) {
        return RVReadInfo { format, 0, RVReadStatus_Finished };
    }

    // Mix S16 stereo directly to output buffer
    auto* output = static_cast<int16_t*>(dest.channels_output);
    unsigned int mixed = data->engine->mix(output, static_cast<unsigned int>(samples));

    return RVReadInfo { format, static_cast<uint16_t>(mixed / 2), RVReadStatus_Ok };
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

    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("Failed to load %s to memory", url);
        return -1;
    }

    SidTune tune(static_cast<const uint8_t*>(read_res.data), static_cast<uint32_t>(read_res.data_size));

    if (!tune.getStatus()) {
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    const SidTuneInfo* info = tune.getInfo();
    if (!info) {
        rv_io_free_url_to_memory(read_res.data);
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

    rv_io_free_url_to_memory(read_res.data);
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
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t sidplayfp_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);
    if (data == nullptr || data->builder == nullptr || buffer == nullptr) {
        return 0;
    }

    return data->builder->getScopeData(channel, buffer, num_samples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t sidplayfp_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);

    // Single SID: "Voice 1", "Voice 2", "Voice 3"
    static const char* s_single_names[] = { "Voice 1", "Voice 2", "Voice 3" };

    // Multi SID: "SID 1 V1" .. "SID 3 V3"
    static const char* s_multi_names[] = {
        "SID 1 V1", "SID 1 V2", "SID 1 V3", "SID 2 V1", "SID 2 V2", "SID 2 V3", "SID 3 V1", "SID 3 V2", "SID 3 V3",
    };

    int sid_count = (data != nullptr) ? data->sid_count : 1;
    if (sid_count < 1)
        sid_count = 1;

    const char** src = (sid_count > 1) ? s_multi_names : s_single_names;
    uint32_t count = static_cast<uint32_t>(sid_count) * 3;

    if (count > max_channels)
        count = max_channels;

    for (uint32_t i = 0; i < count; i++)
        names[i] = src[i];

    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int sidplayfp_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    SidPlayData* data = static_cast<SidPlayData*>(user_data);

    if (data == nullptr || data->tune == nullptr || info == nullptr) {
        return -1;
    }

    memset(info, 0, sizeof(RVTrackerInfo));

    const SidTuneInfo* tune_info = data->tune->getInfo();
    if (tune_info == nullptr) {
        return -1;
    }

    // Info strings: 0=title, 1=author, 2=released
    if (tune_info->numberOfInfoStrings() > 0 && tune_info->infoString(0)[0] != '\0') {
        strncpy(info->song_name, tune_info->infoString(0), sizeof(info->song_name) - 1);
    }
    if (tune_info->numberOfInfoStrings() > 1 && tune_info->infoString(1)[0] != '\0') {
        strncpy(info->artist_name, tune_info->infoString(1), sizeof(info->artist_name) - 1);
    }
    if (tune_info->numberOfInfoStrings() > 2 && tune_info->infoString(2)[0] != '\0') {
        strncpy(info->game_name, tune_info->infoString(2), sizeof(info->game_name) - 1);
    }

    info->num_channels = data->sid_count * 3;

    return 0;
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

    // Tracker visualization API
    sidplayfp_get_tracker_info,
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    sidplayfp_get_scope_data,
    nullptr, // static_destroy
    sidplayfp_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_sidplayfp_plugin;
}
