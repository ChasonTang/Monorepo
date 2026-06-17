// odin/testing/transport_unittests.cpp
//
// Unit test T1 from §6 of odin/docs/rfc_013_transport_interface_fd_impl.md.
//
// Exercises the abstract odin_transport_* dispatchers against a test-local fake
// vtable (no fd, no loop).

#include "odin/transport.h"

#include <cstddef>

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// A fake implementation whose six slots record their call and arguments and
// return distinctive sentinels. base is first so each slot recovers the fake
// from the odin_transport_t * the dispatcher forwards.
struct FakeTransport {
  odin_transport_t base;
  int read_calls;
  int write_calls;
  int shutdown_calls;
  int set_interest_calls;
  int error_calls;
  int destroy_calls;
  void *last_read_buf;
  size_t last_read_len;
  size_t *last_read_out_n;
  const void *last_write_buf;
  size_t last_write_len;
  size_t *last_write_out_n;
  unsigned int last_set_interest_events;
};

// Sentinels returned by the fake slots; a forwarding dispatcher returns these
// verbatim.
constexpr odin_transport_io_t kReadSentinel = ODIN_TRANSPORT_EOF;
constexpr odin_transport_io_t kWriteSentinel = ODIN_TRANSPORT_AGAIN;
constexpr size_t kReadOutN = 123;
constexpr size_t kWriteOutN = 456;
constexpr int kShutdownSentinel = 7;
constexpr int kSetInterestSentinel = 9;
constexpr int kErrorSentinel = 13;

odin_transport_io_t FakeRead(odin_transport_t *t, void *buf, size_t len,
                             size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->read_calls += 1;
  f->last_read_buf = buf;
  f->last_read_len = len;
  f->last_read_out_n = out_n;
  *out_n = kReadOutN;
  return kReadSentinel;
}

odin_transport_io_t FakeWrite(odin_transport_t *t, const void *buf, size_t len,
                              size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->write_calls += 1;
  f->last_write_buf = buf;
  f->last_write_len = len;
  f->last_write_out_n = out_n;
  *out_n = kWriteOutN;
  return kWriteSentinel;
}

int FakeShutdownWrite(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->shutdown_calls += 1;
  return kShutdownSentinel;
}

int FakeSetInterest(odin_transport_t *t, unsigned int events) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->set_interest_calls += 1;
  f->last_set_interest_events = events;
  return kSetInterestSentinel;
}

int FakeError(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->error_calls += 1;
  return kErrorSentinel;
}

void FakeDestroy(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->destroy_calls += 1;
}

const odin_transport_vtable_t kFakeVtable = {
    FakeRead,        FakeWrite, FakeShutdownWrite,
    FakeSetInterest, FakeError, FakeDestroy,
};

} // namespace

// T1 — Dispatchers forward to the installed vtable.
TEST(OdinTransportTest, T1) {
  FakeTransport fake = {};
  fake.base.vt = &kFakeVtable;
  odin_transport_t *t = &fake.base;

  char rbuf[64];
  size_t rn = 0;
  EXPECT_EQ(odin_transport_read(t, rbuf, sizeof(rbuf), &rn), kReadSentinel);
  EXPECT_EQ(fake.read_calls, 1);
  EXPECT_EQ(fake.last_read_buf, rbuf);
  EXPECT_EQ(fake.last_read_len, sizeof(rbuf));
  EXPECT_EQ(fake.last_read_out_n, &rn);
  EXPECT_EQ(rn, kReadOutN);

  const char wbuf[] = "hi";
  size_t wn = 0;
  EXPECT_EQ(odin_transport_write(t, wbuf, sizeof(wbuf), &wn), kWriteSentinel);
  EXPECT_EQ(fake.write_calls, 1);
  EXPECT_EQ(fake.last_write_buf, wbuf);
  EXPECT_EQ(fake.last_write_len, sizeof(wbuf));
  EXPECT_EQ(fake.last_write_out_n, &wn);
  EXPECT_EQ(wn, kWriteOutN);

  EXPECT_EQ(odin_transport_shutdown_write(t), kShutdownSentinel);
  EXPECT_EQ(fake.shutdown_calls, 1);

  const unsigned int interest = ODIN_TRANSPORT_READ | ODIN_TRANSPORT_WRITE;
  EXPECT_EQ(odin_transport_set_interest(t, interest), kSetInterestSentinel);
  EXPECT_EQ(fake.set_interest_calls, 1);
  EXPECT_EQ(fake.last_set_interest_events, interest);

  EXPECT_EQ(odin_transport_error(t), kErrorSentinel);
  EXPECT_EQ(fake.error_calls, 1);

  odin_transport_destroy(t);
  EXPECT_EQ(fake.destroy_calls, 1);

  // destroy(NULL) is a no-op: it must invoke no slot and must not crash.
  odin_transport_destroy(nullptr);
  EXPECT_EQ(fake.destroy_calls, 1);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
