// odin/connect_session_unittests.cpp
//
// Unit tests T1-T27 from §5 of odin/docs/rfc_018_connect_session.md.
//
// T1-T26 drive the session against a test-local fake transport (no fd, no
// event loop): the fake embeds odin_transport_t as its first member, serves
// scripted read/write/error results, records every set_interest call
// (asserted zero in every T1-T26 row, the G4 check), refuses destroy calls
// (asserted zero in every T1-T26 row, the never-owns-transport check), and
// records every read call's (len, n_served_before_this_call) pair for T15/T16
// to assert the §4 S1 buffer-length invariant per call. Readiness is injected
// by calling odin_connect_session_drive(s, &fake.base, events) directly.
//
// T27 instantiates no fake transport at all —
// odin_connect_session_create_client takes no transport argument and does no
// I/O on the host_len validation early-return path.

#include "odin/connect_session.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "odin/protocol.h"
#include "odin/transport.h"

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// Scripted read step.
enum ReadKind { kReadData, kReadEof, kReadAgain, kReadFail };

struct ReadStep {
  ReadKind kind;
  std::string data;
  int err;
};

ReadStep ReadData(const std::string &s) { return ReadStep{kReadData, s, 0}; }
ReadStep ReadEof() { return ReadStep{kReadEof, std::string(), 0}; }
ReadStep ReadAgain() { return ReadStep{kReadAgain, std::string(), 0}; }
ReadStep ReadFail(int e) { return ReadStep{kReadFail, std::string(), e}; }

// Scripted write step. Each step describes one odin_transport_write() call's
// outcome.
enum WriteKind { kWriteAccept, kWriteAgain, kWriteFail };

struct WriteStep {
  WriteKind kind;
  size_t accept_n; // bytes consumed from buf when kWriteAccept; 0 = whole len
  int err;
};

WriteStep WriteAcceptAll() { return WriteStep{kWriteAccept, 0, 0}; }
WriteStep WriteAcceptN(size_t n) { return WriteStep{kWriteAccept, n, 0}; }
WriteStep WriteAgain() { return WriteStep{kWriteAgain, 0, 0}; }
WriteStep WriteFail(int e) { return WriteStep{kWriteFail, 0, e}; }

struct ReadRecord {
  size_t len;
  size_t buf_used_at_call_time;
};

struct FakeTransport {
  odin_transport_t base;

  std::deque<ReadStep> reads;
  std::deque<WriteStep> writes;

  // Running tally of bytes the fake has previously returned on kReadData
  // (i.e., on ODIN_TRANSPORT_OK returns) for this session. Mirrors s.buf_used
  // at call time exactly (the session's only writer of s.buf_used is the
  // do_read_resp / do_read_req `s.buf_used += n` after each OK read).
  size_t served_total = 0;

  std::vector<ReadRecord> read_records;
  std::string written;

  int error_result = 0;
  int error_calls = 0;
  int read_calls = 0;
  int set_interest_calls = 0;
  int destroy_calls = 0;
};

odin_transport_io_t FakeReadFn(odin_transport_t *t, void *buf, size_t len,
                               size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->read_calls += 1;
  f->read_records.push_back(ReadRecord{len, f->served_total});

  if (f->reads.empty()) {
    return ODIN_TRANSPORT_AGAIN;
  }
  const ReadStep s = f->reads.front();
  f->reads.pop_front();
  switch (s.kind) {
  case kReadData: {
    size_t n = s.data.size();
    if (n > len) {
      n = len;
    }
    std::memcpy(buf, s.data.data(), n);
    *out_n = n;
    f->served_total += n;
    return ODIN_TRANSPORT_OK;
  }
  case kReadEof:
    *out_n = 0;
    return ODIN_TRANSPORT_EOF;
  case kReadAgain:
    return ODIN_TRANSPORT_AGAIN;
  case kReadFail:
    errno = s.err;
    return ODIN_TRANSPORT_IO_ERROR;
  }
  return ODIN_TRANSPORT_AGAIN;
}

