#pragma once

#include "replay_index.h"

#include <string>
#include <vector>

struct ReplayConfig {
  std::vector<ReplayTopic> topics;
};

ReplayConfig default_replay_config();
ReplayConfig load_replay_config(const std::string& path);
