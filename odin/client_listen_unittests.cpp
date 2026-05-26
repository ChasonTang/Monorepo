// odin/client_listen_unittests.cpp
//
// Unit tests T1-T9 from §7 of odin/docs/rfc_009_client_listen_handshake.md.

#include "odin/client_listen.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

namespace {

constexpr const char kResp200[] =
    "HTTP/1.1 200 Connection Established\r\n\r\n";
constexpr size_t kResp200Len = sizeof(kResp200) - 1; // 39

constexpr const char kResp400[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
constexpr size_t kResp400Len = sizeof(kResp400) - 1; // 28

constexpr const char kResp405[] =
    "HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n";
constexpr size_t kResp405Len = sizeof(kResp405) - 1; // 51

constexpr const char kResp414[] = "HTTP/1.1 414 URI Too Long\r\n\r\n";
constexpr size_t kResp414Len = sizeof(kResp414) - 1; // 29

constexpr const char kResp505[] =
    "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
constexpr size_t kResp505Len = sizeof(kResp505) - 1; // 43

// Drain fd until EOF or non-EINTR error; return collected bytes.
std::string DrainToEof(int fd) {
  std::string out;
  uint8_t buf[1024];
  for (;;) {
    const ssize_t r = read(fd, buf, sizeof(buf));
    if (r > 0) {
      out.append(reinterpret_cast<char *>(buf), static_cast<size_t>(r));
      continue;
    }
    if (r == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  return out;
}

// Write all bytes to fd, retrying on EINTR; return true on full delivery.
bool WriteAll(int fd, const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t r = write(fd, p + off, len - off);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (r == 0) {
      return false;
    }
    off += static_cast<size_t>(r);
  }
  return true;
}

} // namespace

// T1 — OK round-trip: well-formed CONNECT → 200 → EOF.
TEST(OdinClientListenTest, T1OkRoundTrip) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);
  const char *req = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";
  ASSERT_TRUE(WriteAll(fds[0], req, 36));
  ASSERT_EQ(shutdown(fds[0], SHUT_WR), 0) << strerror(errno);

  ASSERT_EQ(odin_client_listen_handshake(fds[1]), ODIN_CLIENT_LISTEN_OK);

  const std::string got = DrainToEof(fds[0]);
  EXPECT_EQ(got.size(), kResp200Len);
  EXPECT_EQ(got, std::string(kResp200, kResp200Len));
  EXPECT_EQ(close(fds[0]), 0);
}

// T2 — Five small-input parser errors → byte-exact 4xx/5xx responses.
TEST(OdinClientListenTest, T2RejectedResponses) {
  struct Case {
    std::string request;
    const char *expected_bytes;
    size_t expected_len;
  };
  const std::vector<Case> cases = {
      {"GET / HTTP/1.1\r\n\r\n", kResp405, kResp405Len},
      {"CONNECT bad HTTP/1.1\r\n\r\n", kResp400, kResp400Len},
      {"CONNECT a:1 HTTP/2.0\r\n\r\n", kResp505, kResp505Len},
      {"CONNECT a:99999 HTTP/1.1\r\n\r\n", kResp400, kResp400Len},
      {std::string("CONNECT ") + std::string(256, 'x') +
           ":1 HTTP/1.1\r\n\r\n",
       kResp400, kResp400Len},
  };

  for (const auto &c : cases) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);
    ASSERT_TRUE(WriteAll(fds[0], c.request.data(), c.request.size()))
        << "req=" << c.request;
    ASSERT_EQ(shutdown(fds[0], SHUT_WR), 0) << strerror(errno);

    EXPECT_EQ(odin_client_listen_handshake(fds[1]),
              ODIN_CLIENT_LISTEN_REJECTED)
        << "req=" << c.request;

    const std::string got = DrainToEof(fds[0]);
    EXPECT_EQ(got.size(), c.expected_len) << "req=" << c.request;
    EXPECT_EQ(got, std::string(c.expected_bytes, c.expected_len))
        << "req=" << c.request;
    EXPECT_EQ(close(fds[0]), 0);
  }
}

// T3 — Incremental writes: byte-at-a-time still completes the parse.
TEST(OdinClientListenTest, T3IncrementalWrites) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);

  const std::string req = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";
  std::thread writer([wfd = fds[0], req]() {
    for (size_t i = 0; i < req.size(); ++i) {
      const uint8_t b = static_cast<uint8_t>(req[i]);
      for (;;) {
        const ssize_t r = write(wfd, &b, 1);
        if (r == 1) {
          break;
        }
        if (r < 0 && errno == EINTR) {
          continue;
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)shutdown(wfd, SHUT_WR);
  });

  EXPECT_EQ(odin_client_listen_handshake(fds[1]), ODIN_CLIENT_LISTEN_OK);
  writer.join();

  const std::string got = DrainToEof(fds[0]);
  EXPECT_EQ(got, std::string(kResp200, kResp200Len));
  EXPECT_EQ(close(fds[0]), 0);
}

// T4 — Pipelined bytes after CONNECT discarded; response unaffected.
TEST(OdinClientListenTest, T4PipelinedBytesDiscarded) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);

  const char *bytes =
      "CONNECT example.com:443 HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n";
  ASSERT_TRUE(WriteAll(fds[0], bytes, 52));
  ASSERT_EQ(shutdown(fds[0], SHUT_WR), 0) << strerror(errno);

  EXPECT_EQ(odin_client_listen_handshake(fds[1]), ODIN_CLIENT_LISTEN_OK);

  const std::string got = DrainToEof(fds[0]);
  EXPECT_EQ(got, std::string(kResp200, kResp200Len));
  EXPECT_EQ(close(fds[0]), 0);
}

