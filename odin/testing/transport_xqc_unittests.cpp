// odin/testing/transport_xqc_unittests.cpp
//
// Unit and integration tests T1-T18 from §5 of
// odin/docs/rfc_016_xqc_stream_transport.md.

#include "odin/transport_xqc.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "odin/relay.h"
#include "odin/transport.h"
#if defined(ODIN_TRANSPORT_XQC_TESTING)
#include "odin/testing/transport_xqc_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

constexpr size_t kRelayCap = 65536;

struct RecvStep {
  ssize_t ret;
  uint8_t fin;
  std::string data;
};

struct SendStep {
  ssize_t ret;
};

struct SendRecord {
  std::string data;
  size_t size = 0;
  uint8_t fin = 0;
  bool null_data = false;
};

struct FakeStream {
  std::deque<RecvStep> recv_steps;
  std::deque<SendStep> send_steps;
  std::vector<SendRecord> sends;
  std::vector<void *> user_data_values;
  std::vector<size_t> recv_sizes;
  void *user_data = nullptr;
  int recv_calls = 0;
  int send_calls = 0;
  int set_user_data_calls = 0;
  bool fail_send_if_called = false;
};

xqc_stream_t *AsStream(FakeStream *s) {
  return reinterpret_cast<xqc_stream_t *>(s);
}

FakeStream *FromStream(xqc_stream_t *stream) {
  return reinterpret_cast<FakeStream *>(stream);
}

ssize_t FakeRecv(xqc_stream_t *stream, unsigned char *recv_buf,
                 size_t recv_buf_size, uint8_t *fin) {
  FakeStream *s = FromStream(stream);
  s->recv_calls += 1;
  s->recv_sizes.push_back(recv_buf_size);
  if (s->recv_steps.empty()) {
    ADD_FAILURE() << "unexpected fake xqc recv";
    *fin = 0;
    return -XQC_EAGAIN;
  }

  const RecvStep step = s->recv_steps.front();
  s->recv_steps.pop_front();
  *fin = step.fin;
  const size_t copy =
      std::min(step.data.size(), static_cast<size_t>(recv_buf_size));
  if (copy > 0) {
    const auto *data =
        reinterpret_cast<const unsigned char *>(step.data.data());
    std::copy_n(data, copy, recv_buf);
  }
  return step.ret;
}

ssize_t FakeSend(xqc_stream_t *stream, unsigned char *send_data,
                 size_t send_data_size, uint8_t fin) {
  FakeStream *s = FromStream(stream);
  s->send_calls += 1;
  if (s->fail_send_if_called) {
    ADD_FAILURE() << "unexpected fake xqc send";
  }
  SendRecord rec;
  rec.size = send_data_size;
  rec.fin = fin;
  rec.null_data = send_data == nullptr;
  if (send_data != nullptr && send_data_size > 0) {
    rec.data.assign(reinterpret_cast<const char *>(send_data), send_data_size);
  }
  s->sends.push_back(rec);
  if (!s->send_steps.empty()) {
    const SendStep step = s->send_steps.front();
    s->send_steps.pop_front();
    return step.ret;
  }
  return static_cast<ssize_t>(send_data_size);
}

void FakeSetUserData(xqc_stream_t *stream, void *user_data) {
  FakeStream *s = FromStream(stream);
  s->set_user_data_calls += 1;
  s->user_data = user_data;
  s->user_data_values.push_back(user_data);
}

void InstallFakeOps() {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  static const odin_xqc_stream_transport_test_ops_t kOps = {
      FakeRecv,
      FakeSend,
      FakeSetUserData,
  };
  odin_xqc_stream_transport_test_set_ops(&kOps);
#endif
}

void QueueRecv(FakeStream *s, const std::string &data, ssize_t ret,
               uint8_t fin) {
  s->recv_steps.push_back(RecvStep{ret, fin, data});
}

void QueueSend(FakeStream *s, ssize_t ret) {
  s->send_steps.push_back(SendStep{ret});
}

int FinSends(const FakeStream &s) {
  int count = 0;
  for (const SendRecord &rec : s.sends) {
    if (rec.fin == 1) {
      count += 1;
    }
  }
  return count;
}

std::string DataSends(const FakeStream &s) {
  std::string out;
  for (const SendRecord &rec : s.sends) {
    if (rec.fin == 0 && rec.size > 0) {
      out.append(rec.data);
    }
  }
  return out;
}

