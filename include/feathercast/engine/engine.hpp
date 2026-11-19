#pragma once
#include "feathercast/core/automation.hpp"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <memory>

namespace feathercast::engine {
// All calls on the object's Qt thread; asynchronous OBS signals are queued.
class Engine final : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;
    // runtimeRoot contains bin/64bit, core and data/libobs. Empty uses adjacent obs/.
    bool initialize(QString runtimeRoot = {}, QJsonObject settings = {});
    void shutdown();
    bool initialized() const;
    static QJsonObject defaultSettings();
    QJsonObject settings() const;
    // Requires all outputs stopped; keeps scene graph while resetting video.
    bool applySettings(const QJsonObject& settings);
    bool loadConfiguration(const QString& path, const QJsonObject& legacySourceAudio = {});
    bool saveConfiguration(const QString& path) const;
    bool storeStreamKey(const QString& key);
    QString streamKey() const;
    bool hasStreamKey() const;

    QStringList scenes() const;
    QString currentScene() const;
    bool addScene(const QString& name);
    bool removeScene(const QString& name);
    bool selectScene(const QString& name);
    bool renameScene(const QString& name, const QString& replacement);
    bool duplicateScene(const QString& name, const QString& replacement);
    bool moveScene(const QString& name, int position);
    bool studioMode() const;
    void setStudioMode(bool enabled);
    QString programScene() const;
    QJsonArray transitionTypes() const;
    bool setTransition(const QString& type, int durationMs);
    bool takePreview(const QString& type = {}); // empty uses configured transition
    void attachProgram(quintptr nativeWindow, int width, int height);
    void resizeProgram(int width, int height);
    // Types: desktop, window, game, color, microphone, desktop_audio, application_audio, test_tone.
    // capture_audio enables window/game audio; desktop also requires audio_window.
    // separate_audio_track reserves recording_track 2..6, keeping audio in mix 1.
    bool addSource(const QString& scene, const QString& type, const QString& name, QJsonObject properties = {});
    bool removeSource(const QString& scene, const QString& name);
    QJsonArray sources(const QString& scene) const;
    QJsonArray sourceOptions(const QString& type) const;
    bool renameSource(const QString& scene, const QString& name, const QString& replacement);
    bool addGroup(const QString& scene, const QString& name);
    bool setSourceLocked(const QString& scene, const QString& name, bool locked);
    // Position is zero-based within siblings, bottom to top. Empty group means root.
    bool moveSource(const QString& scene, const QString& name, const QString& group, int position);
    QJsonObject sourceProperties(const QString& scene, const QString& name) const;
    QJsonArray sourcePropertySchema(const QString& scene, const QString& name) const;
    bool setSourceProperties(const QString& scene, const QString& name, const QJsonObject& patch);
    // Parent-local native coordinates. OBS may rebase group/child origins while
    // retaining world placement. Snapshots and edits are scene-atomic.
    QJsonObject sourceTransform(const QString& scene, const QString& name) const;
    // Current native draw-matrix composition: m11,m12,m21,m22,dx,dy, composing the item's
    // draw matrix through all groups. Pending edits settle on the next video tick.
    QJsonObject sourceWorldTransform(const QString& scene, const QString& name) const;
    bool setSourceTransform(const QString& scene, const QString& name, const QJsonObject& patch);
    bool sourceAudioAvailable(const QString& type) const;
    void setSourceVisible(const QString& scene, const QString& name, bool visible);
    // Each row contains name, scene, active and the canonical audio properties.
    QJsonArray audioSources() const;
    QJsonObject audioProperties(const QString& name) const;
    bool setAudioProperties(const QString& name, const QJsonObject& patch);
    // Opt-in microphone-only filters, in audible processing order. Each row has
    // id (stable unique token), name, type, enabled and settings. Catalog includes
    // runtime availability, supported suppression methods, defaults and ranges.
    QJsonArray microphoneFilterTypes() const;
    bool isMicrophone(const QString& name) const;
    QJsonArray microphoneFilters(const QString& name) const;
    bool setMicrophoneFilters(const QString& name, QJsonArray filters);
    QJsonArray videoFilterTypes() const;
    QJsonArray videoFilters(const QString& scene, const QString& name) const;
    bool setVideoFilters(const QString& scene, const QString& name, QJsonArray filters);
    bool audioMonitoringAvailable() const;
    bool recordingActive() const;
    void setSourceAudio(const QString& name, double volume, bool muted);
    void attachPreview(quintptr nativeWindow, int width, int height);
    void resizePreview(int width, int height);
    void setPreviewEnabled(bool enabled);

    bool startStreaming(QString endpoint = {}); // endpoint empty uses settings + protected key
    void stopStreaming();
    bool startRecording(QString filePath = {});
    void stopRecording();
    bool startReplay();
    void stopReplay();
    bool saveReplay();
    bool setDelayMs(qint64 milliseconds);
    QJsonObject statistics() const;
    core::AutomationSnapshot automationSnapshot() const;

    bool loadPlugin(const QString& absolutePath);
    QJsonArray pluginActions() const;
    bool invokePluginAction(const QString& id, const QString& argumentsJson = "{}");
signals:
    void errorOccurred(const QString& message);
    void logMessage(const QString& message);
    void stateChanged();
    void scenesChanged();
    void replaySaved(const QString& path);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace feathercast::engine
