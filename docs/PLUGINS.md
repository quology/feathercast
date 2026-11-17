# FeatherCast plugin ABI v1

Include `feathercast/core/plugin_api.h` and export `fc_plugin_query`. Return null for unsupported host ABI versions. Returned descriptors/strings must remain valid until DLL unload. `initialize` receives a sized/versioned host function table and returns plugin-owned state; `shutdown` frees that state inside the same DLL.

The host checks descriptor sizes, ABI, non-null metadata/functions, unique namespaced IDs, bounded action count, and every action descriptor before initialization. Calls run on the controller thread. UTF-8 arguments are borrowed. Copy anything retained. No C++ objects, allocations, STL containers, exceptions or Qt widgets cross this ABI. A plugin cannot retain host pointers after shutdown.

Version 1 allows action registration and a host delay request. The sample `examples/delay_preset_plugin.cpp` exports `tournament.delay90`; invoking it calls `request_delay_ms(90000)`. The host must report failure when delay is unavailable or outside its configured limits. The example's arguments are ignored, because the preset is fixed.

Load only user-approved native binaries using an absolute path and safe DLL search settings. Before unloading, remove action references and stop/join plugin-owned work, then invoke shutdown. Native plugins are trusted in-process code; structure validation and C++ exception catches cannot contain access violations or malicious code. Future isolated plugins need a separate process protocol.

The `feathercast_example_plugin` CMake target builds the sample DLL. Unit tests call its descriptor directly; DLL discovery/loading must be verified by application integration. OBS source/encoder/output modules use libobs's separate ABI. OBS frontend plugins expecting the OBS Qt application are not automatically compatible.
