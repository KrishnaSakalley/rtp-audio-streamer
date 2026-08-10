#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rtp::net {

// Sized to the Ethernet MTU (1500 B): recvfrom() silently truncates a
// datagram larger than the caller's buffer, so every receiver sizes to this.
constexpr size_t kMaxDatagramBytes = 1500;

// Thin wrapper over a POSIX UDP socket. No abstraction beyond RAII lifetime
// and clear error reporting -- the plan calls for hand-rolled sockets, not a
// networking framework.
class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Binds to INADDR_ANY:port so this socket can receive datagrams.
  void bind_to(uint16_t port);

  // 0 (the default) blocks forever; otherwise receive() returns -1 after
  // this many idle milliseconds instead of blocking.
  void set_receive_timeout(int timeout_ms);

  // Returns the payload length, or -1 if set_receive_timeout() elapsed
  // without a datagram arriving. Throws on any other socket error.
  ssize_t receive(void* buffer, size_t buffer_len);

  void send_to(const void* buffer, size_t buffer_len, const std::string& host, uint16_t port);

 private:
  int fd_;
};

}  // namespace rtp::net
