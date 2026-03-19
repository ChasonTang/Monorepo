import { describe, it, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import {
  handleRequest,
  buildUpstreamHeaders,
  filterResponseHeaders,
  abortUpstream,
  createRequestLogEmitter,
} from "../src/server.js";

// ── handleRequest ─────────────────────────────────────────

describe("handleRequest — route matching", () => {
  it("returns 404 for unknown path", () => {
    const res = handleRequest({ method: "POST", url: "/v1/unknown" }, false);
    assert.equal(res.statusCode, 404);
    const body = JSON.parse(res.body);
    assert.equal(body.type, "error");
    assert.equal(body.error.type, "not_found_error");
  });

  it("returns 404 for root path", () => {
    const res = handleRequest({ method: "POST", url: "/" }, false);
    assert.equal(res.statusCode, 404);
  });

  it("returns null for /v1/messages", () => {
    const res = handleRequest({ method: "POST", url: "/v1/messages" }, false);
    assert.equal(res, null);
  });

  it("returns null for /v1/messages/count_tokens", () => {
    const res = handleRequest(
      { method: "POST", url: "/v1/messages/count_tokens" },
      false,
    );
    assert.equal(res, null);
  });
});

describe("handleRequest — method validation", () => {
  it("returns 405 for GET on valid route", () => {
    const res = handleRequest({ method: "GET", url: "/v1/messages" }, false);
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers["Allow"], "POST");
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "invalid_request_error");
  });

  it("returns 405 for PUT on valid route", () => {
    const res = handleRequest({ method: "PUT", url: "/v1/messages" }, false);
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers["Allow"], "POST");
  });

  it("returns 404 (not 405) for GET on unknown path", () => {
    const res = handleRequest({ method: "GET", url: "/v1/unknown" }, false);
    assert.equal(res.statusCode, 404);
  });
});

describe("handleRequest — error response format", () => {
  it("follows Anthropic error shape", () => {
    const res = handleRequest({ method: "POST", url: "/v1/unknown" }, false);
    const body = JSON.parse(res.body);
    assert.equal(body.type, "error");
    assert.ok(body.error);
    assert.equal(typeof body.error.type, "string");
    assert.equal(typeof body.error.message, "string");
  });

  it("sets Content-Type to application/json", () => {
    const res = handleRequest({ method: "POST", url: "/v1/unknown" }, false);
    assert.equal(res.headers["Content-Type"], "application/json");
  });
});

describe("handleRequest — shutdown flag", () => {
  it("sets connection: close when shutting down", () => {
    const res = handleRequest({ method: "GET", url: "/v1/messages" }, true);
    assert.equal(res.headers["connection"], "close");
  });

  it("does not set connection: close when not shutting down", () => {
    const res = handleRequest({ method: "GET", url: "/v1/messages" }, false);
    assert.equal(res.headers["connection"], undefined);
  });
});

describe("handleRequest — evaluation order", () => {
  it("returns 404 before 405 for unknown path with wrong method", () => {
    const res = handleRequest({ method: "GET", url: "/v1/unknown" }, false);
    assert.equal(res.statusCode, 404);
  });
});

// ── buildUpstreamHeaders ──────────────────────────────────

