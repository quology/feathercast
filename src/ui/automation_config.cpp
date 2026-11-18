#include "feathercast/ui/automation_panel.hpp"
#include <cmath>
namespace feathercast::ui {
using namespace core;
QJsonObject encodeRules(const std::vector<Rule> &rules, bool enabled,
                        const QString &fallback) {
  QJsonArray rs;
  for (const auto &r : rules) {
    QJsonArray groups, acts;
    for (const auto &g : r.any_of) {
      QJsonArray cs;
      for (const auto &c : g)
        cs.append(QJsonObject{{"type", int(c.type)},
                              {"text", QString::fromStdString(c.text)},
                              {"number", c.number},
                              {"negate", c.negate}});
      groups.append(cs);
    }
    for (const auto &a : r.actions)
      acts.append(QJsonObject{{"type", int(a.type)},
                              {"argument", QString::fromStdString(a.argument)},
                              {"value", double(a.value)}});
    rs.append(QJsonObject{{"id", QString::fromStdString(r.id)},
                          {"priority", r.priority},
                          {"enabled", r.enabled},
                          {"groups", groups},
                          {"actions", acts},
                          {"debounceMs", double(r.debounce_ms)},
                          {"cooldownMs", double(r.cooldown_ms)},
                          {"repeat", r.repeat_while_matching}});
  }
  return {{"format", "FeatherCastAutomation"},
          {"version", 1},
          {"enabled", enabled},
          {"fallback", fallback},
          {"rules", rs}};
}
bool decodeRules(const QJsonObject &root, std::vector<Rule> &rules,
                 bool &enabled, QString &fallback, QString &error) {
  auto fail = [&](QString e) {
    error = e;
    return false;
  };
  if (root["format"] != "FeatherCastAutomation" || root["version"] != 1 ||
      !root["rules"].isArray() || !root["enabled"].isBool() ||
      !root["fallback"].isString())
    return fail("Unsupported automation document");
  std::vector<Rule> next;
  auto integer = [](QJsonValue v, double min, double max) {
    auto n = v.toDouble(NAN);
    return v.isDouble() && std::isfinite(n) && std::floor(n) == n && n >= min &&
           n <= max;
  };
  for (auto v : root["rules"].toArray()) {
    if (!v.isObject())
      return fail("Invalid rule");
    auto o = v.toObject();
    Rule r;
    if (!o["id"].isString() || !integer(o["priority"], INT_MIN, INT_MAX) ||
        !o["enabled"].isBool() || !o["groups"].isArray() ||
        !o["actions"].isArray() || !integer(o["debounceMs"], 0, 86400000) ||
        !integer(o["cooldownMs"], 0, 86400000) || !o["repeat"].isBool())
      return fail("Invalid rule fields");
    r.id = o["id"].toString().toStdString();
    r.priority = o["priority"].toInt();
    r.enabled = o["enabled"].toBool();
    r.debounce_ms = o["debounceMs"].toInteger();
    r.cooldown_ms = o["cooldownMs"].toInteger();
    r.repeat_while_matching = o["repeat"].toBool();
    for (auto gv : o["groups"].toArray()) {
      if (!gv.isArray())
        return fail("Invalid condition group");
      ConditionGroup g;
      for (auto cv : gv.toArray()) {
        auto c = cv.toObject();
        if (!cv.isObject() || !integer(c["type"], 0, 10) ||
            !c["text"].isString() || !c["number"].isDouble() ||
            !std::isfinite(c["number"].toDouble()) || !c["negate"].isBool())
          return fail("Invalid condition");
        g.push_back({ConditionType(c["type"].toInt()),
                     c["text"].toString().toStdString(), c["number"].toDouble(),
                     c["negate"].toBool()});
      }
      r.any_of.push_back(g);
    }
    for (auto av : o["actions"].toArray()) {
      auto a = av.toObject();
      if (!av.isObject() || !integer(a["type"], 0, 10) ||
          !a["argument"].isString() ||
          !integer(a["value"], -86400000, 86400000))
        return fail("Invalid action");
      if ((a["type"].toInt() == int(ActionType::SetDelayMs) &&
           (a["value"].toInteger() < 0 || a["value"].toInteger() > 300000)))
        return fail("Delay must be within 0–300000 ms");
      if (a["argument"].toString().contains("://"))
        return fail(
            "URLs and credentials are not allowed in automation arguments");
      r.actions.push_back({ActionType(a["type"].toInt()),
                           a["argument"].toString().toStdString(),
                           a["value"].toInteger()});
    }
    next.push_back(r);
  }
  AutomationEngine validator;
  std::string reason;
  if (!validator.set_rules(next, &reason))
    return fail(QString::fromStdString(reason));
  rules = std::move(next);
  enabled = root["enabled"].toBool();
  fallback = root["fallback"].toString();
  error.clear();
  return true;
}
} // namespace feathercast::ui