odin_transport_io_t FakeWriteFn(odin_transport_t *t, const void *buf,
                                size_t len, size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  if (f->writes.empty()) {
    // Default: absorb everything.
    f->written.append(static_cast<const char *>(buf), len);
    *out_n = len;
    return ODIN_TRANSPORT_OK;
  }
  const WriteStep s = f->writes.front();
  f->writes.pop_front();
  switch (s.kind) {
  case kWriteAccept: {
    size_t n = s.accept_n == 0 ? len : s.accept_n;
    if (n > len) {
      n = len;
    }
    f->written.append(static_cast<const char *>(buf), n);
    *out_n = n;
    return ODIN_TRANSPORT_OK;
  }
  case kWriteAgain:
    return ODIN_TRANSPORT_AGAIN;
  case kWriteFail:
    errno = s.err;
    return ODIN_TRANSPORT_IO_ERROR;
  }
  return ODIN_TRANSPORT_AGAIN;
}

int FakeShutdownWriteFn(odin_transport_t *t) {
  (void)t;
  return 0;
}

int FakeSetInterestFn(odin_transport_t *t, unsigned int events) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  (void)events;
  f->set_interest_calls += 1;
  return 0;
}

int FakeErrorFn(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->error_calls += 1;
  return f->error_result;
}

void FakeDestroyFn(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->destroy_calls += 1;
}

const odin_transport_vtable_t kFakeVtable = {
    FakeReadFn,        FakeWriteFn, FakeShutdownWriteFn,
    FakeSetInterestFn, FakeErrorFn, FakeDestroyFn,
};

FakeTransport MakeFake() {
  FakeTransport f{};
  f.base.vt = &kFakeVtable;
  return f;
}

// done callback recorder.
struct DoneRecord {
  int calls = 0;
  odin_connect_session_status_t status = ODIN_CONNECT_SESSION_OK;
  int err = 0;

  bool capture_client = false;
  bool destroy_in_cb = false;

  uint16_t captured_error_code = 0;
  const uint8_t *captured_tail_ptr = nullptr;
  size_t captured_tail_len = 0;
};

void OnDone(odin_connect_session_t *s, odin_connect_session_status_t status,
            int err, void *user_data) {
  DoneRecord *r = static_cast<DoneRecord *>(user_data);
  r->calls += 1;
  r->status = status;
  r->err = err;
  if (r->capture_client && status == ODIN_CONNECT_SESSION_OK) {
    r->captured_error_code = odin_connect_session_client_error_code(s);
    odin_connect_session_client_tail(s, &r->captured_tail_ptr,
                                     &r->captured_tail_len);
  }
  if (r->destroy_in_cb) {
    odin_connect_session_destroy(s);
  }
}

struct ReqDecodedRecord {
  int calls = 0;
};

// Combined user_data for Server-mode rows: on_req_decoded and on_done share
// one user_data slot, so tests that need to record both events bundle their
// recorders behind one pointer.
struct Combo {
  ReqDecodedRecord *rd;
  DoneRecord *dn;
};

void OnComboReqDecoded(odin_connect_session_t *s, void *user_data) {
  (void)s;
  static_cast<Combo *>(user_data)->rd->calls += 1;
}

void OnComboDone(odin_connect_session_t *s,
                 odin_connect_session_status_t status, int err,
                 void *user_data) {
  Combo *c = static_cast<Combo *>(user_data);
  DoneRecord *r = c->dn;
  r->calls += 1;
  r->status = status;
  r->err = err;
  if (r->destroy_in_cb) {
    odin_connect_session_destroy(s);
  }
}

// Build the 16-byte REQ that odin_proto_encode_connect_req flattens for the
// given (host, port).
std::string EncodedReq(const std::string &host, uint16_t port) {
  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  const odin_proto_status_t st = odin_proto_encode_connect_req(
      host.data(), host.size(), port, iov, hdr, portbe);
  EXPECT_EQ(st, ODIN_PROTO_OK);
  std::string out;
  for (int i = 0; i < 3; ++i) {
    out.append(static_cast<const char *>(iov[i].base), iov[i].len);
  }
  return out;
}

} // namespace

// T1 — Client OK round-trip, no tail; destroy-from-on_done.
TEST(OdinConnectSessionTest, T1) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  done.capture_client = true;
  done.destroy_in_cb = true;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("example.com", 11, 443, OnDone,
                                               &done, &s),
            0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);

  t.writes.push_back(WriteAcceptAll());
  const odin_connect_session_drive_t r1 =
      odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(r1, ODIN_CONNECT_SESSION_DRIVE_CONTINUE);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadData(std::string("\x01\x02\x00\x00", 4)));
  t.reads.push_back(ReadAgain());

  const odin_connect_session_drive_t r2 =
      odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  EXPECT_EQ(r2, ODIN_CONNECT_SESSION_DRIVE_DONE);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(done.err, 0);
  EXPECT_EQ(done.captured_error_code, 0);
  EXPECT_EQ(done.captured_tail_len, 0u);

  EXPECT_EQ(t.written, EncodedReq("example.com", 443));
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);
}

