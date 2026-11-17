#ifndef FEATHERCAST_PLUGIN_API_H
#define FEATHERCAST_PLUGIN_API_H
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define FC_PLUGIN_EXPORT __declspec(dllexport)
#define FC_CALL __cdecl
#else
#define FC_PLUGIN_EXPORT __attribute__((visibility("default")))
#define FC_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define FC_PLUGIN_ABI_VERSION 1u

// UTF-8 strings, borrowed for the duration of the call. No allocation ownership
// crosses the ABI. All callbacks execute on the controller thread. No exceptions
// may cross this boundary. Host is alive until shutdown returns.
typedef struct fc_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    void (FC_CALL *log)(void* context, int level, const char* message);
    int (FC_CALL *request_delay_ms)(void* context, int64_t delay_ms);
} fc_host_api;

typedef struct fc_action_descriptor {
    uint32_t struct_size;
    const char* id;
    const char* display_name;
    int (FC_CALL *invoke)(void* plugin_context, const char* arguments_json);
} fc_action_descriptor;

typedef struct fc_plugin_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    // Return 1 on success. On failure leave *plugin_context null and clean up.
    int (FC_CALL *initialize)(const fc_host_api* host, void** plugin_context);
    void (FC_CALL *shutdown)(void* plugin_context);
    uint32_t action_count;
    const fc_action_descriptor* actions;
} fc_plugin_descriptor;

// Descriptor and strings live until DLL unload. Host validates sizes, version,
// pointers and a bounded action_count before initialization; quiesces callbacks
// before shutdown/unload. Trusted native plugins are not a security sandbox.
typedef const fc_plugin_descriptor* (FC_CALL *fc_plugin_query_fn)(uint32_t host_abi);
const fc_plugin_descriptor* FC_CALL fc_plugin_query(uint32_t host_abi);

#ifdef __cplusplus
}
#endif
#endif
