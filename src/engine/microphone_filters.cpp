#include "microphone_filters.hpp"
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <memory>
#include <vector>

namespace feathercast::engine::filters {
namespace {
QJsonObject number(const char* key, const char* label, double value, double min, double max,
                   const char* suffix, bool integer = false) {
    return {{"key", key}, {"label", label}, {"default", value}, {"min", min}, {"max", max},
            {"suffix", suffix}, {"integer", integer}};
}
QString registeredId(const QString& base) {
    // Input-type lookup does not resolve filters. Prefer the current v2
    // suppression implementation, using only IDs actually registered at runtime.
    QSet<QString> ids;
    const char* id = nullptr;
    for (size_t i = 0; obs_enum_filter_types(i, &id); ++i) ids.insert(QString::fromUtf8(id));
    if (ids.contains(base + "_v2")) return base + "_v2";
    return ids.contains(base) ? base : QString{};
}
QJsonArray methods(const QString& id) {
    QJsonArray result;
    if (id.isEmpty()) return result;
    auto* props = obs_get_source_properties(id.toUtf8().constData());
    auto* method = props ? obs_properties_get(props, "method") : nullptr;
    if (method && obs_property_get_type(method) == OBS_PROPERTY_LIST) {
        for (size_t i = 0; i < obs_property_list_item_count(method); ++i) {
            const QString value = QString::fromUtf8(obs_property_list_item_string(method, i));
            if (!obs_property_list_item_disabled(method, i) && (value == "speex" || value == "rnnoise")) result.append(value);
        }
    } else if (props) {
        // Single-method builds omit the selector. A Speex level property and
        // the advertised default identify that build without guessing support.
        if (obs_properties_get(props, "suppress_level")) result.append("speex");
        auto* defaults = obs_get_source_defaults(id.toUtf8().constData());
        if (defaults && QString::fromUtf8(obs_data_get_string(defaults, "method")) == "rnnoise") result.append("rnnoise");
        obs_data_release(defaults);
    }
    obs_properties_destroy(props);
    return result;
}
QString nativeName(const QJsonObject& row) { return "FeatherCast filter " + row["id"].toString(); }
using Source = std::shared_ptr<obs_source_t>;
Source own(obs_source_t* source) { return Source(source, obs_source_release); }
}

QJsonArray catalog(bool runtime) {
    QJsonArray result;
    auto add = [&](const char* type, const char* label, const char* base, QJsonArray fields) {
        QJsonObject defaults;
        for (auto field : fields) { auto f = field.toObject(); defaults[f["key"].toString()] = f["default"]; }
        const auto id = runtime ? registeredId(base) : QString{};
        QJsonObject row{{"type", type}, {"name", label}, {"obs_id", id}, {"available", !id.isEmpty()},
                        {"fields", fields}, {"defaults", defaults}};
        if (QString(type) == "noise_suppression") {
            const auto supported = runtime ? methods(id) : QJsonArray{"speex", "rnnoise"};
            row["methods"] = supported;
            row["available"] = !id.isEmpty() && !supported.isEmpty();
            defaults["method"] = supported.contains("speex") ? "speex" : "rnnoise";
            row["defaults"] = defaults;
        }
        result.append(row);
    };
    add("compressor", "Compressor", "compressor_filter", {
        number("ratio", "Ratio", 10, 1, 32, " :1"), number("threshold", "Threshold", -18, -60, 0, " dB"),
        number("attack_time", "Attack", 6, 1, 500, " ms", true), number("release_time", "Release", 60, 1, 1000, " ms", true),
        number("output_gain", "Output gain", 0, -32, 32, " dB")});
    add("noise_gate", "Noise Gate", "noise_gate_filter", {
        number("close_threshold", "Close threshold", -32, -96, 0, " dB"), number("open_threshold", "Open threshold", -26, -96, 0, " dB"),
        number("attack_time", "Attack", 25, 0, 10000, " ms", true), number("hold_time", "Hold", 200, 0, 10000, " ms", true),
        number("release_time", "Release", 150, 0, 10000, " ms", true)});
    add("noise_suppression", "Noise Suppression", "noise_suppress_filter", {
        number("suppress_level", "Suppression level", -30, -60, 0, " dB", true)});
    return result;
}

QString normalize(QJsonArray& chain, bool runtime) {
    if (chain.size() > 32) return "A microphone can have at most 32 filters";
    QMap<QString, QJsonObject> types;
    for (auto value : catalog(runtime)) { auto type = value.toObject(); types.insert(type["type"].toString(), type); }
    QSet<QString> ids;
    static const QRegularExpression safeId("^[A-Za-z0-9_-]{1,64}$");
    for (qsizetype i = 0; i < chain.size(); ++i) {
        auto row = chain[i].toObject();
        const auto id = row["id"].toString(), name = row["name"].toString(), type = row["type"].toString();
        if (!chain[i].isObject() || !safeId.match(id).hasMatch() || ids.contains(id) || name.trimmed().isEmpty() ||
            name.size() > 128 || name.contains(QChar(0)) || !types.contains(type) || !row["enabled"].isBool() || !row["settings"].isObject())
            return "Invalid microphone filter (expected a unique ID, name, type, enabled state and settings)";
        for (auto it = row.begin(); it != row.end(); ++it)
            if (it.key() != "id" && it.key() != "name" && it.key() != "type" && it.key() != "enabled" && it.key() != "settings")
                return "Unknown microphone filter field: " + it.key();
        ids.insert(id);
        const auto spec = types[type];
        if (runtime && !spec["available"].toBool()) return spec["name"].toString() + " is unavailable in this OBS runtime";
        auto settings = spec["defaults"].toObject();
        const auto patch = row["settings"].toObject();
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (!settings.contains(it.key())) return "Unknown filter setting: " + it.key();
            settings[it.key()] = it.value();
        }
        for (auto field : spec["fields"].toArray()) {
            const auto f = field.toObject(); const auto value = settings[f["key"].toString()]; const double n = value.toDouble();
            if (!value.isDouble() || !std::isfinite(n) || n < f["min"].toDouble() || n > f["max"].toDouble() ||
                (f["integer"].toBool() && std::floor(n) != n)) return "Invalid filter setting: " + f["label"].toString();
        }
        if (type == "noise_suppression" && !spec["methods"].toArray().contains(settings["method"]))
            return "Noise suppression method is unavailable";
        if (type == "noise_gate" && settings["close_threshold"].toDouble() > settings["open_threshold"].toDouble())
            return "Noise Gate close threshold must not exceed its open threshold";
        row["settings"] = settings; chain[i] = row;
    }
    return {};
}

