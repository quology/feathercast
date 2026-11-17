# FeatherCast user guide

FeatherCast opens with a safe color scene and does not start capture or outputs automatically. Add scenes and sources with the + buttons. Display, window, game and audio devices are activated only when explicitly added. Select a source and choose Audio to change its volume or mute it; meters are not yet available.

Configure the base streaming endpoint and stream key in Settings. The key is protected separately by Windows DPAPI. Choose an absolute output folder. Start streaming, recording and replay independently; Save replay is available while the buffer is active. Streaming status distinguishes connection from starting or reconnecting.

Apply delay in the Stream delay dock. Increasing delay holds output and may cause the ingest server to disconnect; decreasing delay cuts content. The displayed effective delay is approximate and dropped packets include intentional cuts. Dock visibility and layout are saved on exit. Plugins are trusted in-process code and must be selected explicitly. Global hotkeys and the scene automation editor are not yet available.
