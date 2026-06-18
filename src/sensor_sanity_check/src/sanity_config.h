#pragma once

#include <string>
#include <unordered_map>

std::unordered_map<std::string, double> load_gap_thresholds(
    const std::string& path);