// T2 — Client OK with 7 tail bytes pipelined behind the RESP.
TEST(OdinConnectSessionTest, T2) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  done.capture_client = true;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  const std::string blob("\x01\x02\x00\x00\xDE\xAD\xBE\xEF\xCA\xFE\xBA", 11);
  t.reads.push_back(ReadData(blob));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(done.err, 0);
  EXPECT_EQ(done.captured_error_code, 0);
  ASSERT_EQ(done.captured_tail_len, 7u);
  EXPECT_EQ(
      std::memcmp(done.captured_tail_ptr, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA", 7),
      0);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T3 — Client decoder rejects bad version.
TEST(OdinConnectSessionTest, T3) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadData(std::string("\xFF\x02\x00\x00", 4)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPROTO);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T4 — Client decoder rejects bad frame_type.
TEST(OdinConnectSessionTest, T4) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadData(std::string("\x01\x03\x00\x00", 4)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPROTO);

  odin_connect_session_destroy(s);
}

// T5 — Client RESP arrives across multiple READ readiness deliveries.
TEST(OdinConnectSessionTest, T5) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  done.capture_client = true;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadData(std::string("\x01", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);
  EXPECT_EQ(done.calls, 0);

  t.reads.push_back(ReadData(std::string("\x02", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);
  EXPECT_EQ(done.calls, 0);

  t.reads.push_back(ReadData(std::string("\x12\x34\x99", 3)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(done.captured_error_code, 0x1234);
  ASSERT_EQ(done.captured_tail_len, 1u);
  EXPECT_EQ(done.captured_tail_ptr[0], 0x99);

  odin_connect_session_destroy(s);
}

// T6 — Client write IO_ERROR.
TEST(OdinConnectSessionTest, T6) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);

  t.writes.push_back(WriteFail(EPIPE));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPIPE);

  odin_connect_session_destroy(s);
}

// T7 — Client read IO_ERROR.
TEST(OdinConnectSessionTest, T7) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadFail(ECONNRESET));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);

  odin_connect_session_destroy(s);
}

// T8 — Client ERROR readiness classified through odin_transport_error.
TEST(OdinConnectSessionTest, T8) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  t.error_result = ECONNRESET;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base,
                             ODIN_TRANSPORT_READ | ODIN_TRANSPORT_ERROR);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);
  EXPECT_EQ(t.error_calls, 1);
  EXPECT_EQ(t.read_calls, 1);

  odin_connect_session_destroy(s);
}

// T9 — Client EOF before RESP completes.
TEST(OdinConnectSessionTest, T9) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadEof());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);

  odin_connect_session_destroy(s);
}

// T10 — Server OK round-trip, no tail; destroy-from-on_done.
TEST(OdinConnectSessionTest, T10) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  done.destroy_in_cb = true;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);

  const std::string req = EncodedReq("example.com", 443);
  t.reads.push_back(ReadData(req));
  t.reads.push_back(ReadAgain());

  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(req_decoded.calls, 1);
  const char *h = nullptr;
  size_t hl = 0;
  odin_connect_session_server_host(s, &h, &hl);
  ASSERT_EQ(hl, 11u);
  EXPECT_EQ(std::memcmp(h, "example.com", 11), 0);
  EXPECT_EQ(odin_connect_session_server_port(s), 443);
  EXPECT_EQ(odin_connect_session_wants(s), 0u);

  odin_connect_session_server_set_error_code(s, 0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);

  t.writes.push_back(WriteAcceptAll());
  const odin_connect_session_drive_t r2 =
      odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(r2, ODIN_CONNECT_SESSION_DRIVE_DONE);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(done.err, 0);
  EXPECT_EQ(t.written, std::string("\x01\x02\x00\x00", 4));
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);
}

