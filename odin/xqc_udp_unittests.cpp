// odin/xqc_udp_unittests.cpp
//
// Unit and integration tests T1-T20 from §5 of
// odin/docs/rfc_017_xqc_udp_event_driver.md.
//
// Each test is gated by the ODIN_XQC_UDP_RED environment variable during P1
// red verification: with the variable unset, the test SKIPs (so the default
// run stays green over the deliberately wrong P1 stub); with
// ODIN_XQC_UDP_RED=1, the test executes and asserts against the §5 row's
// expected behavior, so the stub trips the assertions. P2 removes the gate.

#include "odin/xqc_udp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#include "odin/udp.h"
#include "odin/udp_internal_test.h"
#if defined(ODIN_XQC_UDP_TESTING)
#include "odin/xqc_udp_internal_test.h"
#endif
#include <xquic/xquic.h>

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage,
// performance-no-int-to-ptr)

namespace {

#define ODIN_XQC_UDP_RED_OR_SKIP() ((void)0)

class XqcUdpRunDeadline {
public:
  template <typename Fn> static void Run(Fn fn) {
    const pid_t pid = fork();
    ASSERT_NE(pid, -1) << std::strerror(errno);
    if (pid == 0) {
      fn();
      _exit(::testing::Test::HasFailure() ? 1 : 0);
    }
    int wstatus = 0;
    bool exited = false;
    for (int i = 0; i < 300; ++i) {
      const pid_t got = waitpid(pid, &wstatus, WNOHANG);
      if (got == pid) {
        exited = true;
        break;
      }
      if (got == -1 && errno != EINTR) {
        break;
      }
      usleep(10000);
    }
    if (!exited) {
      kill(pid, SIGKILL);
      waitpid(pid, &wstatus, 0);
      FAIL() << "XqcUdpRunDeadline exceeded";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

void CloseFd(int fd) {
  if (fd >= 0) {
    EXPECT_EQ(close(fd), 0) << std::strerror(errno);
  }
}

struct sockaddr_in Loopback4(uint16_t port) {
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  return addr;
}

void MakeUdp4Peer(int *fd_out, struct sockaddr_in *out_addr) {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in addr = Loopback4(0);
  ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  *fd_out = fd;
  *out_addr = addr;
}

struct PacketRecord {
  std::vector<unsigned char> data;
  std::vector<unsigned char> local;
  socklen_t local_len = 0;
  std::vector<unsigned char> peer;
  socklen_t peer_len = 0;
  xqc_usec_t recv_time = 0;
  void *user_data = nullptr;
};

struct ContinueRecord {
  xqc_cid_t cid;
};

#if defined(ODIN_XQC_UDP_TESTING)
struct FakeXqc {
  xqc_set_event_timer_pt set_event_timer = nullptr;
  xqc_stateless_reset_pt stateless_reset = nullptr;
  xqc_socket_write_pt write_socket = nullptr;
  xqc_socket_write_ex_pt write_socket_ex = nullptr;
  xqc_socket_write_pt conn_send_packet_before_accept = nullptr;
  xqc_timestamp_pt monotonic_ts = nullptr;

  bool create_calls_set_event_timer = false;
  xqc_usec_t create_set_event_timer_wake = 0;
  bool create_returns_null = false;
  xqc_engine_t *engine_handle = nullptr;
  void *engine_user_data = nullptr;

  int engine_create_calls = 0;
  int engine_destroy_calls = 0;
  int finish_recv_calls = 0;
  int main_logic_calls = 0;

  std::vector<PacketRecord> packets;
  std::vector<ContinueRecord> continues;

  odin_event_loop_t *loop = nullptr;
  bool stop_on_finish_recv = false;
  bool stop_on_main_logic = false;
  bool stop_on_packet_process = false;

  std::function<void(xqc_engine_t *)> finish_recv_action;
  std::function<void(xqc_engine_t *)> main_logic_action;
  std::function<xqc_int_t(xqc_engine_t *, const xqc_cid_t *)>
      continue_send_action;
  std::function<xqc_int_t(
      xqc_engine_t *, const unsigned char *, size_t, const struct sockaddr *,
      socklen_t, const struct sockaddr *, socklen_t, xqc_usec_t, void *)>
      packet_process_action;

  xqc_usec_t fake_now = 0;
};

FakeXqc *g_fake = nullptr;

xqc_engine_t *FakeEngineCreate(xqc_engine_type_t engine_type,
                               const xqc_config_t *engine_config,
                               const xqc_engine_ssl_config_t *ssl_config,
                               const xqc_engine_callback_t *engine_callback,
                               const xqc_transport_callbacks_t *transport_cbs,
                               void *user_data) {
  (void)engine_type;
  (void)engine_config;
  (void)ssl_config;
  if (g_fake == nullptr) {
    return nullptr;
  }
  g_fake->engine_create_calls += 1;
  g_fake->engine_user_data = user_data;
  g_fake->set_event_timer = engine_callback->set_event_timer;
  g_fake->monotonic_ts = engine_callback->monotonic_ts;
  g_fake->stateless_reset = transport_cbs->stateless_reset;
  g_fake->write_socket = transport_cbs->write_socket;
  g_fake->write_socket_ex = transport_cbs->write_socket_ex;
  g_fake->conn_send_packet_before_accept =
      transport_cbs->conn_send_packet_before_accept;
  if (g_fake->create_calls_set_event_timer &&
      g_fake->set_event_timer != nullptr) {
    g_fake->set_event_timer(g_fake->create_set_event_timer_wake, user_data);
  }
  if (g_fake->create_returns_null) {
    return nullptr;
  }
  return g_fake->engine_handle;
}

void FakeEngineDestroy(xqc_engine_t *engine) {
  (void)engine;
  if (g_fake != nullptr) {
    g_fake->engine_destroy_calls += 1;
  }
}

xqc_int_t
FakePacketProcess(xqc_engine_t *engine, const unsigned char *packet_in_buf,
                  size_t packet_in_size, const struct sockaddr *local_addr,
                  socklen_t local_addrlen, const struct sockaddr *peer_addr,
                  socklen_t peer_addrlen, xqc_usec_t recv_time,
                  void *user_data) {
  if (g_fake == nullptr) {
    return 0;
  }
  PacketRecord rec;
  rec.data.assign(packet_in_buf, packet_in_buf + packet_in_size);
  if (local_addr != nullptr && local_addrlen > 0) {
    rec.local.assign(reinterpret_cast<const unsigned char *>(local_addr),
                     reinterpret_cast<const unsigned char *>(local_addr) +
                         local_addrlen);
  }
  rec.local_len = local_addrlen;
  if (peer_addr != nullptr && peer_addrlen > 0) {
    rec.peer.assign(reinterpret_cast<const unsigned char *>(peer_addr),
                    reinterpret_cast<const unsigned char *>(peer_addr) +
                        peer_addrlen);
  }
  rec.peer_len = peer_addrlen;
  rec.recv_time = recv_time;
  rec.user_data = user_data;
  g_fake->packets.push_back(rec);
  if (g_fake->packet_process_action) {
    return g_fake->packet_process_action(engine, packet_in_buf, packet_in_size,
                                         local_addr, local_addrlen, peer_addr,
                                         peer_addrlen, recv_time, user_data);
  }
  if (g_fake->stop_on_packet_process && g_fake->loop != nullptr) {
    odin_event_loop_stop(g_fake->loop);
  }
  return 0;
}

void FakeFinishRecv(xqc_engine_t *engine) {
  if (g_fake == nullptr) {
    return;
  }
  g_fake->finish_recv_calls += 1;
  if (g_fake->finish_recv_action) {
    g_fake->finish_recv_action(engine);
  }
  if (g_fake->stop_on_finish_recv && g_fake->loop != nullptr) {
    odin_event_loop_stop(g_fake->loop);
  }
}

void FakeMainLogic(xqc_engine_t *engine) {
  if (g_fake == nullptr) {
    return;
  }
  g_fake->main_logic_calls += 1;
  if (g_fake->main_logic_action) {
    g_fake->main_logic_action(engine);
  }
  if (g_fake->stop_on_main_logic && g_fake->loop != nullptr) {
    odin_event_loop_stop(g_fake->loop);
  }
}

xqc_int_t FakeContinueSend(xqc_engine_t *engine, const xqc_cid_t *cid) {
  if (g_fake == nullptr) {
    return 0;
  }
  ContinueRecord rec;
  rec.cid = *cid;
  g_fake->continues.push_back(rec);
  if (g_fake->continue_send_action) {
    return g_fake->continue_send_action(engine, cid);
  }
  return 0;
}

xqc_usec_t FakeNowUs() { return g_fake != nullptr ? g_fake->fake_now : 0; }

void InstallFakeXqc(FakeXqc *fake) {
  g_fake = fake;
  static const odin_xqc_udp_test_ops_t kOps = {
      FakeEngineCreate, FakeEngineDestroy, FakePacketProcess, FakeFinishRecv,
      FakeMainLogic,    FakeContinueSend,  FakeNowUs};
  odin_xqc_udp_test_set_ops(&kOps);
}

void ClearFakeXqc() {
  odin_xqc_udp_test_set_ops(nullptr);
  g_fake = nullptr;
}

xqc_engine_callback_t MakeEngineCallbacks() {
  xqc_engine_callback_t cbs;
  std::memset(&cbs, 0, sizeof(cbs));
  cbs.set_event_timer =
      reinterpret_cast<xqc_set_event_timer_pt>(reinterpret_cast<void *>(0x1));
  return cbs;
}

xqc_transport_callbacks_t MakeTransportCallbacks() {
  xqc_transport_callbacks_t cbs;
  std::memset(&cbs, 0, sizeof(cbs));
  cbs.stateless_reset =
      reinterpret_cast<xqc_stateless_reset_pt>(reinterpret_cast<void *>(0x2));
  cbs.write_socket =
      reinterpret_cast<xqc_socket_write_pt>(reinterpret_cast<void *>(0x3));
  cbs.write_socket_ex =
      reinterpret_cast<xqc_socket_write_ex_pt>(reinterpret_cast<void *>(0x4));
  cbs.conn_send_packet_before_accept =
      reinterpret_cast<xqc_socket_write_pt>(reinterpret_cast<void *>(0x5));
  return cbs;
}

odin_xqc_udp_config_t
MakeConfig(odin_event_loop_t *loop, const struct sockaddr *local,
           socklen_t local_len, const xqc_engine_callback_t *eng_cbs,
           const xqc_transport_callbacks_t *trans_cbs, void *app_user_data) {
  odin_xqc_udp_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.loop = loop;
  cfg.local_addr = local;
  cfg.local_addrlen = local_len;
  cfg.engine_type = XQC_ENGINE_CLIENT;
  cfg.engine_config = nullptr;
  cfg.ssl_config = nullptr;
  cfg.engine_callbacks = eng_cbs;
  cfg.transport_callbacks = trans_cbs;
  cfg.app_user_data = app_user_data;
  return cfg;
}

xqc_cid_t MakeCid(uint8_t tag) {
  xqc_cid_t cid;
  std::memset(&cid, 0, sizeof(cid));
  cid.cid_len = 8;
  cid.cid_buf[0] = tag;
  cid.cid_buf[7] = tag;
  cid.cid_seq_num = tag;
  return cid;
}

void GetUdpBoundAddr4(odin_xqc_udp_t *xu, struct sockaddr_in *out,
                      int *fd_out) {
  odin_udp_t *u = nullptr;
  ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0) << std::strerror(errno);
  const int fd = odin_udp_test_fd(u);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  *out = addr;
  *fd_out = fd;
}
#endif // ODIN_XQC_UDP_TESTING

} // namespace

#if defined(ODIN_XQC_UDP_TESTING)

TEST(OdinXqcUdpTest, T1) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    int test_app_pointer = 42;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, &test_app_pointer);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);

    EXPECT_EQ(fake.engine_create_calls, 1);
    EXPECT_EQ(fake.engine_user_data, odin_xqc_udp_xqc_user_data(xu));
    EXPECT_NE(fake.set_event_timer, reinterpret_cast<xqc_set_event_timer_pt>(
                                        reinterpret_cast<void *>(0x1)));
    EXPECT_NE(fake.stateless_reset, reinterpret_cast<xqc_stateless_reset_pt>(
                                        reinterpret_cast<void *>(0x2)));
    EXPECT_NE(fake.write_socket, reinterpret_cast<xqc_socket_write_pt>(
                                     reinterpret_cast<void *>(0x3)));
    EXPECT_NE(fake.write_socket_ex, reinterpret_cast<xqc_socket_write_ex_pt>(
                                        reinterpret_cast<void *>(0x4)));
    EXPECT_NE(
        fake.conn_send_packet_before_accept,
        reinterpret_cast<xqc_socket_write_pt>(reinterpret_cast<void *>(0x5)));
    EXPECT_EQ(odin_xqc_udp_engine(xu), fake.engine_handle);
    EXPECT_EQ(odin_xqc_udp_xqc_user_data(xu), xu);
    EXPECT_EQ(odin_xqc_udp_app_user_data(xu), &test_app_pointer);

    odin_udp_t *u = nullptr;
    ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0) << std::strerror(errno);
    const int fd = odin_udp_test_fd(u);
    ASSERT_GE(fd, 0);

    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    unsigned int mask = 99;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, ODIN_EVENT_READ);

    ASSERT_EQ(odin_xqc_udp_stop(xu), 0) << std::strerror(errno);
    mask = 99;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, 0u);

    odin_xqc_udp_destroy(xu);
    EXPECT_EQ(fake.engine_destroy_calls, 1);
    errno = 0;
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T2) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.fake_now = 12345;
    fake.stop_on_finish_recv = true;
    InstallFakeXqc(&fake);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);

    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    ASSERT_EQ(sendto(peer_fd, "pkt", 3, 0,
                     reinterpret_cast<struct sockaddr *>(&bound),
                     sizeof(bound)),
              3)
        << std::strerror(errno);

    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(fake.packets.size(), 1u);
    const PacketRecord &rec = fake.packets[0];
    EXPECT_EQ(std::string(rec.data.begin(), rec.data.end()), "pkt");
    EXPECT_EQ(rec.user_data, odin_xqc_udp_xqc_user_data(xu));
    EXPECT_EQ(rec.recv_time, fake.fake_now);
    ASSERT_EQ(rec.peer_len, sizeof(peer_addr));
    const struct sockaddr_in *recorded_peer =
        reinterpret_cast<const struct sockaddr_in *>(rec.peer.data());
    EXPECT_EQ(recorded_peer->sin_port, peer_addr.sin_port);
    EXPECT_EQ(recorded_peer->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(rec.local_len, sizeof(bound));
    const struct sockaddr_in *recorded_local =
        reinterpret_cast<const struct sockaddr_in *>(rec.local.data());
    EXPECT_EQ(recorded_local->sin_port, bound.sin_port);
    EXPECT_EQ(recorded_local->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(fake.finish_recv_calls, 1);

    odin_xqc_udp_destroy(xu);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T3) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.fake_now = 77;
    fake.stop_on_finish_recv = true;
    InstallFakeXqc(&fake);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);

    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);

    std::vector<unsigned char> payload(1400);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<unsigned char>(i & 0xffu);
    }
    ASSERT_EQ(sendto(peer_fd, payload.data(), payload.size(), 0,
                     reinterpret_cast<struct sockaddr *>(&bound),
                     sizeof(bound)),
              static_cast<ssize_t>(payload.size()))
        << std::strerror(errno);

    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    ASSERT_EQ(fake.packets.size(), 1u);
    EXPECT_EQ(fake.packets[0].data.size(), payload.size());
    EXPECT_EQ(std::memcmp(fake.packets[0].data.data(), payload.data(),
                          payload.size()),
              0);
    EXPECT_EQ(fake.packets[0].recv_time, fake.fake_now);
    EXPECT_EQ(fake.packets[0].local_len, sizeof(bound));
    EXPECT_EQ(fake.finish_recv_calls, 1);

    odin_xqc_udp_destroy(xu);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T4) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.stop_on_finish_recv = true;
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);

    for (int i = 0; i < 65; ++i) {
      const unsigned char one = static_cast<unsigned char>('A' + (i % 26));
      ASSERT_EQ(sendto(peer_fd, &one, 1, 0,
                       reinterpret_cast<struct sockaddr *>(&bound),
                       sizeof(bound)),
                1)
          << std::strerror(errno);
    }
    /* Allow the kernel to make all 65 datagrams available before the first
     * readiness pass. */
    usleep(20000);

    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.packets.size(), 64u);
    EXPECT_EQ(fake.finish_recv_calls, 1);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.packets.size(), 65u);
    EXPECT_EQ(fake.finish_recv_calls, 2);

    odin_xqc_udp_destroy(xu);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T5) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.stop_on_main_logic = true;
    InstallFakeXqc(&fake);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);

    ASSERT_NE(fake.set_event_timer, nullptr);
    odin_event_loop_test_set_now_us(loop, 1000000);
    fake.set_event_timer(500, xu);
    odin_event_loop_test_wait_record_t wait_rec;
    ASSERT_EQ(odin_event_loop_test_prepare_wait(loop, &wait_rec), 0)
        << std::strerror(errno);
    EXPECT_EQ(fake.main_logic_calls, 0);

    odin_event_loop_test_set_now_us(loop, 1000500);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.main_logic_calls, 1);

    // T5 also asserts the wait preparation reports a 500us armed timer.
    if (wait_rec.backend == ODIN_EVENT_LOOP_TEST_BACKEND_MACOS) {
      EXPECT_EQ(wait_rec.macos_kevent.timeout_is_null, 0);
      EXPECT_EQ(wait_rec.macos_kevent.rel_sec, 0);
      EXPECT_EQ(wait_rec.macos_kevent.rel_nsec, 500 * 1000);
    } else {
      EXPECT_EQ(wait_rec.linux_timerfd.armed, 1);
      const int64_t want_ns = 500 * 1000;
      const int64_t got_ns = wait_rec.linux_timerfd.abs_sec * 1000000000LL +
                             wait_rec.linux_timerfd.abs_nsec - 1000000LL * 1000;
      EXPECT_EQ(got_ns, want_ns);
    }

    odin_xqc_udp_destroy(xu);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T6) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.stop_on_main_logic = true;
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_NE(fake.set_event_timer, nullptr);
    fake.set_event_timer(60000000, xu);
    fake.set_event_timer(0, xu);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.main_logic_calls, 1);
    odin_event_loop_test_wait_record_t wait_rec;
    ASSERT_EQ(odin_event_loop_test_prepare_wait(loop, &wait_rec), 0)
        << std::strerror(errno);
    if (wait_rec.backend == ODIN_EVENT_LOOP_TEST_BACKEND_MACOS) {
      EXPECT_EQ(wait_rec.macos_kevent.timeout_is_null, 1);
    } else {
      EXPECT_EQ(wait_rec.linux_timerfd.armed, 0);
    }
    EXPECT_EQ(odin_xqc_udp_test_timer_active(xu), 0);

    odin_xqc_udp_destroy(xu);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T7) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);

    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    void *ud = odin_xqc_udp_xqc_user_data(xu);
    ASSERT_NE(fake.write_socket, nullptr);

    const ssize_t ok_ret = fake.write_socket(
        reinterpret_cast<const unsigned char *>("ok"), 2,
        reinterpret_cast<struct sockaddr *>(&peer_addr), sizeof(peer_addr), ud);
    EXPECT_EQ(ok_ret, 2);
    char buf[8];
    ssize_t got = recvfrom(peer_fd, buf, sizeof(buf), 0, nullptr, nullptr);
    EXPECT_EQ(got, 2);
    EXPECT_EQ(std::string(buf, 2), "ok");

    // EAGAIN path.
    odin_udp_t *u = nullptr;
    ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
    ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
    const ssize_t eagain_ret = fake.write_socket(
        reinterpret_cast<const unsigned char *>("blocked"), 7,
        reinterpret_cast<struct sockaddr *>(&peer_addr), sizeof(peer_addr), ud);
    EXPECT_EQ(eagain_ret, XQC_SOCKET_EAGAIN);
    EXPECT_EQ(odin_xqc_udp_test_write_blocked(xu), 1);
    const int fd = odin_udp_test_fd(u);
    unsigned int mask = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0);
    EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);

    // Oversized.
    std::vector<unsigned char> big(70000, 'x');
    const ssize_t big_ret = fake.write_socket(
        big.data(), big.size(), reinterpret_cast<struct sockaddr *>(&peer_addr),
        sizeof(peer_addr), ud);
    EXPECT_EQ(big_ret, XQC_SOCKET_ERROR);

    odin_xqc_udp_destroy(xu);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T8) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);

    auto make_xu = [&](odin_event_loop_t **loop_out, odin_xqc_udp_t **xu_out,
                       int *peer_fd_out, struct sockaddr_in *peer_addr_out,
                       struct sockaddr_in *local_addr_out,
                       socklen_t *local_len_out) {
      ASSERT_EQ(odin_event_loop_create(loop_out), 0) << std::strerror(errno);
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(*loop_out, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      ASSERT_EQ(odin_xqc_udp_create(&cfg, xu_out), 0) << std::strerror(errno);
      ASSERT_EQ(odin_xqc_udp_start(*xu_out), 0) << std::strerror(errno);
      MakeUdp4Peer(peer_fd_out, peer_addr_out);
      int bound_fd = -1;
      GetUdpBoundAddr4(*xu_out, local_addr_out, &bound_fd);
      *local_len_out = sizeof(*local_addr_out);
    };

    // success calls for each callback.
    {
      odin_event_loop_t *loop = nullptr;
      odin_xqc_udp_t *xu = nullptr;
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      struct sockaddr_in local_addr;
      socklen_t local_len = 0;
      make_xu(&loop, &xu, &peer_fd, &peer_addr, &local_addr, &local_len);
      void *ud = odin_xqc_udp_xqc_user_data(xu);

      ASSERT_NE(fake.write_socket_ex, nullptr);
      ASSERT_NE(fake.stateless_reset, nullptr);
      ASSERT_NE(fake.conn_send_packet_before_accept, nullptr);

      EXPECT_EQ(fake.write_socket_ex(
                    123, reinterpret_cast<const unsigned char *>("ex"), 2,
                    reinterpret_cast<struct sockaddr *>(&peer_addr),
                    sizeof(peer_addr), ud),
                2);
      char buf[8];
      EXPECT_EQ(recvfrom(peer_fd, buf, sizeof(buf), 0, nullptr, nullptr), 2);
      EXPECT_EQ(std::string(buf, 2), "ex");

      EXPECT_EQ(
          fake.stateless_reset(reinterpret_cast<const unsigned char *>("sr"), 2,
                               reinterpret_cast<struct sockaddr *>(&peer_addr),
                               sizeof(peer_addr),
                               reinterpret_cast<struct sockaddr *>(&local_addr),
                               local_len, ud),
          2);
      EXPECT_EQ(recvfrom(peer_fd, buf, sizeof(buf), 0, nullptr, nullptr), 2);
      EXPECT_EQ(std::string(buf, 2), "sr");

      EXPECT_EQ(fake.conn_send_packet_before_accept(
                    reinterpret_cast<const unsigned char *>("pa"), 2,
                    reinterpret_cast<struct sockaddr *>(&peer_addr),
                    sizeof(peer_addr), ud),
                2);
      EXPECT_EQ(recvfrom(peer_fd, buf, sizeof(buf), 0, nullptr, nullptr), 2);
      EXPECT_EQ(std::string(buf, 2), "pa");

      // EAGAIN for write_socket_ex -> EAGAIN + READ|WRITE.
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      EXPECT_EQ(fake.write_socket_ex(
                    7, reinterpret_cast<const unsigned char *>("z"), 1,
                    reinterpret_cast<struct sockaddr *>(&peer_addr),
                    sizeof(peer_addr), ud),
                XQC_SOCKET_EAGAIN);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 0;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);

      // Oversized.
      std::vector<unsigned char> big(70000, 'x');
      EXPECT_EQ(
          fake.write_socket_ex(0, big.data(), big.size(),
                               reinterpret_cast<struct sockaddr *>(&peer_addr),
                               sizeof(peer_addr), ud),
          XQC_SOCKET_ERROR);

      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
    }

    // EAGAIN for stateless_reset -> ERROR + no WRITE.
    {
      odin_event_loop_t *loop = nullptr;
      odin_xqc_udp_t *xu = nullptr;
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      struct sockaddr_in local_addr;
      socklen_t local_len = 0;
      make_xu(&loop, &xu, &peer_fd, &peer_addr, &local_addr, &local_len);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      EXPECT_EQ(
          fake.stateless_reset(reinterpret_cast<const unsigned char *>("x"), 1,
                               reinterpret_cast<struct sockaddr *>(&peer_addr),
                               sizeof(peer_addr),
                               reinterpret_cast<struct sockaddr *>(&local_addr),
                               local_len, ud),
          XQC_SOCKET_ERROR);
      EXPECT_EQ(odin_xqc_udp_test_write_blocked(xu), 0);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 99;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask & ODIN_EVENT_WRITE, 0u);

      // Oversized.
      std::vector<unsigned char> big(70000, 'x');
      EXPECT_EQ(
          fake.stateless_reset(big.data(), big.size(),
                               reinterpret_cast<struct sockaddr *>(&peer_addr),
                               sizeof(peer_addr),
                               reinterpret_cast<struct sockaddr *>(&local_addr),
                               local_len, ud),
          XQC_SOCKET_ERROR);

      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
    }

    // EAGAIN for conn_send_packet_before_accept -> ERROR + no WRITE.
    {
      odin_event_loop_t *loop = nullptr;
      odin_xqc_udp_t *xu = nullptr;
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      struct sockaddr_in local_addr;
      socklen_t local_len = 0;
      make_xu(&loop, &xu, &peer_fd, &peer_addr, &local_addr, &local_len);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      EXPECT_EQ(fake.conn_send_packet_before_accept(
                    reinterpret_cast<const unsigned char *>("x"), 1,
                    reinterpret_cast<struct sockaddr *>(&peer_addr),
                    sizeof(peer_addr), ud),
                XQC_SOCKET_ERROR);
      EXPECT_EQ(odin_xqc_udp_test_write_blocked(xu), 0);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 99;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask & ODIN_EVENT_WRITE, 0u);

      // Oversized.
      std::vector<unsigned char> big(70000, 'x');
      EXPECT_EQ(fake.conn_send_packet_before_accept(
                    big.data(), big.size(),
                    reinterpret_cast<struct sockaddr *>(&peer_addr),
                    sizeof(peer_addr), ud),
                XQC_SOCKET_ERROR);

      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
    }

    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T9) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.create_calls_set_event_timer = true;
    fake.create_set_event_timer_wake = 60000000;
    fake.create_returns_null = true;
    InstallFakeXqc(&fake);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *const sentinel =
        reinterpret_cast<odin_xqc_udp_t *>(static_cast<intptr_t>(-1));
    odin_xqc_udp_t *xu = sentinel;
    errno = 0;
    const int rc = odin_xqc_udp_create(&cfg, &xu);
    const int err = errno;
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(err, EIO);
    EXPECT_EQ(xu, sentinel);
    EXPECT_EQ(fake.engine_destroy_calls, 0);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);

    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T10) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    fake.packet_process_action = [&](xqc_engine_t *, const unsigned char *,
                                     size_t, const struct sockaddr *, socklen_t,
                                     const struct sockaddr *, socklen_t,
                                     xqc_usec_t, void *) -> xqc_int_t {
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      odin_xqc_udp_destroy(xu);
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      EXPECT_EQ(odin_xqc_udp_test_destroy_requested(xu), 1);
      odin_event_loop_stop(loop);
      return 0;
    };

    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    ASSERT_EQ(sendto(peer_fd, "p", 1, 0,
                     reinterpret_cast<struct sockaddr *>(&bound),
                     sizeof(bound)),
              1)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.finish_recv_calls, 0);
    EXPECT_EQ(fake.engine_destroy_calls, 1);

    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T11) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_NE(fake.set_event_timer, nullptr);
    fake.main_logic_action = [&](xqc_engine_t *) {
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      odin_xqc_udp_destroy(xu);
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      odin_event_loop_stop(loop);
    };
    fake.set_event_timer(0, xu);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(fake.main_logic_calls, 1);
    EXPECT_EQ(fake.engine_destroy_calls, 1);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T12) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    ASSERT_NE(fake.set_event_timer, nullptr);
    odin_event_loop_test_set_now_us(loop, 1000000);
    fake.set_event_timer(60000000, xu);
    ASSERT_EQ(odin_xqc_udp_stop(xu), 0) << std::strerror(errno);

    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    ASSERT_EQ(sendto(peer_fd, "z", 1, 0,
                     reinterpret_cast<struct sockaddr *>(&bound),
                     sizeof(bound)),
              1);
    odin_event_loop_test_set_now_us(loop, 1000000 + 60000001);

    auto post_stop = [](odin_event_loop_t *l, void *) {
      odin_event_loop_stop(l);
    };
    ASSERT_EQ(odin_event_post(loop, post_stop, nullptr), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(fake.packets.size(), 0u);
    EXPECT_EQ(fake.finish_recv_calls, 0);
    EXPECT_EQ(fake.main_logic_calls, 0);

    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    odin_udp_t *u = nullptr;
    ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
    const int fd = odin_udp_test_fd(u);
    unsigned int mask = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0);
    EXPECT_EQ(mask & ODIN_EVENT_READ, ODIN_EVENT_READ);

    odin_xqc_udp_destroy(xu);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

