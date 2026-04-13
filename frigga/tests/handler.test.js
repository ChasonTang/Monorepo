import { describe, it, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import {
  handleRequest,
  buildUpstreamHeaders,
  buildUpstreamUrl,
  filterResponseHeaders,
  abortUpstream,
  bufferRequestBody,
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

// ── buildUpstreamUrl ─────────────────────────────────────

describe("buildUpstreamUrl — origin-only base (regression)", () => {
  it("joins origin base with /v1/messages", () => {
    const url = buildUpstreamUrl("https://api.anthropic.com", "/v1/messages");
    assert.equal(url.href, "https://api.anthropic.com/v1/messages");
  });

  it("joins origin base with /v1/messages/count_tokens", () => {
    const url = buildUpstreamUrl(
      "https://api.anthropic.com",
      "/v1/messages/count_tokens",
    );
    assert.equal(
      url.href,
      "https://api.anthropic.com/v1/messages/count_tokens",
    );
  });
});

describe("buildUpstreamUrl — base with path prefix", () => {
  const base = "https://codewiz.alibaba-inc.com/api/gateway/anthropic";

  it("preserves path prefix for /v1/messages", () => {
    const url = buildUpstreamUrl(base, "/v1/messages");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/v1/messages",
    );
  });

  it("preserves path prefix for /v1/messages/count_tokens", () => {
    const url = buildUpstreamUrl(base, "/v1/messages/count_tokens");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/v1/messages/count_tokens",
    );
  });

  it("handles trailing slash on base URL", () => {
    const url = buildUpstreamUrl(`${base}/`, "/v1/messages");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/v1/messages",
    );
  });
});

describe("buildUpstreamUrl — query string preservation", () => {
  it("preserves query string from request URL", () => {
    const url = buildUpstreamUrl(
      "https://api.anthropic.com",
      "/v1/messages?stream=true",
    );
    assert.equal(url.href, "https://api.anthropic.com/v1/messages?stream=true");
  });

  it("preserves query string with path-prefix base", () => {
    const url = buildUpstreamUrl(
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic",
      "/v1/messages?stream=true",
    );
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/v1/messages?stream=true",
    );
  });
});

describe("buildUpstreamUrl — reqUrl without leading slash", () => {
  it("handles reqUrl without leading / (origin-only base)", () => {
    const url = buildUpstreamUrl("https://api.anthropic.com", "v1/messages");
    assert.equal(url.href, "https://api.anthropic.com/v1/messages");
  });

  it("handles reqUrl without leading / (path-prefix base)", () => {
    const url = buildUpstreamUrl(
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic",
      "v1/messages",
    );
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/v1/messages",
    );
  });
});

describe("buildUpstreamUrl — path traversal normalization", () => {
  const base = "https://codewiz.alibaba-inc.com/api/gateway/anthropic";

  it("normalizes /../secret to stay within basePath", () => {
    const url = buildUpstreamUrl(base, "/../secret");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/secret",
    );
  });

  it("normalizes /v1/../secret to stay within basePath", () => {
    const url = buildUpstreamUrl(base, "/v1/../secret");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/secret",
    );
  });

  it("normalizes percent-encoded /%2e%2e/secret", () => {
    const url = buildUpstreamUrl(base, "/%2e%2e/secret");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/secret",
    );
  });

  it("normalizes /../secret with origin-only base", () => {
    const url = buildUpstreamUrl("https://api.anthropic.com", "/../secret");
    assert.equal(url.href, "https://api.anthropic.com/secret");
  });
});

describe("buildUpstreamUrl — protocol-relative URL", () => {
  it("ignores hostname from //evil.com/path and uses base origin", () => {
    const url = buildUpstreamUrl(
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic",
      "//evil.com/path",
    );
    assert.equal(url.origin, "https://codewiz.alibaba-inc.com");
    assert.equal(
      url.href,
      "https://codewiz.alibaba-inc.com/api/gateway/anthropic/path",
    );
  });
});