struct ReadObservation {
  odin_transport_io_t rc = ODIN_TRANSPORT_AGAIN;
  size_t n = 0;
  std::string data;
};

struct ReadyState {
  int calls = 0;
  std::vector<unsigned int> events;
  std::vector<ReadObservation> reads;
  odin_transport_io_t write_rc = ODIN_TRANSPORT_AGAIN;
  size_t write_n = 0;
  size_t read_len = 64;
  bool read_once_per_read = false;
  bool clear_interest_after_ok_read = false;
  bool destroy_on_read = false;
  bool write_on_first_write = false;
  int write_callbacks_seen = 0;
};

void RecordingReady(odin_transport_t *t, unsigned int events, void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events.push_back(events);

  if ((events & ODIN_TRANSPORT_READ) && s->destroy_on_read) {
    odin_transport_destroy(t);
    return;
  }

  if ((events & ODIN_TRANSPORT_READ) && s->read_once_per_read) {
    std::vector<char> buf(s->read_len);
    ReadObservation obs;
    obs.rc = odin_transport_read(t, buf.data(), buf.size(), &obs.n);
    if (obs.rc == ODIN_TRANSPORT_OK) {
      obs.data.assign(buf.data(), obs.n);
    }
    s->reads.push_back(obs);
    if (s->clear_interest_after_ok_read && obs.rc == ODIN_TRANSPORT_OK) {
      EXPECT_EQ(odin_transport_set_interest(t, 0), 0);
    }
  }

  if ((events & ODIN_TRANSPORT_WRITE) && s->write_on_first_write) {
    s->write_callbacks_seen += 1;
    if (s->write_callbacks_seen == 1) {
      s->write_rc = odin_transport_write(t, "abc", 3, &s->write_n);
    }
  }
}

void CreateTransport(FakeStream *stream, ReadyState *state,
                     odin_transport_t **out) {
  InstallFakeOps();
  ASSERT_EQ(odin_xqc_stream_transport_create(AsStream(stream), RecordingReady,
                                             state, out),
            0)
      << std::strerror(errno);
}

struct RelayDoneState {
  int calls = 0;
  odin_relay_status_t status = ODIN_RELAY_ERROR;
  int err = 0;
  bool destroy_in_cb = false;
};

void RelayDone(odin_relay_t *relay, odin_relay_status_t status, int err,
               void *user_data) {
  RelayDoneState *s = static_cast<RelayDoneState *>(user_data);
  s->calls += 1;
  s->status = status;
  s->err = err;
  if (s->destroy_in_cb) {
    odin_relay_destroy(relay);
  }
}

} // namespace

// T1 — Factory installs and destroy clears xquic stream user data.
TEST(OdinXqcStreamTransportTest, T1) {
  FakeStream stream;
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  ASSERT_EQ(stream.set_user_data_calls, 1);
  EXPECT_EQ(stream.user_data, t);
  odin_transport_destroy(t);
  ASSERT_EQ(stream.set_user_data_calls, 2);
  EXPECT_EQ(stream.user_data, nullptr);
  EXPECT_EQ(stream.recv_calls, 0);
  EXPECT_EQ(stream.send_calls, 0);
}

// T2 — Read returns buffered stream bytes.
TEST(OdinXqcStreamTransportTest, T2) {
  FakeStream stream;
  QueueRecv(&stream, "hello", 5, 0);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  char buf[64];
  size_t n = 0;
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_OK);
  EXPECT_EQ(n, 5u);
  EXPECT_EQ(std::string(buf, n), "hello");

  odin_transport_destroy(t);
}

// T3 — FIN-only read becomes EOF.
TEST(OdinXqcStreamTransportTest, T3) {
  FakeStream stream;
  QueueRecv(&stream, "", 0, 1);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  char buf[64];
  size_t n = 99;
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_EOF);
  EXPECT_EQ(n, 0u);

  odin_transport_destroy(t);
}

// T4 — Empty xquic receive queue becomes AGAIN.
TEST(OdinXqcStreamTransportTest, T4) {
  FakeStream stream;
  QueueRecv(&stream, "", -XQC_EAGAIN, 0);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  char buf[64];
  size_t n = 0;
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_AGAIN);
  EXPECT_EQ(odin_transport_error(t), 0);

  odin_transport_destroy(t);
}