#if defined(__APPLE__)
TEST(OdinXqcUdpTest, T13) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                  loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ,
                  ENOSPC),
              0);
    errno = 0;
    const int rc1 = odin_xqc_udp_start(xu);
    const int err = errno;
    EXPECT_EQ(rc1, -1);
    EXPECT_EQ(err, ENOSPC);
    odin_udp_t *u = nullptr;
    ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
    odin_event_io_t *io_handle = nullptr;
    errno = 0;
    EXPECT_EQ(odin_udp_test_io(u, &io_handle), -1);

    EXPECT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    odin_event_io_t *io_after = nullptr;
    EXPECT_EQ(odin_udp_test_io(u, &io_after), 0) << std::strerror(errno);
    const int fd = odin_udp_test_fd(u);
    unsigned int mask = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0);
    EXPECT_EQ(mask, ODIN_EVENT_READ);

    odin_xqc_udp_destroy(xu);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}
#else
TEST(OdinXqcUdpTest, T13) { GTEST_SKIP() << "T13 is macOS-only (kqueue hook)"; }
#endif

TEST(OdinXqcUdpTest, T14) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    ASSERT_NE(fake.set_event_timer, nullptr);
    odin_event_loop_test_set_now_us(loop, 1000000);
    fake.set_event_timer(60000000, xu);

    odin_udp_t *u = nullptr;
    ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
    const int fd = odin_udp_test_fd(u);
    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);

    odin_xqc_udp_destroy(xu);
    EXPECT_EQ(fake.engine_destroy_calls, 1);
    errno = 0;
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);

    odin_event_loop_test_set_now_us(loop, 1000000 + 60000001);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    (void)sendto(peer_fd, "x", 1, 0,
                 reinterpret_cast<struct sockaddr *>(&bound), sizeof(bound));
    auto post_stop = [](odin_event_loop_t *l, void *) {
      odin_event_loop_stop(l);
    };
    ASSERT_EQ(odin_event_post(loop, post_stop, nullptr), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(fake.packets.size(), 0u);
    EXPECT_EQ(fake.finish_recv_calls, 0);
    EXPECT_EQ(fake.main_logic_calls, 0);

    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T15) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
    ASSERT_EQ(odin_xqc_udp_start(xu), 0) << std::strerror(errno);
    fake.finish_recv_action = [&](xqc_engine_t *) {
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      odin_xqc_udp_destroy(xu);
      EXPECT_EQ(fake.engine_destroy_calls, 0);
      EXPECT_EQ(odin_xqc_udp_test_destroy_requested(xu), 1);
      odin_event_loop_stop(loop);
    };
    struct sockaddr_in bound;
    int bound_fd = -1;
    GetUdpBoundAddr4(xu, &bound, &bound_fd);
    int peer_fd = -1;
    struct sockaddr_in peer_addr;
    MakeUdp4Peer(&peer_fd, &peer_addr);
    ASSERT_EQ(sendto(peer_fd, "fr", 2, 0,
                     reinterpret_cast<struct sockaddr *>(&bound),
                     sizeof(bound)),
              2);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(fake.packets.size(), 1u);
    EXPECT_EQ(fake.finish_recv_calls, 1);
    EXPECT_EQ(fake.engine_destroy_calls, 1);
    CloseFd(peer_fd);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

