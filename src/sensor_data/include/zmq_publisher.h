#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/timestamp.pb.h>
#include <zmq.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

struct ZmqPublisherOptions {
  int send_high_water_mark = 10;
};

class ZmqPublisher {
public:
  ZmqPublisher(zmq::context_t& context, std::string endpoint,
               std::string topic,
               ZmqPublisherOptions options = ZmqPublisherOptions{});

  bool bind();
  bool publish(const google::protobuf::Message& message);
  bool publish_with_raw_payload(const google::protobuf::Message& header,
                                const void* data, size_t size);

  const std::string& endpoint() const { return endpoint_; }
  const std::string& topic() const { return topic_; }

private:
  std::string endpoint_;
  std::string topic_;
  zmq::socket_t socket_;
};

void set_timestamp_from_ns(google::protobuf::Timestamp* stamp, int64_t ns);
void set_timestamp_from_hhmmss(google::protobuf::Timestamp* stamp, double hhmmss);
double hhmmss_to_seconds_of_day(double hhmmss);
void unlink_ipc_endpoint(const std::string& endpoint);
