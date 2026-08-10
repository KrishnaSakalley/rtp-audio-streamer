#include "rtp/udp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace rtp::net {

UdpSocket::UdpSocket() {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
  }
}

UdpSocket::~UdpSocket() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void UdpSocket::bind_to(uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
  }
}

void UdpSocket::set_receive_timeout(int timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    throw std::runtime_error(std::string("setsockopt(SO_RCVTIMEO) failed: ") + std::strerror(errno));
  }
}

ssize_t UdpSocket::receive(void* buffer, size_t buffer_len) {
  ssize_t n = ::recvfrom(fd_, buffer, buffer_len, 0, nullptr, nullptr);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }
    throw std::runtime_error(std::string("recvfrom() failed: ") + std::strerror(errno));
  }
  return n;
}

void UdpSocket::send_to(const void* buffer, size_t buffer_len, const std::string& host, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    throw std::runtime_error("invalid IPv4 address: " + host);
  }
  ssize_t n = ::sendto(fd_, buffer, buffer_len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (n < 0 || static_cast<size_t>(n) != buffer_len) {
    throw std::runtime_error(std::string("sendto() failed: ") + std::strerror(errno));
  }
}

}  // namespace rtp::net