TEST(OdinXqcUdpTest, T16) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    // Destroy pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      xqc_cid_t cid = MakeCid(1);
      ASSERT_EQ(odin_xqc_udp_register_conn(xu, &cid), 0);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      EXPECT_EQ(
          fake.write_socket(reinterpret_cast<const unsigned char *>("blocked"),
                            7, reinterpret_cast<struct sockaddr *>(&peer_addr),
                            sizeof(peer_addr), ud),
          XQC_SOCKET_EAGAIN);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 0;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);
      fake.continue_send_action = [&](xqc_engine_t *,
                                      const xqc_cid_t *) -> xqc_int_t {
        unsigned int mid_mask = 99;
        EXPECT_EQ(
            odin_event_loop_test_kqueue_registered_mask(loop, fd, &mid_mask),
            0);
        EXPECT_EQ(mid_mask & ODIN_EVENT_WRITE, 0u);
        odin_xqc_udp_unregister_conn(xu, &cid);
        EXPECT_EQ(fake.engine_destroy_calls, 0);
        odin_xqc_udp_destroy(xu);
        EXPECT_EQ(fake.engine_destroy_calls, 0);
        return 0;
      };
      odin_event_io_t *io_handle = nullptr;
      ASSERT_EQ(odin_udp_test_io(u, &io_handle), 0);
      const odin_event_loop_test_ready_t entries[] = {
          {io_handle, ODIN_EVENT_WRITE},
      };
      ASSERT_EQ(odin_event_loop_test_dispatch_backend_events(loop, entries, 1),
                0);
      EXPECT_EQ(fake.engine_destroy_calls, 1);
      EXPECT_EQ(fake.continues.size(), 1u);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
    // Recovery pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x2000);
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      xqc_cid_t cid_a = MakeCid(0xAA);
      xqc_cid_t cid_b = MakeCid(0xBB);
      ASSERT_EQ(odin_xqc_udp_register_conn(xu, &cid_a), 0);
      ASSERT_EQ(odin_xqc_udp_register_conn(xu, &cid_b), 0);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      EXPECT_EQ(
          fake.write_socket(reinterpret_cast<const unsigned char *>("blocked"),
                            7, reinterpret_cast<struct sockaddr *>(&peer_addr),
                            sizeof(peer_addr), ud),
          XQC_SOCKET_EAGAIN);
      int retry_call = 0;
      fake.continue_send_action = [&](xqc_engine_t *,
                                      const xqc_cid_t *) -> xqc_int_t {
        retry_call += 1;
        if (retry_call == 1) {
          (void)fake.write_socket(
              reinterpret_cast<const unsigned char *>("retry"), 5,
              reinterpret_cast<struct sockaddr *>(&peer_addr),
              sizeof(peer_addr), ud);
        }
        return 0;
      };
      odin_event_io_t *io_handle = nullptr;
      ASSERT_EQ(odin_udp_test_io(u, &io_handle), 0);
      const odin_event_loop_test_ready_t entries[] = {
          {io_handle, ODIN_EVENT_WRITE},
      };
      ASSERT_EQ(odin_event_loop_test_dispatch_backend_events(loop, entries, 1),
                0);
      EXPECT_EQ(fake.continues.size(), 2u);
      char buf[16];
      const ssize_t got =
          recvfrom(peer_fd, buf, sizeof(buf), 0, nullptr, nullptr);
      EXPECT_EQ(got, 5);
      if (got == 5) {
        EXPECT_EQ(std::string(buf, 5), "retry");
      }
      const int fd = odin_udp_test_fd(u);
      unsigned int after_mask = 99;
      ASSERT_EQ(
          odin_event_loop_test_kqueue_registered_mask(loop, fd, &after_mask),
          0);
      EXPECT_EQ(after_mask, ODIN_EVENT_READ);
      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
  });
}