// T5 — Read hard error maps to synchronous errno-style failure.
TEST(OdinXqcStreamTransportTest, T5) {
  FakeStream stream;
  QueueRecv(&stream, "", -XQC_ESTREAM_RESET, 0);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  char buf[64];
  size_t n = 0;
  errno = 0;
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n),
            ODIN_TRANSPORT_IO_ERROR);
  EXPECT_EQ(errno, EPIPE);
  EXPECT_EQ(odin_transport_error(t), 0);

  odin_transport_destroy(t);
}

// T6 — Write reports partial byte progress.
TEST(OdinXqcStreamTransportTest, T6) {
  FakeStream stream;
  QueueSend(&stream, 3);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  size_t n = 0;
  EXPECT_EQ(odin_transport_write(t, "abcdef", 6, &n), ODIN_TRANSPORT_OK);
  EXPECT_EQ(n, 3u);
  ASSERT_EQ(stream.sends.size(), 1u);
  EXPECT_EQ(stream.sends[0].fin, 0);
  EXPECT_EQ(stream.sends[0].size, 6u);
  EXPECT_EQ(stream.sends[0].data, "abcdef");

  odin_transport_destroy(t);
}

// T7 — Zero-length write is local; non-empty backpressure and hard error
// differ.
TEST(OdinXqcStreamTransportTest, T7) {
  FakeStream stream;
  stream.fail_send_if_called = true;
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  size_t n = 99;
  EXPECT_EQ(odin_transport_write(t, "", 0, &n), ODIN_TRANSPORT_OK);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(stream.send_calls, 0);

  stream.fail_send_if_called = false;
  QueueSend(&stream, -XQC_EAGAIN);
  EXPECT_EQ(odin_transport_write(t, "x", 1, &n), ODIN_TRANSPORT_AGAIN);
  EXPECT_EQ(odin_transport_error(t), 0);

  QueueSend(&stream, -XQC_EPARAM);
  errno = 0;
  EXPECT_EQ(odin_transport_write(t, "y", 1, &n), ODIN_TRANSPORT_IO_ERROR);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(odin_transport_error(t), 0);

  odin_transport_destroy(t);
}

// T8 — shutdown_write sends an immediate FIN-only block.
TEST(OdinXqcStreamTransportTest, T8) {
  FakeStream stream;
  QueueSend(&stream, 0);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  EXPECT_EQ(odin_transport_shutdown_write(t), 0);
  EXPECT_EQ(odin_transport_shutdown_write(t), 0);
  ASSERT_EQ(stream.sends.size(), 1u);
  EXPECT_EQ(stream.sends[0].fin, 1);
  EXPECT_EQ(stream.sends[0].size, 0u);
  EXPECT_TRUE(stream.sends[0].null_data);

  odin_transport_destroy(t);
}

// T9 — shutdown_write queues FIN across backpressure and reports async retry
// failure.
TEST(OdinXqcStreamTransportTest, T9) {
  {
    FakeStream stream;
    QueueSend(&stream, -XQC_EAGAIN);
    ReadyState state;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    EXPECT_EQ(odin_transport_shutdown_write(t), 0);
    EXPECT_EQ(odin_transport_error(t), 0);
    QueueSend(&stream, 0);
    EXPECT_EQ(odin_xqc_stream_transport_write_notify(AsStream(&stream), t),
              XQC_OK);
    EXPECT_EQ(FinSends(stream), 2);
    EXPECT_EQ(state.calls, 0);
    EXPECT_EQ(odin_transport_error(t), 0);

    odin_transport_destroy(t);
  }
  {
    FakeStream stream;
    QueueSend(&stream, -XQC_EAGAIN);
    ReadyState state;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    EXPECT_EQ(odin_transport_shutdown_write(t), 0);
    QueueSend(&stream, -XQC_EPARAM);
    EXPECT_EQ(odin_xqc_stream_transport_write_notify(AsStream(&stream), t),
              XQC_OK);
    ASSERT_EQ(state.events.size(), 1u);
    EXPECT_EQ(state.events[0], ODIN_TRANSPORT_ERROR);
    EXPECT_EQ(odin_transport_error(t), EIO);

    odin_transport_destroy(t);
  }
}

