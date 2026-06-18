#pragma once

#include <cstdint>
#include <string>

int open_serial_port(const std::string& port, int baud);
int serial_read_with_timeout(int fd, uint8_t* buf, int size, int timeout_ms);
uint64_t monotonic_ns();