TEST(OdinXqcUdpTest, T17) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.stop_on_main_logic = true;
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
    ASSERT_NE(fake.set_event_timer, nullptr);
    odin_event_loop_test_set_now_us(loop, 1000000);
    fake.set_event_timer(0, xu);
    fake.main_logic_action = [&](xqc_engine_t *) {
      EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
      odin_event_loop_test_set_now_us(loop, 1000050);
      fake.set_event_timer(700, xu);
    };
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(fake.main_logic_calls, 1);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 1u);
    EXPECT_EQ(odin_xqc_udp_test_timer_active(xu), 1);
    odin_event_loop_test_wait_record_t wait_rec;
    ASSERT_EQ(odin_event_loop_test_prepare_wait(loop, &wait_rec), 0);
    if (wait_rec.backend == ODIN_EVENT_LOOP_TEST_BACKEND_MACOS) {
      EXPECT_EQ(wait_rec.macos_kevent.timeout_is_null, 0);
      EXPECT_EQ(wait_rec.macos_kevent.rel_nsec, 700 * 1000);
    } else {
      EXPECT_EQ(wait_rec.linux_timerfd.armed, 1);
    }
    odin_xqc_udp_destroy(xu);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

#if defined(__APPLE__)
TEST(OdinXqcUdpTest, T18) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    // Arming failure pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      xqc_cid_t cid = MakeCid(1);
      ASSERT_EQ(odin_xqc_udp_register_conn(xu, &cid), 0);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                    loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD,
                    ODIN_EVENT_WRITE, ENOSPC),
                0);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      const ssize_t rc =
          fake.write_socket(reinterpret_cast<const unsigned char *>("x"), 1,
                            reinterpret_cast<struct sockaddr *>(&peer_addr),
                            sizeof(peer_addr), ud);
      EXPECT_EQ(rc, XQC_SOCKET_ERROR);
      EXPECT_EQ(odin_xqc_udp_test_write_blocked(xu), 0);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 99;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask & ODIN_EVENT_WRITE, 0u);
      EXPECT_EQ(fake.continues.size(), 0u);
      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
    // Clearing failure pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x2000);
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      xqc_cid_t cid = MakeCid(1);
      ASSERT_EQ(odin_xqc_udp_register_conn(xu, &cid), 0);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      odin_udp_t *u = nullptr;
      ASSERT_EQ(odin_xqc_udp_test_udp(xu, &u), 0);
      ASSERT_EQ(odin_udp_test_fail_next_sendto(u, EAGAIN), 0);
      void *ud = odin_xqc_udp_xqc_user_data(xu);
      EXPECT_EQ(
          fake.write_socket(reinterpret_cast<const unsigned char *>("x"), 1,
                            reinterpret_cast<struct sockaddr *>(&peer_addr),
                            sizeof(peer_addr), ud),
          XQC_SOCKET_EAGAIN);
      const int fd = odin_udp_test_fd(u);
      unsigned int mask = 0;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);
      ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                    loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE,
                    ODIN_EVENT_WRITE, ENOSPC),
                0);
      odin_event_io_t *io_handle = nullptr;
      ASSERT_EQ(odin_udp_test_io(u, &io_handle), 0);
      const odin_event_loop_test_ready_t entries[] = {
          {io_handle, ODIN_EVENT_WRITE},
      };
      ASSERT_EQ(odin_event_loop_test_dispatch_backend_events(loop, entries, 1),
                0);
      EXPECT_EQ(fake.continues.size(), 0u);
      mask = 0;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);
      EXPECT_EQ(odin_xqc_udp_test_write_blocked(xu), 1);
      ASSERT_EQ(odin_event_loop_test_dispatch_backend_events(loop, entries, 1),
                0);
      EXPECT_EQ(fake.continues.size(), 1u);
      mask = 99;
      ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask),
                0);
      EXPECT_EQ(mask, ODIN_EVENT_READ);
      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
  });
}
#else
TEST(OdinXqcUdpTest, T18) { GTEST_SKIP() << "T18 is macOS-only (kqueue hook)"; }
#endif

