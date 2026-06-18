#include "bounded_frame_queue.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

}  // namespace

int main() {
  slam::BoundedFrameQueue<int> queue(2);

  require(queue.push(1), "first push failed");
  require(queue.push(2), "second push failed");
  require(queue.push(3), "third push failed");
  require(queue.dropped_count() == 1, "queue should drop one oldest item");

  int value = 0;
  require(queue.pop(&value) && value == 2,
          "first pop should return the oldest retained item");
  require(queue.pop(&value) && value == 3,
          "second pop should return the newest retained item");

  queue.close();
  require(!queue.push(4), "push should fail after close");
  require(!queue.pop(&value), "pop should fail after closed and drained");
  return 0;
}
