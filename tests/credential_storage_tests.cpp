#include "feathercast/engine/engine.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto applicationName =
      "CredentialStorageTest-" + QUuid::createUuid().toString(QUuid::Id128);
  QCoreApplication::setOrganizationName("FeatherCastValidation");
  QCoreApplication::setApplicationName(applicationName);
  QStandardPaths::setTestModeEnabled(true);
  const auto directory =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  const auto isolatedSuffix = "/FeatherCastValidation/" + applicationName;
  if (!QDir::fromNativeSeparators(directory).endsWith(isolatedSuffix))
    return 1;
  const auto vaultPath = directory + "/stream-key.dpapi";
  // Never remove an existing file, even in this randomly named test namespace.
  if (QFile::exists(vaultPath))
    return 2;
  struct Cleanup {
    QString path;
    ~Cleanup() { QFile::remove(path); }
  } cleanup{vaultPath};
  auto require = [](bool passed, const char *check) {
    if (!passed)
      std::cerr << "Credential storage check failed: " << check << '\n';
    return passed;
  };

  feathercast::engine::Engine engine;
  const QString fakeKey = "PUBLIC-FAKE-TEST-KEY-NOT-A-CREDENTIAL-123456789";
  if (!require(!engine.initialized() && !engine.hasStreamKey(), "empty vault") ||
      !require(engine.storeStreamKey(fakeKey), "protect and store") ||
      !require(engine.hasStreamKey() && engine.streamKey() == fakeKey,
               "application roundtrip"))
    return 3;
  feathercast::engine::Engine reader;
  if (!require(reader.streamKey() == fakeKey, "new instance reads stored key"))
    return 4;
  QFile vault(vaultPath);
  if (!require(vault.open(QIODevice::ReadOnly), "vault exists"))
    return 5;
  const auto protectedBytes = vault.readAll();
  vault.close();
  if (!require(!protectedBytes.isEmpty() &&
                   !protectedBytes.contains(fakeKey.toUtf8()) &&
                   !protectedBytes.contains(QByteArray(
                       reinterpret_cast<const char *>(fakeKey.utf16()),
                       fakeKey.size() * 2)),
               "vault is not plaintext"))
    return 6;
  QTemporaryDir temporary;
  const auto exportPath = temporary.filePath("configuration.json");
  if (!require(temporary.isValid() && engine.saveConfiguration(exportPath),
               "configuration export"))
    return 7;
  QFile exported(exportPath);
  if (!require(exported.open(QIODevice::ReadOnly), "read export"))
    return 8;
  const auto json = exported.readAll();
  if (!require(QJsonDocument::fromJson(json).isObject() &&
                   !json.contains(fakeKey.toUtf8()) &&
                   !json.toLower().contains("streamkey") &&
                   !json.toLower().contains("stream-key"),
               "export excludes credentials"))
    return 9;
  if (!require(engine.storeStreamKey({}) && !engine.hasStreamKey() &&
                   engine.streamKey().isEmpty() && reader.streamKey().isEmpty(),
               "empty key clears stored credential") ||
      !require(!engine.initialized(), "no capture initialization"))
    return 10;
  std::cout << "Application credential roundtrip, encrypted storage, export exclusion, "
               "and empty-key clearing passed\n";
  return 0;
}
