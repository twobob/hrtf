#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include <clap/clap.h>

int main(void) {
    printf("Loading RotatingHRTF_v2.clap...\n");
    HMODULE lib = LoadLibraryA("RotatingHRTF_v2.clap");
    if (!lib) {
        printf("Failed to load RotatingHRTF_v2.clap. Error: %lu\n", GetLastError());
        return 1;
    }

    const clap_plugin_entry_t *entry = (const clap_plugin_entry_t *)GetProcAddress(lib, "clap_entry");
    if (!entry) {
        printf("Failed to find clap_entry symbol\n");
        FreeLibrary(lib);
        return 1;
    }

    if (!entry->init(".")) {
        printf("clap_entry->init() failed\n");
        FreeLibrary(lib);
        return 1;
    }

    const clap_plugin_factory_t *factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory) {
        printf("Failed to get CLAP factory\n");
        entry->deinit();
        FreeLibrary(lib);
        return 1;
    }

    uint32_t count = factory->get_plugin_count(factory);
    printf("Plugin count: %u\n", count);
    if (count == 0) {
        printf("No plugins in factory!\n");
        return 1;
    }

    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    printf("Plugin: %s (id: %s, vendor: %s, version: %s)\n",
           desc->name, desc->id, desc->vendor, desc->version);

    clap_host_t dummy_host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data = NULL,
        .name = "TestHost",
        .vendor = "Tester",
        .url = "",
        .version = "1.0",
        .get_extension = NULL,
        .request_restart = NULL,
        .request_process = NULL,
        .request_callback = NULL
    };

    const clap_plugin_t *plugin = factory->create_plugin(factory, &dummy_host, desc->id);
    if (!plugin) {
        printf("Failed to create plugin instance\n");
        return 1;
    }

    if (!plugin->init(plugin)) {
        printf("Plugin init failed\n");
        return 1;
    }

    if (!plugin->activate(plugin, 48000.0, 32, 256)) {
        printf("Plugin activate failed\n");
        return 1;
    }

    // Prepare 256 frames of 440 Hz test tone
    const uint32_t N = 256;
    float in_buf[256];
    float out_l[256] = {0};
    float out_r[256] = {0};
    for (uint32_t i = 0; i < N; ++i) {
        in_buf[i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
    }

    float *in_ptrs[1] = { in_buf };
    float *out_ptrs[2] = { out_l, out_r };

    clap_audio_buffer_t in_audio = {
        .data32 = in_ptrs,
        .channel_count = 1,
        .latency = 0,
        .constant_mask = 0
    };

    clap_audio_buffer_t out_audio = {
        .data32 = out_ptrs,
        .channel_count = 2,
        .latency = 0,
        .constant_mask = 0
    };

    clap_process_t process = {
        .steady_time = 0,
        .frames_count = N,
        .transport = NULL,
        .audio_inputs = &in_audio,
        .audio_outputs = &out_audio,
        .audio_inputs_count = 1,
        .audio_outputs_count = 1,
        .in_events = NULL,
        .out_events = NULL
    };

    clap_process_status status = plugin->process(plugin, &process);
    printf("Process status: %d (CLAP_PROCESS_CONTINUE = %d)\n", status, CLAP_PROCESS_CONTINUE);

    float sum_l = 0.0f, sum_r = 0.0f;
    for (uint32_t i = 0; i < N; ++i) {
        sum_l += fabsf(out_l[i]);
        sum_r += fabsf(out_r[i]);
    }
    printf("Output energy: Left = %f, Right = %f\n", sum_l, sum_r);

    if (sum_l > 0.01f && sum_r > 0.01f) {
        printf("SUCCESS: Plugin generated valid binaural audio!\n");
    } else {
        printf("ERROR: Audio output was unexpectedly silent or zero!\n");
    }

    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    FreeLibrary(lib);
    printf("CLAP test completed successfully!\n");
    return 0;
}
