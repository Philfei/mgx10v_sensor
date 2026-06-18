#include "receiver_runtime.h"

#include "recorder_config.h"

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

TopicConfig make_topic(std::string name, TopicType type) {
  TopicConfig topic;
  topic.name = std::move(name);
  topic.type = type;
  topic.endpoint = "ipc:///tmp/" + topic.name;
  topic.topic = topic.name + "_topic";
  return topic;
}

}  // namespace

int main() {
  const std::vector<TopicConfig> topics = {
      make_topic("cam_left", TopicType::RawImage),
      make_topic("imu", TopicType::Imu),
      make_topic("gnss", TopicType::Gnss),
  };

  const std::vector<ReceiverRuntime> runtimes =
      make_receiver_runtimes(topics);
  require(runtimes.size() == topics.size(), "runtime count mismatch");

  std::unordered_set<int> context_ids;
  for (size_t i = 0; i < runtimes.size(); ++i) {
    require(runtimes[i].topic.name == topics[i].name, "topic order changed");
    require(runtimes[i].context_io_threads == 1,
            "unexpected context IO thread count");
    context_ids.insert(runtimes[i].context_id);
  }

  require(context_ids.size() == topics.size(),
          "each topic must use an independent ZMQ context");
  return 0;
}
