// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verilog/analysis/verilog_linter_configuration.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "common/analysis/line_lint_rule.h"
#include "common/analysis/syntax_tree_lint_rule.h"
#include "common/analysis/text_structure_lint_rule.h"
#include "common/analysis/token_stream_lint_rule.h"
#include "common/util/container_util.h"
#include "common/util/enum_flags.h"
#include "common/util/logging.h"
#include "verilog/analysis/default_rules.h"
#include "verilog/analysis/lint_rule_registry.h"

namespace verilog {

using verible::LineLintRule;
using verible::SyntaxTreeLintRule;
using verible::TextStructureLintRule;
using verible::TokenStreamLintRule;
using verible::container::FindOrNull;

template <typename List>
static const char* MatchesAnyItem(absl::string_view filename,
                                  const List& items) {
  for (const auto item : items) {
    if (filename.find(item) != absl::string_view::npos) {
      return item;
    }
  }
  return nullptr;
}

const char* ProjectPolicy::MatchesAnyPath(absl::string_view filename) const {
  return MatchesAnyItem(filename, path_substrings);
}

const char* ProjectPolicy::MatchesAnyExclusions(
    absl::string_view filename) const {
  return MatchesAnyItem(filename, path_exclusions);
}

bool ProjectPolicy::IsValid() const {
  for (const auto rule : disabled_rules) {
    if (!analysis::IsRegisteredLintRule(rule)) return false;
  }
  for (const auto rule : enabled_rules) {
    if (!analysis::IsRegisteredLintRule(rule)) return false;
  }
  return true;
}

std::string ProjectPolicy::ListPathGlobs() const {
  return absl::StrJoin(path_substrings.begin(), path_substrings.end(), " | ",
                       [](std::string* out, absl::string_view pattern) {
                         absl::StrAppend(out, "*", pattern, "*");
                       });
}

bool RuleBundle::ParseConfiguration(absl::string_view text,
                                    std::string* error) {
  // Clear the vector to overwrite any existing value.
  rules.clear();

  for (absl::string_view part :
       absl::StrSplit(text, absl::ByAnyChar(",\n"), absl::SkipEmpty())) {
    // If prefix is '-', the rule is disabled.
    // Note that part is guaranteed to be at least one character because
    // of absl::SkipEmpty()
    const bool prefix_minus = (part[0] == '-');

    RuleSetting setting = {!prefix_minus, ""};

    const auto rule_name_with_config = part.substr(prefix_minus ? 1 : 0);

    // Independent of the enabled-ness: extract a configuration string
    // if there is any assignment.
    const auto equals_pos = rule_name_with_config.find('=');
    if (equals_pos != absl::string_view::npos) {
      const auto config = rule_name_with_config.substr(equals_pos + 1);
      setting.configuration.assign(config.data(), config.size());
    }
    const auto rule_name = rule_name_with_config.substr(0, equals_pos);
    const auto rule_name_set = analysis::GetAllRegisteredLintRuleNames();
    const auto rule_iter = rule_name_set.find(rule_name);

    // Check if text is a valid lint rule.
    if (rule_iter == rule_name_set.end()) {
      *error = absl::StrCat("invalid flag \"", rule_name, "\"");
      return false;
    } else {
      // Map keys must use canonical registered string_views for guaranteed
      // lifetime, not just any string-equivalent copy.
      rules[*rule_iter] = setting;
    }
  }

  return true;
}

// Parse and unparse for RuleBundle (for commandlineflags)
std::string RuleBundle::UnparseConfiguration(const char separator) const {
  std::vector<std::string> switches;
  for (const auto& rule : rules) {
    switches.push_back(absl::StrCat(
        // If rule is set off, prepend "-"
        rule.second.enabled ? "" : "-", rule.first,
        // If we have a configuration, append assignment.
        rule.second.configuration.empty() ? "" : "=",
        rule.second.configuration));
  }
  // Concatenates all of rules into text.
  return absl::StrJoin(switches.rbegin(), switches.rend(),
                       std::string(1, separator));
}

bool LinterConfiguration::RuleIsOn(const analysis::LintRuleId& rule) const {
  const auto* entry = FindOrNull(configuration_, rule);
  if (entry == nullptr) return false;
  return entry->enabled;
}

void LinterConfiguration::UseRuleSet(const RuleSet& rules) {
  configuration_.clear();
  switch (rules) {
    case RuleSet::kAll: {
      for (const auto& rule : analysis::RegisteredTextStructureRulesNames()) {
        TurnOn(rule);
      }
      for (const auto& rule : analysis::RegisteredSyntaxTreeRulesNames()) {
        TurnOn(rule);
      }
      for (const auto& rule : analysis::RegisteredTokenStreamRulesNames()) {
        TurnOn(rule);
      }
      for (const auto& rule : analysis::RegisteredLineRulesNames()) {
        TurnOn(rule);
      }
      break;
    }
    case RuleSet::kNone:
      break;
    case RuleSet::kDefault:
      for (const auto& rule : analysis::kDefaultRuleSet) {
        TurnOn(rule);
      }
  }
}

void LinterConfiguration::UseRuleBundle(const RuleBundle& rule_bundle) {
  const auto name_set = analysis::GetAllRegisteredLintRuleNames();
  for (const auto& rule_pair : rule_bundle.rules) {
    // This needs to use the canonical registered key string_view, which has
    // guaranteed lifetime.
    configuration_[rule_pair.first] = rule_pair.second;
  }
}

void LinterConfiguration::UseProjectPolicy(const ProjectPolicy& policy,
                                           absl::string_view filename) {
  if (const char* matched_path = policy.MatchesAnyPath(filename)) {
    VLOG(1) << "File \"" << filename << "\" matches path \"" << matched_path
            << "\" from project policy [" << policy.name << "], applying it.";
    for (const auto rule : policy.disabled_rules) {
      VLOG(1) << "  disabling rule: " << rule;
      TurnOff(rule);
    }
    for (const auto rule : policy.enabled_rules) {
      VLOG(1) << "  enabling rule: " << rule;
      TurnOn(rule);
    }
  }
}

std::set<analysis::LintRuleId> LinterConfiguration::ActiveRuleIds() const {
  std::set<analysis::LintRuleId> result;
  for (const auto& rule_pair : configuration_) {
    if (rule_pair.second.enabled) {
      result.insert(rule_pair.first);
    }
  }
  return result;
}

// Iterates through all rules that are mentioned and enabled
// in the "config" map. Constructs instances using the
// "factory"-function, and configures them if a configuration string is
// available. Returns a vector of all successfully created instances.
//
// T should be a descendant of verible::LintRule.
template <typename T>
static std::vector<std::unique_ptr<T>> CreateRules(
    const std::map<analysis::LintRuleId, RuleSetting>& config,
    std::function<std::unique_ptr<T>(const analysis::LintRuleId&)> factory) {
  std::vector<std::unique_ptr<T>> rule_instances;
  for (const auto& rule_pair : config) {
    const RuleSetting& setting = rule_pair.second;
    if (!setting.enabled) continue;

    std::unique_ptr<T> rule_ptr = factory(rule_pair.first);
    if (rule_ptr == nullptr) continue;

    if (!setting.configuration.empty()) {
      absl::Status status;
      if (!(status = rule_ptr->Configure(setting.configuration)).ok()) {
        // TODO(hzeller): return error message to caller to handle ?
        LOG(QFATAL) << rule_pair.first << ": " << status.message();
      }
    }

    rule_instances.push_back(std::move(rule_ptr));
  }
  return rule_instances;
}

std::vector<std::unique_ptr<SyntaxTreeLintRule>>
LinterConfiguration::CreateSyntaxTreeRules() const {
  return CreateRules<SyntaxTreeLintRule>(configuration_,
                                         analysis::CreateSyntaxTreeLintRule);
}

std::vector<std::unique_ptr<TokenStreamLintRule>>
LinterConfiguration::CreateTokenStreamRules() const {
  return CreateRules<TokenStreamLintRule>(configuration_,
                                          analysis::CreateTokenStreamLintRule);
}

std::vector<std::unique_ptr<LineLintRule>>
LinterConfiguration::CreateLineRules() const {
  return CreateRules<LineLintRule>(configuration_,
                                   analysis::CreateLineLintRule);
}

std::vector<std::unique_ptr<TextStructureLintRule>>
LinterConfiguration::CreateTextStructureRules() const {
  return CreateRules<TextStructureLintRule>(
      configuration_, analysis::CreateTextStructureLintRule);
}

bool LinterConfiguration::operator==(const LinterConfiguration& config) const {
  return ActiveRuleIds() == config.ActiveRuleIds();
}

std::ostream& operator<<(std::ostream& stream,
                         const LinterConfiguration& config) {
  const auto rules = config.ActiveRuleIds();
  return stream << "{ " << absl::StrJoin(rules.begin(), rules.end(), ", ")
                << " }";
}

static const std::initializer_list<std::pair<const absl::string_view, RuleSet>>
    kRuleSetEnumStringMap = {
        {"all", RuleSet::kAll},
        {"none", RuleSet::kNone},
        {"default", RuleSet::kDefault},
};

std::ostream& operator<<(std::ostream& stream, RuleSet rules) {
  static const auto* flag_map =
      verible::MakeEnumToStringMap(kRuleSetEnumStringMap);
  return stream << flag_map->find(rules)->second;
}

//
// Parse and unparse for ruleset (for commandlineflags)
//
std::string AbslUnparseFlag(const RuleSet& rules) {
  std::ostringstream stream;
  stream << rules;
  return stream.str();
}

bool AbslParseFlag(absl::string_view text, RuleSet* rules, std::string* error) {
  static const auto* flag_map =
      verible::MakeStringToEnumMap(kRuleSetEnumStringMap);
  return EnumMapParseFlag(*flag_map, text, rules, error);
}

std::string AbslUnparseFlag(const RuleBundle& bundle) {
  return bundle.UnparseConfiguration(',');
}

bool AbslParseFlag(absl::string_view text, RuleBundle* bundle,
                   std::string* error) {
  return bundle->ParseConfiguration(text, error);
}

}  // namespace verilog