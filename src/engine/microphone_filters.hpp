#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <obs.h>

namespace feathercast::engine::filters {
// Arrays use audible processing order (first row processes first).
QJsonArray catalog(bool runtime);
QString normalize(QJsonArray& chain, bool runtime);
QString normalizeSource(const QString& type, QJsonObject& properties);
// Only engine-owned chains. Stages new instances before modifying the graph;
// unchanged instances survive edits so their DSP state is not reset.
QString apply(obs_source_t* source, const QJsonArray& before, const QJsonArray& after);
}