// T11 — Server REQ across two reads; non-OK error_code.
TEST(OdinConnectSessionTest, T11) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadData(std::string("\x01\x01\x03"
                                         "a",
                                         4)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  EXPECT_EQ(req_decoded.calls, 0);

  t.reads.push_back(ReadData(std::string("bc\x01\xBB", 4)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  EXPECT_EQ(req_decoded.calls, 1);

  const char *h = nullptr;
  size_t hl = 0;
  odin_connect_session_server_host(s, &h, &hl);
  ASSERT_EQ(hl, 3u);
  EXPECT_EQ(std::memcmp(h, "abc", 3), 0);
  EXPECT_EQ(odin_connect_session_server_port(s), 0x01BB);

  odin_connect_session_server_set_error_code(s, 0x000A);
  t.writes.push_back(WriteAcceptAll());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(t.written, std::string("\x01\x02\x00\x0A", 4));

  odin_connect_session_destroy(s);
}

// T12 — Server decoder rejects bad host_len (0).
TEST(OdinConnectSessionTest, T12) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadData(std::string("\x01\x01\x00", 3)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPROTO);
  EXPECT_EQ(req_decoded.calls, 0);

  odin_connect_session_destroy(s);
}

// T13 — Server decoder rejects bad version.
TEST(OdinConnectSessionTest, T13) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadData(std::string("\xFF\x01\x03"
                                         "abc"
                                         "\x01\xBB",
                                         8)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPROTO);
  EXPECT_EQ(req_decoded.calls, 0);

  odin_connect_session_destroy(s);
}

// T14 — Server CONNECT_RESP write splits across two WRITE readiness
// deliveries.
TEST(OdinConnectSessionTest, T14) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  // Complete REQ read as in T10.
  const std::string req = EncodedReq("example.com", 443);
  t.reads.push_back(ReadData(req));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  ASSERT_EQ(req_decoded.calls, 1);

  odin_connect_session_server_set_error_code(s, 0xABCD);

  t.writes.push_back(WriteAcceptN(2));
  t.writes.push_back(WriteAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(done.calls, 0);

  t.writes.push_back(WriteAcceptN(2));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(t.written, std::string("\x01\x02\xAB\xCD", 4));

  odin_connect_session_destroy(s);
}