// T10 — READ notify honors interest, and WRITE enablement kicks the first send.
TEST(OdinXqcStreamTransportTest, T10) {
  FakeStream stream;
  ReadyState state;
  state.write_on_first_write = true;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
            XQC_OK);
  EXPECT_EQ(odin_xqc_stream_transport_write_notify(AsStream(&stream), t),
            XQC_OK);
  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_WRITE), 0);
  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_WRITE), 0);
  EXPECT_EQ(odin_xqc_stream_transport_write_notify(AsStream(&stream), t),
            XQC_OK);
  ASSERT_EQ(odin_transport_set_interest(t, 0), 0);
  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
            XQC_OK);
  EXPECT_EQ(odin_xqc_stream_transport_write_notify(AsStream(&stream), t),
            XQC_OK);

  ASSERT_EQ(state.events.size(), 3u);
  EXPECT_EQ(state.events[0], ODIN_TRANSPORT_READ);
  EXPECT_EQ(state.events[1], ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(state.events[2], ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(state.write_rc, ODIN_TRANSPORT_OK);
  EXPECT_EQ(state.write_n, 3u);
  ASSERT_EQ(stream.sends.size(), 1u);
  EXPECT_EQ(stream.sends[0].data, "abc");
  EXPECT_EQ(stream.sends[0].fin, 0);

  odin_transport_destroy(t);
}

// T11 — Invalid interest mask is rejected without clearing prior interest.
TEST(OdinXqcStreamTransportTest, T11) {
  FakeStream stream;
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
  errno = 0;
  EXPECT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_ERROR | 0x80u), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
            XQC_OK);
  ASSERT_EQ(state.events.size(), 1u);
  EXPECT_EQ(state.events[0], ODIN_TRANSPORT_READ);

  odin_transport_destroy(t);
}

// T12 — Receive capacity and impossible oversized returns are bounded.
TEST(OdinXqcStreamTransportTest, T12) {
  {
    FakeStream stream;
    QueueRecv(&stream, std::string(64, 'a'), 16, 0);
    ReadyState state;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    char buf[16];
    size_t n = 0;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_OK);
    EXPECT_EQ(n, sizeof(buf));
    ASSERT_EQ(stream.recv_sizes.size(), 1u);
    EXPECT_EQ(stream.recv_sizes[0], sizeof(buf));

    odin_transport_destroy(t);
  }
  {
    FakeStream stream;
    QueueRecv(&stream, std::string(16, 'b'), 17, 0);
    ReadyState state;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    char buf[16];
    size_t n = 0;
    errno = 0;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n),
              ODIN_TRANSPORT_IO_ERROR);
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(odin_transport_error(t), 0);
    ASSERT_EQ(stream.recv_sizes.size(), 1u);
    EXPECT_EQ(stream.recv_sizes[0], sizeof(buf));

    odin_transport_destroy(t);
  }
}

// T13 — Destroy clears user data; NULL notify and in-callback destroy are safe.
TEST(OdinXqcStreamTransportTest, T13) {
  {
    FakeStream stream;
    ReadyState state;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    odin_transport_destroy(t);
    ASSERT_EQ(stream.set_user_data_calls, 2);
    EXPECT_EQ(stream.user_data, nullptr);
    EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), nullptr),
              XQC_OK);
    EXPECT_EQ(
        odin_xqc_stream_transport_write_notify(AsStream(&stream), nullptr),
        XQC_OK);
    odin_xqc_stream_transport_closing_notify(AsStream(&stream),
                                             XQC_ESTREAM_RESET, nullptr);
    EXPECT_EQ(state.calls, 0);
  }
  {
    FakeStream stream;
    ReadyState state;
    state.destroy_on_read = true;
    odin_transport_t *t = nullptr;
    CreateTransport(&stream, &state, &t);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
    EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
              XQC_OK);
    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(stream.user_data, nullptr);
  }
}

// T14 — xquic stream closing surfaces Odin transport ERROR.
TEST(OdinXqcStreamTransportTest, T14) {
  FakeStream stream;
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  odin_xqc_stream_transport_closing_notify(AsStream(&stream), XQC_ESTREAM_RESET,
                                           t);
  ASSERT_EQ(state.events.size(), 1u);
  EXPECT_EQ(state.events[0], ODIN_TRANSPORT_ERROR);
  EXPECT_EQ(odin_transport_error(t), EPIPE);

  odin_transport_destroy(t);
}

// T15 — Final stream data with FIN returns bytes before EOF.
TEST(OdinXqcStreamTransportTest, T15) {
  FakeStream stream;
  QueueRecv(&stream, "bye", 3, 1);
  ReadyState state;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  char buf[64];
  size_t n = 0;
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_OK);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(std::string(buf, n), "bye");
  EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_EOF);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(stream.recv_calls, 1);

  odin_transport_destroy(t);
}

