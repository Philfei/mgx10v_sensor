#include "receiver_runtime.h"

#include <cstddef>
#include <utility>

std::vector<ReceiverRuntime> make_receiver_runtimes(
    const std::vector<TopicConfig>& topics) {
  std::vector<ReceiverRuntime> runtimes;
  runtimes.reserve(topics.size());
  for (std::size_t i = 0; i < topics.size(); ++i) {
    ReceiverRuntime runtime;
    runtime.topic = topics[i];
    runtime.context_id = static_cast<int>(i);
    runtime.context_io_threads = 1;
    runtimes.push_back(std::move(runtime));
  }
  return runtimes;
}