describe("buildUpstreamUrl — double slashes in base URL", () => {
  it("throws when leading double slash in path triggers protocol-relative reparse", () => {
    // basePath becomes "//api//gateway/" which new URL() reparses as
    // protocol-relative (host=api), escaping the intended base path.
    // The prefix check catches this misconfiguration.
    assert.throws(
      () =>
        buildUpstreamUrl("https://example.com//api//gateway", "/v1/messages"),
      { message: /upstream path traversal blocked/ },
    );
  });

  it("handles double slashes mid-path (not at start)", () => {
    const url = buildUpstreamUrl(
      "https://example.com/api//gateway",
      "/v1/messages",
    );
    assert.equal(url.href, "https://example.com/api//gateway/v1/messages");
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
    });
    assert.equal(headers["content-type"], "application/json");
    assert.equal(headers["accept"], "*/*");
    assert.equal(headers["accept-encoding"], "gzip, br");
    assert.equal(headers["user-agent"], "claude-code/1.0");
    assert.equal(headers["anthropic-beta"], "messages-2024-12-19");
    assert.equal(headers["anthropic-version"], "2023-06-01");
    assert.equal(headers["authorization"], "Bearer sk-ant-xxx");
  });

  it("does not forward content-length", () => {
    const headers = buildUpstreamHeaders({
      "content-type": "application/json",
      "content-length": "42",
    });
    assert.equal(headers["content-type"], "application/json");
    assert.equal(headers["content-length"], undefined);
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
      "request-id": "req-456",
      "retry-after": "30",
    });
    assert.equal(filtered["content-type"], "text/event-stream");
    assert.equal(filtered["content-length"], "1234");
    assert.equal(filtered["content-encoding"], "gzip");
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
  it("calls upstreamReq.destroy()", () => {
    let destroyed = false;
    const upstreamReq = {
      destroyed: false,
      destroy: () => {
        destroyed = true;
      },
    };
    abortUpstream(upstreamReq);
    assert.ok(destroyed);
  });

  it("is no-op when upstreamReq.destroyed is true", () => {
    let destroyed = false;
    const upstreamReq = {
      destroyed: true,
      destroy: () => {
        destroyed = true;
      },
    };
    abortUpstream(upstreamReq);
    assert.ok(!destroyed);
  });

  it("is no-op when upstreamReq is undefined", () => {
    abortUpstream(undefined);
  });
});

// ── bufferRequestBody ────────────────────────────────────

describe("bufferRequestBody", () => {
  it("resolves with concatenated Buffer from multiple chunks", async () => {
    const stream = new PassThrough();
    const promise = bufferRequestBody(stream);
    stream.write("hello ");
    stream.write("world");
    stream.end();
    const result = await promise;
    assert.deepEqual(result, Buffer.from("hello world"));
  });

  it("resolves with empty Buffer when no data is written", async () => {
    const stream = new PassThrough();
    const promise = bufferRequestBody(stream);
    stream.end();
    const result = await promise;
    assert.deepEqual(result, Buffer.alloc(0));
  });

  it("rejects on stream error", async () => {
    const stream = new PassThrough();
    const promise = bufferRequestBody(stream);
    stream.destroy(new Error("ECONNRESET"));
    await assert.rejects(promise, { message: "ECONNRESET" });
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

  it("includes request_body when requestBody provided", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestBody: Buffer.from('{"model":"test"}'),
      startTime: Date.now(),
    });

    emitRequestLog(200, { "content-type": "application/json" });

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 200);
    assert.equal(logs[0].request_body, '{"model":"test"}');
    assert.equal(logs[0].response_headers["content-type"], "application/json");
  });

  it("omits request_body when requestBody not provided", () => {
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

  it("emits exactly one log — second call is no-op", () => {
    const req = {
      method: "POST",
      url: "/v1/messages",
      headers: {},
      readableEnded: true,
      destroyed: false,
    };
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestBody: Buffer.from("hello"),
      startTime: Date.now(),
    });

    emitRequestLog(200);
    emitRequestLog(502);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].status, 200);
  });

  it("calls req.resume() when requestBody omitted and stream active", () => {
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
      requestBody: Buffer.from(""),
      startTime: Date.now(),
    });

    emitRequestLog(404);

    const logs = parseLog();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].response_headers, undefined);
  });
});