// T5 — Peer half-closes mid-request → PEER_CLOSED, no response written.
TEST(OdinClientListenTest, T5PeerClosed) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);
  ASSERT_TRUE(WriteAll(fds[0], "CONNECT ", 8));
  ASSERT_EQ(shutdown(fds[0], SHUT_WR), 0) << strerror(errno);

  EXPECT_EQ(odin_client_listen_handshake(fds[1]),
            ODIN_CLIENT_LISTEN_PEER_CLOSED);

  uint8_t junk[16];
  const ssize_t r = read(fds[0], junk, sizeof(junk));
  EXPECT_EQ(r, 0);
  EXPECT_EQ(close(fds[0]), 0);
}

// T6 — Request exceeds ODIN_HTTP_REQUEST_MAX → REJECTED + byte-exact 414.
TEST(OdinClientListenTest, T6RequestTooLarge) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << strerror(errno);

  std::string big = "CONNECT example.com:443 HTTP/1.1\r\n";
  big += "X-Pad: ";
  big.append(8188, 'a');
  big += "\r\n\r\n";
  ASSERT_EQ(big.size(), static_cast<size_t>(8233));

  std::thread writer([wfd = fds[0], big]() {
    size_t off = 0;
    while (off < big.size()) {
      const ssize_t r = write(wfd, big.data() + off, big.size() - off);
      if (r > 0) {
        off += static_cast<size_t>(r);
        continue;
      }
      if (r < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
    (void)shutdown(wfd, SHUT_WR);
  });

  EXPECT_EQ(odin_client_listen_handshake(fds[1]), ODIN_CLIENT_LISTEN_REJECTED);
  writer.join();

  const std::string got = DrainToEof(fds[0]);
  EXPECT_EQ(got.size(), kResp414Len);
  EXPECT_EQ(got, std::string(kResp414, kResp414Len));
  EXPECT_EQ(close(fds[0]), 0);
}

// T7 — Unconnected socket → IO_ERROR on first read(2).
TEST(OdinClientListenTest, T7UnconnectedSocketIoError) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << strerror(errno);
  EXPECT_EQ(odin_client_listen_handshake(fd), ODIN_CLIENT_LISTEN_IO_ERROR);
}

// T8 — odin_client_listen_open binds to 127.0.0.1 with ephemeral port.
TEST(OdinClientListenTest, T8OpenBindsLoopbackEphemeral) {
  const int fd = odin_client_listen_open(0);
  ASSERT_GE(fd, 0) << strerror(errno);

  struct sockaddr_in addr;
  socklen_t alen = sizeof(addr);
  std::memset(&addr, 0, sizeof(addr));
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &alen),
            0)
      << strerror(errno);
  EXPECT_EQ(addr.sin_family, AF_INET);
  EXPECT_EQ(addr.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
  EXPECT_NE(addr.sin_port, 0);

  int reuse = 0;
  socklen_t rlen = sizeof(reuse);
  ASSERT_EQ(
      getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, &rlen), 0)
      << strerror(errno);
  EXPECT_NE(reuse, 0);

  EXPECT_EQ(close(fd), 0);
}

// T9 — End-to-end on real TCP: open + accept + handshake round-trip.
TEST(OdinClientListenTest, T9EndToEndRealTcp) {
  const int lfd = odin_client_listen_open(0);
  ASSERT_GE(lfd, 0) << strerror(errno);

  struct sockaddr_in addr;
  socklen_t alen = sizeof(addr);
  std::memset(&addr, 0, sizeof(addr));
  ASSERT_EQ(getsockname(lfd, reinterpret_cast<struct sockaddr *>(&addr), &alen),
            0);
  const uint16_t port = ntohs(addr.sin_port);

  std::string client_got;
  std::thread client([port, &client_got]() {
    const int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) {
      return;
    }
    struct sockaddr_in peer;
    std::memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = htons(port);
    if (connect(cfd, reinterpret_cast<struct sockaddr *>(&peer),
                sizeof(peer)) != 0) {
      (void)close(cfd);
      return;
    }
    const char *req = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";
    if (!WriteAll(cfd, req, 36)) {
      (void)close(cfd);
      return;
    }
    (void)shutdown(cfd, SHUT_WR);
    client_got = DrainToEof(cfd);
    (void)close(cfd);
  });

  const int afd = accept(lfd, nullptr, nullptr);
  ASSERT_GE(afd, 0) << strerror(errno);
  EXPECT_EQ(odin_client_listen_handshake(afd), ODIN_CLIENT_LISTEN_OK);
  client.join();

  EXPECT_EQ(client_got.size(), kResp200Len);
  EXPECT_EQ(client_got, std::string(kResp200, kResp200Len));
  EXPECT_EQ(close(lfd), 0);
}