describe("buildUpstreamHeaders", () => {
  it("forwards whitelisted headers", () => {
    const headers = buildUpstreamHeaders({
      "content-type": "application/json",
      accept: "*/*",
      "accept-encoding": "gzip, br",
      "user-agent": "claude-code/1.0",
      "anthropic-beta": "messages-2024-12-19",
      "anthropic-version": "2023-06-01",
      authorization: "Bearer sk-ant-xxx",
      "content-length": "42",
    });
    assert.equal(headers["content-type"], "application/json");
    assert.equal(headers["accept"], "*/*");
    assert.equal(headers["accept-encoding"], "gzip, br");
    assert.equal(headers["user-agent"], "claude-code/1.0");
    assert.equal(headers["anthropic-beta"], "messages-2024-12-19");
    assert.equal(headers["anthropic-version"], "2023-06-01");
    assert.equal(headers["authorization"], "Bearer sk-ant-xxx");
    assert.equal(headers["content-length"], "42");
  });

  it("excludes non-whitelisted headers", () => {
    const headers = buildUpstreamHeaders({
      "content-type": "application/json",
      "x-stainless-arch": "x64",
      connection: "keep-alive",
      "transfer-encoding": "chunked",
      host: "localhost:3000",
      "x-forwarded-for": "10.0.0.1",
      "anthropic-dangerous-direct-browser-access": "true",
    });
    assert.equal(headers["content-type"], "application/json");
    assert.equal(headers["x-stainless-arch"], undefined);
    assert.equal(headers["connection"], undefined);
    assert.equal(headers["transfer-encoding"], undefined);
    assert.equal(headers["host"], undefined);
    assert.equal(headers["x-forwarded-for"], undefined);
    assert.equal(
      headers["anthropic-dangerous-direct-browser-access"],
      undefined,
    );
  });

  it("handles missing optional headers", () => {
    const headers = buildUpstreamHeaders({
      "content-type": "application/json",
    });
    assert.equal(headers["content-type"], "application/json");
    assert.equal(headers["anthropic-beta"], undefined);
    assert.equal(headers["accept-encoding"], undefined);
    assert.equal(Object.keys(headers).length, 1);
  });

  it("forwards authorization as-is", () => {
    const headers = buildUpstreamHeaders({
      authorization: "Bearer sk-ant-api03-xxx",
    });
    assert.equal(headers["authorization"], "Bearer sk-ant-api03-xxx");
  });
});

// ── filterResponseHeaders ─────────────────────────────────

describe("filterResponseHeaders", () => {
  it("forwards whitelisted response headers", () => {
    const filtered = filterResponseHeaders({
      "content-type": "text/event-stream",
      "content-length": "1234",
      "content-encoding": "gzip",
      "x-request-id": "req-123",
      "request-id": "req-456",
      "retry-after": "30",
    });
    assert.equal(filtered["content-type"], "text/event-stream");
    assert.equal(filtered["content-length"], "1234");
    assert.equal(filtered["content-encoding"], "gzip");
    assert.equal(filtered["x-request-id"], "req-123");
    assert.equal(filtered["request-id"], "req-456");
    assert.equal(filtered["retry-after"], "30");
  });

  it("forwards anthropic-ratelimit-* prefix headers", () => {
    const filtered = filterResponseHeaders({
      "anthropic-ratelimit-requests-limit": "100",
      "anthropic-ratelimit-tokens-remaining": "50000",
      "anthropic-ratelimit-requests-reset": "2026-03-18T00:00:00Z",
    });
    assert.equal(filtered["anthropic-ratelimit-requests-limit"], "100");
    assert.equal(filtered["anthropic-ratelimit-tokens-remaining"], "50000");
    assert.equal(
      filtered["anthropic-ratelimit-requests-reset"],
      "2026-03-18T00:00:00Z",
    );
  });

  it("excludes non-whitelisted response headers", () => {
    const filtered = filterResponseHeaders({
      "content-type": "application/json",
      server: "nginx",
      date: "Thu, 18 Mar 2026 00:00:00 GMT",
      via: "1.1 proxy",
      connection: "keep-alive",
    });
    assert.equal(filtered["content-type"], "application/json");
    assert.equal(filtered["server"], undefined);
    assert.equal(filtered["date"], undefined);
    assert.equal(filtered["via"], undefined);
    assert.equal(filtered["connection"], undefined);
  });
});

// ── abortUpstream ─────────────────────────────────────────

describe("abortUpstream", () => {
  it("calls req.unpipe() before upstreamReq.destroy()", () => {
    const callOrder = [];
    const req = { unpipe: () => callOrder.push("unpipe") };
    const upstreamReq = {
      destroyed: false,
      destroy: () => callOrder.push("destroy"),
    };
    abortUpstream(req, upstreamReq);
    assert.deepEqual(callOrder, ["unpipe", "destroy"]);
  });

  it("is no-op when upstreamReq.destroyed is true", () => {
    const callOrder = [];
    const req = { unpipe: () => callOrder.push("unpipe") };
    const upstreamReq = {
      destroyed: true,
      destroy: () => callOrder.push("destroy"),
    };
    abortUpstream(req, upstreamReq);
    assert.deepEqual(callOrder, []);
  });

  it("is no-op when upstreamReq is undefined", () => {
    const req = { unpipe: () => {} };
    abortUpstream(req, undefined);
  });
});

