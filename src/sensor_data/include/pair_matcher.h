#pragma once
#include "types.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace slam {

class PairMatcher {
public:
  using PairCallback = std::function<void(FramePair)>;

  explicit PairMatcher(ns_t tol_ns = 2'000'000) : tol_ns_(tol_ns) {}

  void set_callback(PairCallback cb) { cb_ = std::move(cb); }

  void push_left(std::shared_ptr<Frame> f);
  void push_right(std::shared_ptr<Frame> f);

  size_t left_buffered()  const;
  size_t right_buffered() const;

private:
  void try_match();

  mutable std::mutex mu_;
  std::deque<std::shared_ptr<Frame>> left_;
  std::deque<std::shared_ptr<Frame>> right_;
  PairCallback cb_;
  ns_t tol_ns_;
  size_t max_buf_ = 16;
};

}
