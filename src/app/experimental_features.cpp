// experimental_features.cpp - Features a player turns on with a line in features.txt.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/experimental_features.h"
#include "community/endpoints.h"

#include <cctype>
#include <cstdio>
#include <utility>

namespace playden::app {

ExperimentalFeatures DefaultExperimentalFeatures(bool all_on) {
  // Reports require an explicit API endpoint in features.txt on every platform.
  // Local game import is available in nightlies and development builds.
  ExperimentalFeatures features;
  features.community_recipes = true;
  features.import_games = all_on;
  return features;
}

ExperimentalFeatures ParseExperimentalFeatures(std::string_view text, ExperimentalFeatures defaults,
                                               std::vector<std::string>* unknown) {
  ExperimentalFeatures features = defaults;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    if (line == "community-recipes") features.community_recipes = true;
    else if (line == "online-steam") features.online_steam = true;
    else if (line == "import-games") features.import_games = true;
    else if (line.rfind("game-test-reports=https://", 0) == 0 &&
             line.find_first_of(" \t\r\n?#") == std::string_view::npos) {
      std::string endpoint(line.substr(sizeof("game-test-reports=") - 1));
      if (!community::UrlOrigin(endpoint).empty()) {
        while (endpoint.size() > 1 && endpoint.back() == '/') endpoint.pop_back();
        features.game_test_reports_api = std::move(endpoint);
        features.game_test_reports = true;
      } else if (unknown) {
        unknown->emplace_back(line);
      }
    }
    else if (unknown) unknown->emplace_back(line);
  }
  return features;
}

ExperimentalFeatures ReadExperimentalFeatures(const std::string& path, ExperimentalFeatures defaults,
                                              std::vector<std::string>* unknown) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return defaults;
  std::string text;
  char chunk[1024];
  for (std::size_t count; text.size() < kExperimentalFeaturesMaxBytes && (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
    text.append(chunk, count);
  std::fclose(file);
  if (text.size() > kExperimentalFeaturesMaxBytes) text.resize(kExperimentalFeaturesMaxBytes);
  return ParseExperimentalFeatures(text, defaults, unknown);
}

}  // namespace playden::app