static xqc_usec_t g_caller_clock_value = 0;
extern "C" xqc_usec_t TestCallerClock(void) { return g_caller_clock_value; }

TEST(OdinXqcUdpTest, T19) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    // Default monotonic_ts pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
      fake.stop_on_finish_recv = true;
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      eng_cbs.monotonic_ts = nullptr;
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      xqc_timestamp_pt pt = odin_xqc_udp_test_engine_monotonic_ts(xu);
      ASSERT_NE(pt, nullptr);
      fake.fake_now = 7000;
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      struct sockaddr_in bound;
      int bound_fd = -1;
      GetUdpBoundAddr4(xu, &bound, &bound_fd);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      ASSERT_EQ(sendto(peer_fd, "z", 1, 0,
                       reinterpret_cast<struct sockaddr *>(&bound),
                       sizeof(bound)),
                1);
      EXPECT_EQ(odin_event_loop_run(loop), 0);
      ASSERT_EQ(fake.packets.size(), 1u);
      EXPECT_EQ(fake.packets[0].recv_time, 7000u);
      EXPECT_EQ(pt(), 7000u);
      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
    // Caller-supplied monotonic_ts pass.
    {
      FakeXqc fake;
      fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x2000);
      fake.stop_on_finish_recv = true;
      InstallFakeXqc(&fake);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      fake.loop = loop;
      xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
      eng_cbs.monotonic_ts = TestCallerClock;
      xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
      struct sockaddr_in local = Loopback4(0);
      odin_xqc_udp_config_t cfg =
          MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                     sizeof(local), &eng_cbs, &trans_cbs, nullptr);
      odin_xqc_udp_t *xu = nullptr;
      ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
      xqc_timestamp_pt pt = odin_xqc_udp_test_engine_monotonic_ts(xu);
      EXPECT_EQ(pt, TestCallerClock);
      g_caller_clock_value = 9000;
      ASSERT_EQ(odin_xqc_udp_start(xu), 0);
      struct sockaddr_in bound;
      int bound_fd = -1;
      GetUdpBoundAddr4(xu, &bound, &bound_fd);
      int peer_fd = -1;
      struct sockaddr_in peer_addr;
      MakeUdp4Peer(&peer_fd, &peer_addr);
      ASSERT_EQ(sendto(peer_fd, "z", 1, 0,
                       reinterpret_cast<struct sockaddr *>(&bound),
                       sizeof(bound)),
                1);
      EXPECT_EQ(odin_event_loop_run(loop), 0);
      ASSERT_EQ(fake.packets.size(), 1u);
      EXPECT_EQ(fake.packets[0].recv_time, 9000u);
      odin_xqc_udp_destroy(xu);
      CloseFd(peer_fd);
      odin_event_loop_destroy(loop);
      ClearFakeXqc();
    }
  });
}