// T15 — Client read length is bounded to 260 - buf_used (S1 enforcement).
TEST(OdinConnectSessionTest, T15) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  done.capture_client = true;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  t.reads.push_back(ReadData(std::string("\x01", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadData(std::string("\x02", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadData(std::string("\x00\x00", 2)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_OK);
  EXPECT_EQ(done.captured_error_code, 0x0000);
  EXPECT_EQ(done.captured_tail_len, 0u);

  // Every recorded (len, buf_used) pair satisfies len == 260 - buf_used.
  ASSERT_FALSE(t.read_records.empty());
  for (const ReadRecord &rec : t.read_records) {
    EXPECT_EQ(rec.len, ODIN_PROTO_CONNECT_REQ_MAX - rec.buf_used_at_call_time)
        << "len=" << rec.len << " buf_used=" << rec.buf_used_at_call_time;
  }
  // Specifically the three OK-returning reads recorded (260, 0), (259, 1),
  // (258, 2).
  size_t ok_seen = 0;
  for (const ReadRecord &rec : t.read_records) {
    if (rec.buf_used_at_call_time == 0 && rec.len == 260) {
      ok_seen += (ok_seen == 0) ? 1 : 0;
    } else if (rec.buf_used_at_call_time == 1 && rec.len == 259) {
      ok_seen += (ok_seen == 1) ? 1 : 0;
    } else if (rec.buf_used_at_call_time == 2 && rec.len == 258) {
      ok_seen += (ok_seen == 2) ? 1 : 0;
    }
  }
  EXPECT_EQ(ok_seen, 3u);

  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T16 — Server read length is bounded to 260 - buf_used (S1 enforcement).
TEST(OdinConnectSessionTest, T16) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadData(std::string("\x01", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadData(std::string("\x01", 1)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadData(std::string("\x01"
                                         "A"
                                         "\x01\xBB",
                                         4)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(req_decoded.calls, 1);
  const char *h = nullptr;
  size_t hl = 0;
  odin_connect_session_server_host(s, &h, &hl);
  ASSERT_EQ(hl, 1u);
  EXPECT_EQ(h[0], 'A');
  EXPECT_EQ(odin_connect_session_server_port(s), 0x01BB);
  EXPECT_EQ(odin_connect_session_wants(s), 0u);

  ASSERT_FALSE(t.read_records.empty());
  for (const ReadRecord &rec : t.read_records) {
    EXPECT_EQ(rec.len, ODIN_PROTO_CONNECT_REQ_MAX - rec.buf_used_at_call_time)
        << "len=" << rec.len << " buf_used=" << rec.buf_used_at_call_time;
  }
  size_t ok_seen = 0;
  for (const ReadRecord &rec : t.read_records) {
    if (rec.buf_used_at_call_time == 0 && rec.len == 260 && ok_seen == 0) {
      ok_seen = 1;
    } else if (rec.buf_used_at_call_time == 1 && rec.len == 259 &&
               ok_seen == 1) {
      ok_seen = 2;
    } else if (rec.buf_used_at_call_time == 2 && rec.len == 258 &&
               ok_seen == 2) {
      ok_seen = 3;
    }
  }
  EXPECT_EQ(ok_seen, 3u);

  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T17 — Server OK with 7 pipelined tail bytes (client wrote REQ + 7 upstream
// bytes in same TCP segment).
TEST(OdinConnectSessionTest, T17) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);

  // REQ for ("a", 1, 443): 0x01 0x01 0x01 'a' 0x01 0xBB, plus 7 pipelined.
  const std::string blob("\x01\x01\x01"
                         "a"
                         "\x01\xBB"
                         "\xDE\xAD\xBE\xEF\xCA\xFE\xBA",
                         13);
  t.reads.push_back(ReadData(blob));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(req_decoded.calls, 1);
  const char *h = nullptr;
  size_t hl = 0;
  odin_connect_session_server_host(s, &h, &hl);
  ASSERT_EQ(hl, 1u);
  EXPECT_EQ(h[0], 'a');
  EXPECT_EQ(odin_connect_session_server_port(s), 0x01BB);

  const uint8_t *p = nullptr;
  size_t l = 0;
  odin_connect_session_server_tail(s, &p, &l);
  ASSERT_EQ(l, 7u);
  EXPECT_EQ(std::memcmp(p, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA", 7), 0);

  EXPECT_EQ(odin_connect_session_wants(s), 0u);
  EXPECT_EQ(done.calls, 0);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T18 — Server read IO_ERROR.
TEST(OdinConnectSessionTest, T18) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadFail(ECONNRESET));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);
  EXPECT_EQ(req_decoded.calls, 0);

  odin_connect_session_destroy(s);
}

// T19 — Server EOF before REQ completes.
TEST(OdinConnectSessionTest, T19) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadEof());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);
  EXPECT_EQ(req_decoded.calls, 0);

  odin_connect_session_destroy(s);
}

// T20 — Server write IO_ERROR.
TEST(OdinConnectSessionTest, T20) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  const std::string req = EncodedReq("example.com", 443);
  t.reads.push_back(ReadData(req));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  ASSERT_EQ(req_decoded.calls, 1);

  odin_connect_session_server_set_error_code(s, 0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);

  t.writes.push_back(WriteFail(EPIPE));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPIPE);

  odin_connect_session_destroy(s);
}

// T21 — Server ERROR readiness in S_AWAIT_DIAL classified via
// odin_transport_error.
TEST(OdinConnectSessionTest, T21) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  const std::string req = EncodedReq("example.com", 443);
  t.reads.push_back(ReadData(req));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);
  ASSERT_EQ(req_decoded.calls, 1);
  ASSERT_EQ(odin_connect_session_wants(s), 0u);

  // Stay in S_AWAIT_DIAL. Fire an ERROR readiness with t.error → ECONNRESET.
  const int read_calls_before = t.read_calls;
  t.error_result = ECONNRESET;
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_ERROR);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);
  EXPECT_EQ(t.error_calls, 1);
  EXPECT_EQ(t.read_calls, read_calls_before);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T22 — Server ERROR readiness in S_READING_REQ classified via
// odin_transport_error.
TEST(OdinConnectSessionTest, T22) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);

  t.reads.push_back(ReadAgain());
  t.error_result = ECONNRESET;
  odin_connect_session_drive(s, &t.base,
                             ODIN_TRANSPORT_READ | ODIN_TRANSPORT_ERROR);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, ECONNRESET);
  EXPECT_EQ(t.error_calls, 1);
  EXPECT_EQ(t.read_calls, 1);
  EXPECT_EQ(req_decoded.calls, 0);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T23 — Client CONNECT_REQ write splits across two WRITE readiness
