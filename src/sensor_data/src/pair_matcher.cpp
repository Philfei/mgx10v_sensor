#include "pair_matcher.h"

#include <cstdlib>

namespace slam {

size_t PairMatcher::left_buffered() const {
  std::lock_guard<std::mutex> g(mu_);
  return left_.size();
}
size_t PairMatcher::right_buffered() const {
  std::lock_guard<std::mutex> g(mu_);
  return right_.size();
}

void PairMatcher::push_left(std::shared_ptr<Frame> f) {
  std::lock_guard<std::mutex> g(mu_);
  left_.push_back(std::move(f));
  if (left_.size() > max_buf_) left_.pop_front();
  try_match();
}

void PairMatcher::push_right(std::shared_ptr<Frame> f) {
  std::lock_guard<std::mutex> g(mu_);
  right_.push_back(std::move(f));
  if (right_.size() > max_buf_) right_.pop_front();
  try_match();
}

void PairMatcher::try_match() {
  while (!left_.empty() && !right_.empty()) {
    auto& l = left_.front();
    auto& r = right_.front();
    ns_t dt = l->ts_ns - r->ts_ns;
    ns_t adt = std::llabs(dt);

    if (adt <= tol_ns_) {
      FramePair p;
      p.left  = l;
      p.right = r;
      p.pair_dt_ns = dt;
      left_.pop_front();
      right_.pop_front();
      if (cb_) cb_(std::move(p));
      continue;
    }

    if (dt < 0) {
      left_.pop_front();
    } else {
      right_.pop_front();
    }
  }
}

}