TEST(OdinXqcUdpTest, T20) {
  ODIN_XQC_UDP_RED_OR_SKIP();
  XqcUdpRunDeadline::Run([] {
    FakeXqc fake;
    fake.engine_handle = reinterpret_cast<xqc_engine_t *>(0x1000);
    fake.stop_on_main_logic = true;
    InstallFakeXqc(&fake);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    fake.loop = loop;
    xqc_engine_callback_t eng_cbs = MakeEngineCallbacks();
    xqc_transport_callbacks_t trans_cbs = MakeTransportCallbacks();
    struct sockaddr_in local = Loopback4(0);
    odin_xqc_udp_config_t cfg =
        MakeConfig(loop, reinterpret_cast<struct sockaddr *>(&local),
                   sizeof(local), &eng_cbs, &trans_cbs, nullptr);
    odin_xqc_udp_t *xu = nullptr;
    ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0);
    ASSERT_NE(fake.set_event_timer, nullptr);
    ASSERT_EQ(odin_event_loop_test_fail_next_timer_start(loop, ENOMEM), 0);
    fake.set_event_timer(1000, xu);
    EXPECT_EQ(odin_xqc_udp_test_timer_active(xu), 0);
    EXPECT_EQ(odin_xqc_udp_test_last_timer_errno(xu), ENOMEM);
    EXPECT_EQ(fake.main_logic_calls, 0);
    fake.set_event_timer(0, xu);
    EXPECT_EQ(odin_xqc_udp_test_timer_active(xu), 1);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(fake.main_logic_calls, 1);
    odin_xqc_udp_destroy(xu);
    odin_event_loop_destroy(loop);
    ClearFakeXqc();
  });
}

#endif // ODIN_XQC_UDP_TESTING

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage,
// performance-no-int-to-ptr)