// deliveries.
TEST(OdinConnectSessionTest, T23) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("example.com", 11, 443, OnDone,
                                               &done, &s),
            0);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);

  t.writes.push_back(WriteAcceptN(7));
  t.writes.push_back(WriteAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(done.calls, 0);

  t.writes.push_back(WriteAcceptN(9));
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  EXPECT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);
  EXPECT_EQ(done.calls, 0);

  EXPECT_EQ(t.written, EncodedReq("example.com", 443));

  odin_connect_session_destroy(s);
}

// T24 — Server decoder rejects bad frame_type.
TEST(OdinConnectSessionTest, T24) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);

  t.reads.push_back(ReadData(std::string("\x01\x03\x03"
                                         "abc"
                                         "\x01\xBB",
                                         8)));
  t.reads.push_back(ReadAgain());
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_READ);

  EXPECT_EQ(done.calls, 1);
  EXPECT_EQ(done.status, ODIN_CONNECT_SESSION_ERROR);
  EXPECT_EQ(done.err, EPROTO);
  EXPECT_EQ(req_decoded.calls, 0);

  odin_connect_session_destroy(s);
}

// T25 — Client ERROR readiness with benign odin_transport_error → 0 re-arms.
TEST(OdinConnectSessionTest, T25) {
  FakeTransport t = MakeFake();
  DoneRecord done;

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_client("a", 1, 80, OnDone, &done, &s),
            0);
  odin_connect_session_drive(s, &t.base, ODIN_TRANSPORT_WRITE);
  ASSERT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);
  const unsigned int wants_before = odin_connect_session_wants(s);

  t.reads.push_back(ReadAgain());
  t.error_result = 0;
  const odin_connect_session_drive_t r = odin_connect_session_drive(
      s, &t.base, ODIN_TRANSPORT_READ | ODIN_TRANSPORT_ERROR);
  EXPECT_EQ(r, ODIN_CONNECT_SESSION_DRIVE_CONTINUE);
  EXPECT_EQ(done.calls, 0);
  EXPECT_EQ(odin_connect_session_wants(s), wants_before);
  EXPECT_EQ(t.error_calls, 1);
  EXPECT_EQ(t.read_calls, 1);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T26 — Server ERROR readiness with benign odin_transport_error → 0 re-arms.
TEST(OdinConnectSessionTest, T26) {
  FakeTransport t = MakeFake();
  DoneRecord done;
  ReqDecodedRecord req_decoded;
  Combo combo{&req_decoded, &done};

  odin_connect_session_t *s = nullptr;
  ASSERT_EQ(odin_connect_session_create_server(OnComboReqDecoded, OnComboDone,
                                               &combo, &s),
            0);
  ASSERT_EQ(odin_connect_session_wants(s), ODIN_TRANSPORT_READ);
  const unsigned int wants_before = odin_connect_session_wants(s);

  t.reads.push_back(ReadAgain());
  t.error_result = 0;
  const odin_connect_session_drive_t r = odin_connect_session_drive(
      s, &t.base, ODIN_TRANSPORT_READ | ODIN_TRANSPORT_ERROR);
  EXPECT_EQ(r, ODIN_CONNECT_SESSION_DRIVE_CONTINUE);
  EXPECT_EQ(done.calls, 0);
  EXPECT_EQ(req_decoded.calls, 0);
  EXPECT_EQ(odin_connect_session_wants(s), wants_before);
  EXPECT_EQ(t.error_calls, 1);
  EXPECT_EQ(t.read_calls, 1);
  EXPECT_EQ(t.set_interest_calls, 0);
  EXPECT_EQ(t.destroy_calls, 0);

  odin_connect_session_destroy(s);
}

// T27 — create_client rejects out-of-range host_len synchronously.
TEST(OdinConnectSessionTest, T27) {
  DoneRecord done;
  odin_connect_session_t *out =
      reinterpret_cast<odin_connect_session_t *>(0xDEADBEEFu);

  errno = 0;
  const int r0 =
      odin_connect_session_create_client("a", 0, 80, OnDone, &done, &out);
  EXPECT_EQ(r0, -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_connect_session_t *>(0xDEADBEEFu));
  EXPECT_EQ(done.calls, 0);

  errno = 0;
  const int r1 =
      odin_connect_session_create_client("a", 256, 80, OnDone, &done, &out);
  EXPECT_EQ(r1, -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_connect_session_t *>(0xDEADBEEFu));
  EXPECT_EQ(done.calls, 0);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
