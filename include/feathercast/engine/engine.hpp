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
    bool loadConfiguration(const QString& path);
    bool saveConfiguration(const QString& path) const;
    bool storeStreamKey(const QString& key);
    QString streamKey() const;
    bool hasStreamKey() const;

    QStringList scenes() const;
    QString currentScene() const;
    bool addScene(const QString& name);
    bool removeScene(const QString& name);
    bool selectScene(const QString& name);
    // Types: desktop, window, game, color, microphone, desktop_audio, test_tone.
    bool addSource(const QString& scene, const QString& type, const QString& name, QJsonObject properties = {});
    bool removeSource(const QString& scene, const QString& name);
    QJsonArray sources(const QString& scene) const;
    QJsonArray sourceOptions(const QString& type) const;
    void setSourceVisible(const QString& scene, const QString& name, bool visible);
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
