#pragma once

#include "recorder_config.h"

#include <vector>

struct ReceiverRuntime {
  TopicConfig topic;
  int context_id = 0;
  int context_io_threads = 1;
};

std::vector<ReceiverRuntime> make_receiver_runtimes(
    const std::vector<TopicConfig>& topics);
