#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {

speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return B115200;
  }
}

}  // namespace

uint64_t monotonic_ns() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

int open_serial_port(const std::string& port, int baud) {
  int fd = open(port.c_str(), O_RDWR | O_NOCTTY);
  if (fd < 0) {
    perror("open serial");
    return -1;
  }

  termios t {};
  if (tcgetattr(fd, &t) != 0) {
    perror("tcgetattr");
    close(fd);
    return -1;
  }

  speed_t sp = baud_to_speed(baud);
  cfsetispeed(&t, sp);
  cfsetospeed(&t, sp);

  t.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
  t.c_cflag |= CS8 | CREAD | CLOCAL;
  t.c_iflag &= ~(IXON | IXOFF | IXANY | INLCR | IGNCR | ICRNL);
  t.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  t.c_oflag &= ~OPOST;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &t) != 0) {
    perror("tcsetattr");
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

int serial_read_with_timeout(int fd, uint8_t* buf, int size, int timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);

  timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int ret = select(fd + 1, &fds, nullptr, nullptr, &tv);
  if (ret <= 0) {
    return ret;
  }
  return read(fd, buf, size);
}