QString normalizeSource(const QString& type, QJsonObject& properties) {
    if (!properties.contains("filters")) return {};
    if (type != "microphone" || !properties["filters"].isArray()) return "Filters require a microphone source and an array";
    auto chain = properties["filters"].toArray();
    const auto error = normalize(chain, false);
    if (error.isEmpty()) properties["filters"] = chain;
    return error;
}

QString apply(obs_source_t* source, const QJsonArray& before, const QJsonArray& after) {
    if (before == after) return {};
    QMap<QString, QJsonObject> previous, specs;
    for (auto v : before) { auto row = v.toObject(); previous.insert(row["id"].toString(), row); }
    for (auto v : catalog(true)) { auto row = v.toObject(); specs.insert(row["type"].toString(), row); }
    struct Staged { Source source; QJsonObject row; bool added; };
    std::vector<Staged> staged;
    // Holding strong references also keeps detached objects alive until all
    // mutations finish. A failed creation leaves the old chain untouched.
    for (auto v : after) {
        auto row = v.toObject(); const auto old = previous.value(row["id"].toString());
        const auto spec = specs.value(row["type"].toString());
        // Optional modules/methods may disappear between installations. Keep
        // their saved rows dormant, never substitute a different DSP method.
        if (!spec["available"].toBool() || (row["type"] == "noise_suppression" &&
            !spec["methods"].toArray().contains(row["settings"].toObject()["method"]))) continue;
        Source filter;
        if (!old.isEmpty() && old["type"] == row["type"])
            filter = own(obs_source_get_filter_by_name(source, nativeName(row).toUtf8().constData()));
        const bool added = !filter;
        if (added) {
            auto settings = row["settings"].toObject();
            if (row["type"] == "compressor") settings["sidechain_source"] = "none";
            auto* data = obs_data_create_from_json(QJsonDocument(settings).toJson(QJsonDocument::Compact).constData());
            const auto id = specs.value(row["type"].toString())["obs_id"].toString().toUtf8();
            filter = own(obs_source_create_private(id.constData(), nativeName(row).toUtf8().constData(), data));
            obs_data_release(data);
            if (!filter || !obs_obj_get_data(filter.get())) return "Could not create microphone filter: " + row["name"].toString();
            obs_source_set_enabled(filter.get(), row["enabled"].toBool());
        }
        staged.push_back({std::move(filter), row, added});
    }
    for (auto v : before) {
        const auto row = v.toObject();
        auto old = own(obs_source_get_filter_by_name(source, nativeName(row).toUtf8().constData()));
        bool kept = false;
        for (const auto& entry : staged) if (entry.source == old) kept = true;
        if (old && !kept) obs_source_filter_remove(source, old.get());
    }
    for (const auto& entry : staged) {
        if (entry.added) obs_source_filter_add(source, entry.source.get());
        else {
            const auto old = previous.value(entry.row["id"].toString());
            if (old["settings"] != entry.row["settings"]) {
                auto* data = obs_data_create_from_json(QJsonDocument(entry.row["settings"].toObject()).toJson(QJsonDocument::Compact).constData());
                obs_source_update(entry.source.get(), data); obs_data_release(data);
            }
            if (old["enabled"] != entry.row["enabled"]) obs_source_set_enabled(entry.source.get(), entry.row["enabled"].toBool());
        }
    }
    // libobs enumerates and processes audio in reverse internal-index order.
    // Public/UI order is top-to-bottom processing order, including bypassed rows.
    for (size_t i = 0; i < staged.size(); ++i)
        obs_source_filter_set_index(source, staged[i].source.get(), staged.size() - 1 - i);
    return {};
}
}