// ── createRequestLogEmitter ───────────────────────────────

describe("createRequestLogEmitter", () => {
  let origWrite;
  let stdoutLines;

  beforeEach(() => {
    stdoutLines = [];
    origWrite = process.stdout.write;
    process.stdout.write = (data) => {
      stdoutLines.push(data.toString());
      return true;
    };
  });

  afterEach(() => {
    process.stdout.write = origWrite;
  });

  function parseLog() {
    const results = [];
    for (const line of stdoutLines) {
      for (const part of line.split("\n")) {
        const trimmed = part.trim();
        if (!trimmed) continue;
        try {
          results.push(JSON.parse(trimmed));
        } catch {
          // skip
        }
      }
    }
    return results;
  }

  it("emits exactly one log — second call is no-op", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const requestChunks = [Buffer.from("hello")];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(200, { "content-type": "application/json" });
    emitRequestLog(502);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 200);
    assert.equal(logs[0].response_headers["content-type"], "application/json");
  });

  it("finalizes immediately when req.readableEnded is true", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const requestChunks = [Buffer.from('{"model":"test"}')];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(200);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].request_body, '{"model":"test"}');
  });

  it("finalizes immediately when req.destroyed is true", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: false,
      destroyed: true,
    };
    const requestChunks = [Buffer.from("partial")];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(499);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 499);
    assert.equal(logs[0].request_body, "partial");
  });

  it("defers finalization to end event when req is still streaming", () => {
    const req = new EventEmitter();
    req.method = "POST";
    req.url = "/v1/messages";
    req.headers = {};
    req.readableEnded = false;
    req.destroyed = false;
    req.resume = () => {};

    const requestChunks = [Buffer.from("body")];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(502);

    // Not yet emitted
    assert.equal(parseLog().length, 0);

    // Emit end
    req.emit("end");

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 502);
    assert.equal(logs[0].request_body, "body");
  });

  it("finalized guard prevents double execution when both end and error fire", () => {
    const req = new EventEmitter();
    req.method = "POST";
    req.url = "/v1/messages";
    req.headers = {};
    req.readableEnded = false;
    req.destroyed = false;
    req.resume = () => {};

    const requestChunks = [Buffer.from("data")];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(499);

    // Fire both events
    req.emit("end");
    req.emit("error", new Error("ECONNRESET"));

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].request_body, "data");
  });

  it("clears requestChunks after consumption", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const requestChunks = [Buffer.from("a"), Buffer.from("b")];
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks,
      startTime: Date.now(),
    });

    emitRequestLog(200);

    assert.equal(requestChunks.length, 0);
  });

  it("emits log with no request_body when requestChunks omitted, stream ended", () => {
    const req = {
      method: "POST",
      url: "/v1/unknown",
      headers: { "content-type": "application/json" },
      readableEnded: true,
      destroyed: false,
    };
    const emitRequestLog = createRequestLogEmitter({
      req,
      startTime: Date.now(),
    });

    emitRequestLog(404);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 404);
    assert.equal(logs[0].request_headers["content-type"], "application/json");
    assert.equal(logs[0].request_body, undefined);
  });

  it("calls req.resume() when requestChunks omitted and stream active", () => {
    let resumed = false;
    const req = new EventEmitter();
    req.method = "POST";
    req.url = "/v1/unknown";
    req.headers = {};
    req.readableEnded = false;
    req.destroyed = false;
    req.resume = () => {
      resumed = true;
    };

    const emitRequestLog = createRequestLogEmitter({
      req,
      startTime: Date.now(),
    });

    emitRequestLog(404);

    assert.ok(resumed, "Expected req.resume() to be called");
    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].request_body, undefined);
  });

  it("exactly-once guard works when requestChunks omitted", () => {
    const req = {
      method: "POST",
      url: "/v1/unknown",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const emitRequestLog = createRequestLogEmitter({
      req,
      startTime: Date.now(),
    });

    emitRequestLog(404);
    emitRequestLog(405);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 404);
  });

  it("omits response_headers when not provided", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestChunks: [],
      startTime: Date.now(),
    });

    emitRequestLog(404);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].response_headers, undefined);
  });
});