// T16 — Final stream data with FIN produces a follow-up READ readiness.
TEST(OdinXqcStreamTransportTest, T16) {
  FakeStream stream;
  QueueRecv(&stream, "bye", 3, 1);
  ReadyState state;
  state.read_once_per_read = true;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
            XQC_OK);
  ASSERT_EQ(state.events.size(), 2u);
  EXPECT_EQ(state.events[0], ODIN_TRANSPORT_READ);
  EXPECT_EQ(state.events[1], ODIN_TRANSPORT_READ);
  ASSERT_EQ(state.reads.size(), 2u);
  EXPECT_EQ(state.reads[0].rc, ODIN_TRANSPORT_OK);
  EXPECT_EQ(state.reads[0].n, 3u);
  EXPECT_EQ(state.reads[0].data, "bye");
  EXPECT_EQ(state.reads[1].rc, ODIN_TRANSPORT_EOF);
  EXPECT_EQ(state.reads[1].n, 0u);
  EXPECT_EQ(stream.recv_calls, 1);

  odin_transport_destroy(t);
}

// T17 — Latched EOF survives relay-style READ backpressure.
TEST(OdinXqcStreamTransportTest, T17) {
  FakeStream stream;
  QueueRecv(&stream, "12345678", 8, 1);
  ReadyState state;
  state.read_once_per_read = true;
  state.clear_interest_after_ok_read = true;
  state.read_len = 8;
  odin_transport_t *t = nullptr;
  CreateTransport(&stream, &state, &t);

  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&stream), t),
            XQC_OK);
  ASSERT_EQ(state.events.size(), 1u);
  ASSERT_EQ(state.reads.size(), 1u);
  EXPECT_EQ(state.reads[0].rc, ODIN_TRANSPORT_OK);
  EXPECT_EQ(state.reads[0].n, 8u);

  ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0);
  ASSERT_EQ(state.events.size(), 2u);
  ASSERT_EQ(state.reads.size(), 2u);
  EXPECT_EQ(state.reads[1].rc, ODIN_TRANSPORT_EOF);
  EXPECT_EQ(state.reads[1].n, 0u);
  EXPECT_EQ(stream.recv_calls, 1);

  odin_transport_destroy(t);
}

// T18 — Real relay first-WRITE kick and pending EOF recovery are
// re-entrant-safe.
TEST(OdinXqcStreamTransportTest, T18) {
  InstallFakeOps();
  FakeStream a_stream;
  FakeStream b_stream;
  QueueRecv(&b_stream, "", 0, 1);
  QueueRecv(&a_stream, std::string(kRelayCap, 'q'),
            static_cast<ssize_t>(kRelayCap), 1);

  RelayDoneState done;
  done.destroy_in_cb = true;
  odin_relay_t *relay = nullptr;
  ASSERT_EQ(odin_relay_create(RelayDone, &done, &relay), 0)
      << std::strerror(errno);

  odin_transport_t *a = nullptr;
  odin_transport_t *b = nullptr;
  ASSERT_EQ(odin_xqc_stream_transport_create(AsStream(&a_stream),
                                             odin_relay_ready, relay, &a),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_xqc_stream_transport_create(AsStream(&b_stream),
                                             odin_relay_ready, relay, &b),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(relay, a, b), 0) << std::strerror(errno);

  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&b_stream), b),
            XQC_OK);
  EXPECT_EQ(FinSends(a_stream), 1);

  EXPECT_EQ(odin_xqc_stream_transport_read_notify(AsStream(&a_stream), a),
            XQC_OK);

  EXPECT_EQ(DataSends(b_stream).size(), kRelayCap);
  EXPECT_EQ(DataSends(b_stream), std::string(kRelayCap, 'q'));
  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_RELAY_OK);
  EXPECT_EQ(done.err, 0);
  EXPECT_EQ(odin_xqc_stream_transport_test_interest(a), 0u);
  EXPECT_EQ(odin_xqc_stream_transport_test_interest(b), 0u);
  EXPECT_EQ(FinSends(b_stream), 1);
  EXPECT_EQ(FinSends(a_stream), 1);
  EXPECT_EQ(a_stream.recv_calls, 1);
  EXPECT_EQ(b_stream.recv_calls, 1);

  if (done.calls == 0) {
    odin_relay_destroy(relay);
  }
  odin_transport_destroy(a);
  odin_transport_destroy(b);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
