#include "feathercast/ui/automation_panel.hpp"
#include <QCoreApplication>
#include <iostream>
using namespace feathercast;
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  core::Rule r;
  r.id = "roundtrip";
  r.priority = 91;
  r.enabled = false;
  r.debounce_ms = 4321;
  r.cooldown_ms = 8765;
  r.repeat_while_matching = true;
  r.any_of = {{{core::ConditionType::ProcessRunning, "game.exe", 3.25, true},
               {core::ConditionType::AudioAboveDb, "", -25, false}},
              {{core::ConditionType::Hotkey, "save_replay"}}};
  for (int i = 0; i <= int(core::ActionType::Log); ++i)
    r.actions.push_back({core::ActionType(i), "argument", 12345});
  auto document = ui::encodeRules({r}, true, "Desktop");
  std::vector<core::Rule> parsed;
  bool enabled = false;
  QString fallback, error;
  if (!ui::decodeRules(document, parsed, enabled, fallback, error) ||
      ui::encodeRules(parsed, enabled, fallback) != document)
    return 1;
  int rejected = 0;
  auto reject = [&](QJsonObject invalid) {
    auto before = ui::encodeRules(parsed, enabled, fallback);
    bool result = !ui::decodeRules(invalid, parsed, enabled, fallback, error) &&
                  ui::encodeRules(parsed, enabled, fallback) == before;
    if (result)
      ++rejected;
    return result;
  };
  auto invalid = document;
  invalid["version"] = 2;
  if (!reject(invalid))
    return 2;
  invalid = document;
  auto rules = invalid["rules"].toArray();
  rules.append(rules[0]);
  invalid["rules"] = rules;
  if (!reject(invalid))
    return 3;
  for (auto field : {"priority", "debounceMs", "cooldownMs", "enabled",
                     "groups", "actions"}) {
    invalid = document;
    auto rs = invalid["rules"].toArray();
    auto rule = rs[0].toObject();
    rule[field] = "wrong type";
    rs[0] = rule;
    invalid["rules"] = rs;
    if (!reject(invalid))
      return 4;
  }
  invalid = document;
  rules = invalid["rules"].toArray();
  auto rule = rules[0].toObject();
  auto actions = rule["actions"].toArray();
  auto action = actions[0].toObject();
  action["type"] = 99;
  actions[0] = action;
  rule["actions"] = actions;
  rules[0] = rule;
  invalid["rules"] = rules;
  if (!reject(invalid))
    return 5;
  action["type"] = int(core::ActionType::Log);
  action["argument"] = "rtmp://user:secret@host/app/key";
  actions[0] = action;
  rule["actions"] = actions;
  rules[0] = rule;
  invalid["rules"] = rules;
  if (!reject(invalid))
    return 6;
  std::cout << "Roundtrip all fields/action enums; " << rejected
            << " malformed documents rejected transactionally\n";
  return 0;
}
