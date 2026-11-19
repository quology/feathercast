// Included after Impl, keeping its OBS ownership private to engine.cpp.
namespace {
QJsonObject readTransform(obs_sceneitem_t* item) {
    obs_transform_info t{}; obs_sceneitem_get_info2(item, &t);
    obs_sceneitem_crop c{}; obs_sceneitem_get_crop(item, &c);
    return {{"x", t.pos.x}, {"y", t.pos.y}, {"rotation", t.rot}, {"scaleX", t.scale.x}, {"scaleY", t.scale.y},
        {"boundsType", int(t.bounds_type)}, {"boundsWidth", t.bounds.x}, {"boundsHeight", t.bounds.y},
        {"alignment", int(t.alignment)}, {"boundsAlignment", int(t.bounds_alignment)},
        {"cropLeft", c.left}, {"cropTop", c.top}, {"cropRight", c.right}, {"cropBottom", c.bottom}};
}
bool validTransform(const QJsonObject& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto k = it.key(); const double v = it.value().toDouble();
        if (!it.value().isDouble() || !std::isfinite(v)) return false;
        if (k == "x" || k == "y" || k == "rotation") { if (std::abs(v) > 100000) return false; }
        else if (k == "scaleX" || k == "scaleY") { if (std::abs(v) < .001 || std::abs(v) > 1000) return false; }
        else if (k == "boundsWidth" || k == "boundsHeight") { if (v < 0 || v > 100000) return false; }
        else if (k == "boundsType") { if (v != int(v) || v < int(OBS_BOUNDS_NONE) || v > int(OBS_BOUNDS_MAX_ONLY)) return false; }
        else if (k == "alignment" || k == "boundsAlignment") { if (v != int(v) || v < 0 || v > 15) return false; }
        else if (k == "cropLeft" || k == "cropTop" || k == "cropRight" || k == "cropBottom") { if (v != int(v) || v < 0 || v > 32768) return false; }
        else return false;
    }
    return true;
}
void writeTransform(obs_sceneitem_t* item, const QJsonObject& patch) {
    auto v = readTransform(item); for (auto it = patch.begin(); it != patch.end(); ++it) v[it.key()] = it.value();
    obs_transform_info t{}; obs_sceneitem_get_info2(item, &t);
    t.pos = {float(v["x"].toDouble()), float(v["y"].toDouble())}; t.rot = float(v["rotation"].toDouble());
    t.scale = {float(v["scaleX"].toDouble()), float(v["scaleY"].toDouble())};
    t.bounds = {float(v["boundsWidth"].toDouble()), float(v["boundsHeight"].toDouble())};
    t.bounds_type = obs_bounds_type(v["boundsType"].toInt()); t.alignment = v["alignment"].toInt(); t.bounds_alignment = v["boundsAlignment"].toInt();
    obs_sceneitem_crop c{v["cropLeft"].toInt(), v["cropTop"].toInt(), v["cropRight"].toInt(), v["cropBottom"].toInt()};
    obs_sceneitem_defer_update_begin(item); obs_sceneitem_set_info2(item, &t); obs_sceneitem_set_crop(item, &c); obs_sceneitem_defer_update_end(item);
}
QJsonArray propertySchema(obs_properties_t* properties) {
    QJsonArray result;
    for (auto* p = obs_properties_first(properties); p; obs_property_next(&p)) {
        const QString key = QString::fromUtf8(obs_property_name(p));
        // Audio topology is chosen at source creation; the audio API owns routing.
        if (key == "capture_audio" || !obs_property_visible(p) || !obs_property_enabled(p)) continue;
        QJsonObject row{{"key", key}, {"label", QString::fromUtf8(obs_property_description(p))}};
        switch (obs_property_get_type(p)) {
        case OBS_PROPERTY_BOOL: row["type"] = "bool"; break;
        case OBS_PROPERTY_INT: row["type"] = "int"; row["min"] = obs_property_int_min(p); row["max"] = obs_property_int_max(p); row["step"] = obs_property_int_step(p); break;
        case OBS_PROPERTY_FLOAT: row["type"] = "float"; row["min"] = obs_property_float_min(p); row["max"] = obs_property_float_max(p); row["step"] = obs_property_float_step(p); break;
        case OBS_PROPERTY_COLOR: case OBS_PROPERTY_COLOR_ALPHA: row["type"] = "color"; break;
        case OBS_PROPERTY_LIST: {
            row["type"] = "list"; QJsonArray options;
            for (size_t i = 0; i < obs_property_list_item_count(p); ++i) {
                if (obs_property_list_item_disabled(p, i)) continue;
                QJsonValue value;
                switch (obs_property_list_format(p)) {
                case OBS_COMBO_FORMAT_INT: value = double(obs_property_list_item_int(p, i)); break;
                case OBS_COMBO_FORMAT_FLOAT: value = obs_property_list_item_float(p, i); break;
                case OBS_COMBO_FORMAT_BOOL: value = obs_property_list_item_bool(p, i); break;
                default: value = QString::fromUtf8(obs_property_list_item_string(p, i)); break;
                }
                options.append(QJsonObject{{"label", QString::fromUtf8(obs_property_list_item_name(p, i))}, {"value", value}});
            }
            row["options"] = options; break;
        }
        default: continue; // No buttons, paths, passwords or arbitrary opaque text.
        }
        result.append(row);
    }
    return result;
}
bool validateProperties(const QJsonArray& schema, const QJsonObject& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        bool valid = false;
        for (auto value : schema) {
            const auto field = value.toObject(); if (field["key"] != it.key()) continue;
            const auto type = field["type"].toString(); const auto v = it.value();
            if (type == "bool") valid = v.isBool();
            else if (type == "int" || type == "float") valid = v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= field["min"].toDouble() && v.toDouble() <= field["max"].toDouble() && (type != "int" || v.toDouble() == std::floor(v.toDouble()));
            else if (type == "color") valid = v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= 4294967295.0 && v.toDouble() == std::floor(v.toDouble());
            else if (type == "list") for (auto option : field["options"].toArray()) if (option.toObject()["value"] == v) valid = true;
        }
        if (!valid) return false;
    }
    return true;
}
}
bool Engine::renameScene(const QString& name, const QString& replacement) {
    if (!d->scenes.contains(name) || replacement.trimmed().isEmpty() || replacement.size() > 128 || replacement.contains(QChar(0))) return false;
    if (name == replacement) return true;
    if (auto* existing = obs_get_source_by_name(replacement.toUtf8().constData())) { obs_source_release(existing); return d->fail("Names must be unique"); }
    auto scene = d->scenes.take(name); obs_source_set_name(obs_scene_get_source(scene.scene), replacement.toUtf8().constData()); d->scenes.insert(replacement, scene);
    d->sceneOrder[d->sceneOrder.indexOf(name)] = replacement;
    if (d->selected == name) d->selected = replacement;
    if (d->program == name) d->program = replacement;
    emit scenesChanged(); emit stateChanged(); return true;
}
bool Engine::moveScene(const QString& name, int position) {
    const int old = d->sceneOrder.indexOf(name); if (old < 0 || position < 0 || position >= d->sceneOrder.size()) return false;
    d->sceneOrder.move(old, position); emit scenesChanged(); return true;
}
bool Engine::duplicateScene(const QString& name, const QString& replacement) {
    if (!d->scenes.contains(name)) return false;
    const auto original = sources(name);
    if (!addScene(replacement)) return false;
    QMap<QString, QString> names;
    for (auto value : original) {
        auto row = value.toObject(); auto properties = row["properties"].toObject();
        if (properties.value("audio").toObject().value("scope") == "global") {
            // Global inputs already cover the duplicate scene. A second capture
            // would double the mix even before the duplicate is selected.
            if (row["type"] != "desktop") continue;
            properties["capture_audio"] = false; properties.remove("audio"); properties.remove("audio_window");
        }
        QString candidate = replacement.left(50) + " / " + row["name"].toString().left(60); int n = 2;
        for (;;) { auto* source = obs_get_source_by_name(candidate.toUtf8().constData()); if (!source) break; obs_source_release(source); candidate = replacement.left(50) + " / " + row["name"].toString().left(60) + QString(" %1").arg(n++); }
        names[row["name"].toString()] = candidate;
        if (!addSource(replacement, row["type"].toString(), candidate, properties)) { removeScene(replacement); return false; }
    }
    for (auto value : original) {
        auto row = value.toObject(); if (!names.contains(row["name"].toString())) continue; auto target = names[row["name"].toString()];
        if (!row["parent"].toString().isEmpty()) moveSource(replacement, target, names[row["parent"].toString()], 1000);
        setSourceTransform(replacement, target, row["transform"].toObject()); setSourceVisible(replacement, target, row["visible"].toBool(true));
    }
    for (auto value : original) { auto row = value.toObject(); if (names.contains(row["name"].toString())) setSourceLocked(replacement, names[row["name"].toString()], row["locked"].toBool()); }
    return true;
}
bool Engine::studioMode() const { return d->config["studioMode"].toBool(); }
QString Engine::programScene() const { return d->program; }
void Engine::setStudioMode(bool enabled) {
    d->config["studioMode"] = enabled; d->previewSeparate = enabled;
    if (!enabled && d->ready && !d->selected.isEmpty()) takePreview("cut");
    emit stateChanged();
}
QJsonArray Engine::transitionTypes() const { return {{QJsonObject{{"id", "cut"}, {"name", "Cut"}}}, {QJsonObject{{"id", "fade"}, {"name", "Fade"}}}}; }
bool Engine::setTransition(const QString& type, int durationMs) {
    if ((type != "cut" && type != "fade") || durationMs < 50 || durationMs > 20000) return d->fail("Invalid transition or duration (50–20000 ms)");
    d->config["transition"] = type; d->config["transitionDurationMs"] = durationMs; emit stateChanged(); return true;
}
bool Engine::takePreview(const QString& requested) {
    if (!d->ready || !d->scenes.contains(d->selected) || !d->transition) return false;
    const auto type = requested.isEmpty() ? d->config["transition"].toString() : requested;
    if (type != "cut" && type != "fade") return false;
    if (obs_transition_is_active(d->transition) && type != "cut") return d->fail("Wait for the current transition to finish");
    auto* source = obs_scene_get_source(d->scenes[d->selected].scene);
    if (type == "cut" || d->program.isEmpty()) obs_transition_set(d->transition, source);
    else if (d->program != d->selected && !obs_transition_start(d->transition, OBS_TRANSITION_MODE_AUTO, d->config["transitionDurationMs"].toInt(), source)) return d->fail("Transition could not start");
    obs_set_output_source(0, d->transition); d->program = d->selected;
    emit stateChanged(); return true;
}
void Engine::attachProgram(quintptr nativeWindow, int width, int height) {
    d->detachProgram(); if (!d->ready || !nativeWindow) return;
    gs_init_data init{}; init.window.hwnd = reinterpret_cast<void*>(nativeWindow); init.cx = std::max(1, width); init.cy = std::max(1, height); init.format = GS_BGRA; init.zsformat = GS_ZS_NONE; init.num_backbuffers = 1;
    d->programDisplay = obs_display_create(&init, 0xff161616);
    if (d->programDisplay) obs_display_add_draw_callback(d->programDisplay, Impl::drawProgram, d.get()); else d->fail("Could not create native program display");
}
void Engine::resizeProgram(int width, int height) { if (d->programDisplay) obs_display_resize(d->programDisplay, std::max(1, width), std::max(1, height)); }
bool Engine::addGroup(const QString& scene, const QString& name) {
    auto found = d->scenes.find(scene);
    if (found == d->scenes.end() || name.trimmed().isEmpty() || name.size() > 128 || name.contains(QChar(0)) || found->sources.size() >= 64) return false;
    if (auto* existing = obs_get_source_by_name(name.toUtf8().constData())) { obs_source_release(existing); return d->fail("Names must be unique"); }
    auto* group = obs_scene_add_group(found->scene, name.toUtf8().constData()); if (!group) return d->fail("Could not create group");
    found->sources.append(QJsonObject{{"name", name}, {"type", "group"}, {"properties", QJsonObject{}}, {"parent", ""}, {"locked", false}, {"visible", true}});
    emit scenesChanged(); return true;
}
bool Engine::renameSource(const QString& scene, const QString& name, const QString& replacement) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end() || d->locked(*found, name)) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item || replacement.trimmed().isEmpty() || replacement.size() > 128 || replacement.contains(QChar(0))) return false;
    if (name == replacement) return true;
    if (auto* existing = obs_get_source_by_name(replacement.toUtf8().constData())) { obs_source_release(existing); return d->fail("Names must be unique"); }
    obs_source_set_name(obs_sceneitem_get_source(item), replacement.toUtf8().constData());
    for (int i = 0; i < found->sources.size(); ++i) { auto row = found->sources[i].toObject(); if (row["name"] == name) row["name"] = replacement; if (row["parent"] == name) row["parent"] = replacement; found->sources[i] = row; }
    if (d->globalItems.contains(name)) d->globalItems.insert(replacement, d->globalItems.take(name));
    if (d->audioOwners.contains(name)) d->audioOwners.insert(replacement, d->audioOwners.take(name));
    if (d->audioCompanions.contains(name)) d->audioCompanions.insert(replacement, d->audioCompanions.take(name));
    auto node = d->meters.extract(name); if (!node.empty()) { node.key() = replacement; d->meters.insert(std::move(node)); }
    emit scenesChanged(); return true;
}
bool Engine::setSourceLocked(const QString& scene, const QString& name, bool locked) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item) return false;
    obs_sceneitem_set_locked(item, locked); const int i = d->row(*found, name); auto row = found->sources[i].toObject(); row["locked"] = locked; found->sources[i] = row; emit scenesChanged(); return true;
}
bool Engine::moveSource(const QString& scene, const QString& name, const QString& group, int position) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end() || position < 0 || name == group || d->locked(*found, name) || d->locked(*found, group)) return false;
    auto* item = publicSceneItem(found->scene, name); auto* destination = group.isEmpty() ? nullptr : publicSceneItem(found->scene, group);
    if (!item || (!group.isEmpty() && (!destination || !obs_sceneitem_is_group(destination)))) return false;
    QStringList path; QString ancestor = group;
    while (!ancestor.isEmpty()) { if (ancestor == name) return d->fail("A group cannot contain itself"); path.prepend(ancestor); ancestor = found->sources[d->row(*found, ancestor)].toObject()["parent"].toString(); }
    const auto relocate = [&](obs_sceneitem_t* target) {
        QString parent = found->sources[d->row(*found, name)].toObject()["parent"].toString();
        while (!parent.isEmpty()) { obs_sceneitem_group_remove_item(publicSceneItem(found->scene, parent), target); parent = found->sources[d->row(*found, parent)].toObject()["parent"].toString(); }
        for (const auto& step : path) obs_sceneitem_group_add_item(publicSceneItem(found->scene, step), target);
    };
    if (found->sources[d->row(*found, name)].toObject()["parent"].toString() != group) {
        relocate(item); if (auto* companion = d->audioCompanions.value(name)) relocate(companion);
    }
    const int index = d->row(*found, name); auto entry = found->sources.takeAt(index).toObject(); entry["parent"] = group;
    int insertion = found->sources.size(), count = 0;
    for (int i = 0; i < found->sources.size(); ++i) if (found->sources[i].toObject()["parent"].toString() == group && count++ == position) { insertion = i; break; }
    found->sources.insert(insertion, entry);
    // Apply public bottom-to-top order, keeping private audio adjacent to its owner.
    int nativePosition = 0;
    for (auto value : found->sources) { auto row = value.toObject(); if (row["parent"].toString() != group) continue;
        auto* sibling = publicSceneItem(found->scene, row["name"].toString()); obs_sceneitem_set_order_position(sibling, nativePosition++);
        if (auto* companion = d->audioCompanions.value(row["name"].toString())) obs_sceneitem_set_order_position(companion, nativePosition++);
    }
    emit scenesChanged(); return true;
}
QJsonObject Engine::sourceTransform(const QString& scene, const QString& name) const {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return {};
    auto* item = publicSceneItem(found->scene, name); return item ? readTransform(item) : QJsonObject{};
}
bool Engine::setSourceTransform(const QString& scene, const QString& name, const QJsonObject& patch) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end() || d->locked(*found, name) || !validTransform(patch)) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item) return false;
    writeTransform(item, patch); emit scenesChanged(); return true;
}
QJsonObject Engine::sourceProperties(const QString& scene, const QString& name) const {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return {};
    const int i = d->row(*found, name); if (i < 0) return {};
    auto* item = publicSceneItem(found->scene, name); auto* settings = obs_source_get_settings(obs_sceneitem_get_source(item));
    auto* defaults = obs_data_get_defaults(settings);
    auto* merged = obs_data_create(); obs_data_apply(merged, defaults); obs_data_apply(merged, settings); obs_data_release(defaults); obs_data_release(settings); settings = merged;
    auto result = QJsonDocument::fromJson(obs_data_get_json(settings)).object(); obs_data_release(settings);
    const auto stored = found->sources[i].toObject()["properties"].toObject(); for (auto it = stored.begin(); it != stored.end(); ++it) result[it.key()] = it.value(); return result;
}
QJsonArray Engine::sourcePropertySchema(const QString& scene, const QString& name) const {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return {};
    auto* item = publicSceneItem(found->scene, name); if (!item || obs_sceneitem_is_group(item)) return {};
    auto* props = obs_source_properties(obs_sceneitem_get_source(item)); auto schema = propertySchema(props); obs_properties_destroy(props);
    if (d->audioCompanions.contains(name)) {
        QJsonArray options; for (auto value : sourceOptions("application_audio")) { auto row = value.toObject(); if (row["property"] == "window") options.append(QJsonObject{{"label", row["name"]}, {"value", row["value"]}}); }
        schema.append(QJsonObject{{"key", "audio_window"}, {"label", "Application audio target"}, {"type", "list"}, {"options", options}});
    }
    return schema;
}
bool Engine::setSourceProperties(const QString& scene, const QString& name, const QJsonObject& patch) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end() || d->locked(*found, name)) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item || !validateProperties(sourcePropertySchema(scene, name), patch)) return d->fail("Unsupported source property or value");
    const int i = d->row(*found, name); auto row = found->sources[i].toObject(); auto properties = row["properties"].toObject();
    for (auto it = patch.begin(); it != patch.end(); ++it) properties[it.key()] = it.value();
    const auto error = audioSettingsError(row["type"].toString(), properties); if (!error.isEmpty()) return d->fail(error);
    auto* settings = obsSettings(patch); obs_source_update(obs_sceneitem_get_source(item), settings); obs_data_release(settings);
    if (patch.contains("audio_window")) if (auto* companion = d->audioCompanions.value(name)) {
        auto* data = obsSettings({{"window", patch["audio_window"]}, {"priority", 1}}); obs_source_update(obs_sceneitem_get_source(companion), data); obs_data_release(data);
    }
    row["properties"] = properties; found->sources[i] = row; emit scenesChanged(); return true;
}
namespace {
QJsonArray videoCatalog() {
    QJsonArray result; QSet<QString> available; const char* id{};
    for (size_t i = 0; obs_enum_filter_types(i, &id); ++i) available.insert(QString::fromUtf8(id));
    for (const auto& base : {QString("color_filter"), QString("chroma_key_filter"), QString("crop_filter")}) {
        auto native = available.contains(base + "_v2") ? base + "_v2" : base;
        if (!available.contains(native)) continue;
        auto* defaults = obs_get_source_defaults(native.toUtf8().constData());
        auto* properties = obs_get_source_properties(native.toUtf8().constData());
        auto* values = obs_data_get_defaults(defaults); obs_data_apply(values, defaults);
        auto object = QJsonDocument::fromJson(obs_data_get_json(values)).object();
        const auto schema = propertySchema(properties); QJsonObject exposed;
        for (auto field : schema) { const auto key = field.toObject()["key"].toString(); if (object.contains(key)) exposed[key] = object[key]; }
        result.append(QJsonObject{{"type", base}, {"obs_id", native}, {"name", QString::fromUtf8(obs_source_get_display_name(native.toUtf8().constData()))}, {"fields", schema}, {"defaults", exposed}, {"available", true}});
        obs_data_release(values); obs_data_release(defaults); obs_properties_destroy(properties);
    }
    return result;
}
QString normalizeVideoFilters(QJsonArray& chain) {
    if (chain.size() > 16) return "At most 16 video filters are supported";
    QMap<QString, QJsonObject> specs; for (auto value : videoCatalog()) specs[value.toObject()["type"].toString()] = value.toObject();
    QSet<QString> ids; static const QRegularExpression token("^[A-Za-z0-9_-]{1,64}$");
    for (int i = 0; i < chain.size(); ++i) {
        auto row = chain[i].toObject(); auto id = row["id"].toString(); const auto type = row["type"].toString();
        if (!token.match(id).hasMatch() || ids.contains(id) || !specs.contains(type) || row["name"].toString().trimmed().isEmpty() || row["name"].toString().size() > 128 || row["name"].toString().contains(QChar(0)) || !row["enabled"].isBool() || !row["settings"].isObject()) return "Invalid video filter";
        if (!validateProperties(specs[type]["fields"].toArray(), row["settings"].toObject())) return "Invalid video filter settings";
        ids.insert(id); chain[i] = row;
    }
    return {};
}
QString applyVideoFilters(obs_source_t* source, const QJsonArray& before, const QJsonArray& after) {
    QMap<QString, QJsonObject> specs; for (auto value : videoCatalog()) specs[value.toObject()["type"].toString()] = value.toObject();
    using Ref = std::shared_ptr<obs_source_t>;
    std::vector<Ref> staged;
    for (auto value : after) {
        auto row = value.toObject(); auto* settings = obsSettings(row["settings"].toObject());
        Ref filter(obs_source_create_private(specs[row["type"].toString()]["obs_id"].toString().toUtf8().constData(), ("VELA video " + row["id"].toString()).toUtf8().constData(), settings), obs_source_release);
        obs_data_release(settings);
        if (!filter || !obs_obj_get_data(filter.get())) return "Could not create video filter";
        obs_source_set_enabled(filter.get(), row["enabled"].toBool()); staged.push_back(std::move(filter));
    }
    for (auto value : before) {
        auto* filter = obs_source_get_filter_by_name(source, ("VELA video " + value.toObject()["id"].toString()).toUtf8().constData());
        if (filter) { obs_source_filter_remove(source, filter); obs_source_release(filter); }
    }
    for (auto& filter : staged) obs_source_filter_add(source, filter.get());
    for (size_t i = 0; i < staged.size(); ++i) obs_source_filter_set_index(source, staged[i].get(), staged.size() - 1 - i);
    return {};
}
}
QJsonArray Engine::videoFilterTypes() const { return d->ready ? videoCatalog() : QJsonArray{}; }
QJsonArray Engine::videoFilters(const QString& scene, const QString& name) const {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return {};
    const int i = d->row(*found, name); return i < 0 ? QJsonArray{} : found->sources[i].toObject()["properties"].toObject()["video_filters"].toArray();
}
bool Engine::setVideoFilters(const QString& scene, const QString& name, QJsonArray chain) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end() || d->locked(*found, name)) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item || !(obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO)) return false;
    if (const auto error = normalizeVideoFilters(chain); !error.isEmpty()) return d->fail(error);
    if (const auto error = applyVideoFilters(obs_sceneitem_get_source(item), videoFilters(scene, name), chain); !error.isEmpty()) return d->fail(error);
    const int i = d->row(*found, name); auto row = found->sources[i].toObject(); auto properties = row["properties"].toObject(); properties["video_filters"] = chain; row["properties"] = properties; found->sources[i] = row; emit scenesChanged(); return true;
}
