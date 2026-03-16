import { describe, it, afterEach } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import net from "node:net";
import { startServer } from "../src/server.js";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const API_KEY = "integration-test-key";
const __dirname = dirname(fileURLToPath(import.meta.url));
const INDEX_PATH = join(__dirname, "..", "src", "index.js");

let server;
let output;

afterEach(async () => {
  if (output) {
    output.restore();
    output = null;
  }
  if (server) {
    await new Promise((resolve) => {
      server.closeAllConnections();
      server.close(resolve);
    });
    server = null;
  }
});

function request(
  port,
  { method = "POST", path = "/v1/messages", headers = {}, body } = {},
) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { hostname: "127.0.0.1", port, method, path, headers },
      (res) => {
        const chunks = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          resolve({
            statusCode: res.statusCode,
            headers: res.headers,
            body: Buffer.concat(chunks).toString(),
          });
        });
      },
    );
    req.on("error", reject);
    if (body !== undefined) {
      req.write(body);
    }
    req.end();
  });
}

function captureOutput() {
  const lines = { stdout: [], stderr: [] };
  const origStdoutWrite = process.stdout.write.bind(process.stdout);
  const origStderrWrite = process.stderr.write.bind(process.stderr);

  process.stdout.write = (data, ...args) => {
    lines.stdout.push(data.toString());
    return origStdoutWrite(data, ...args);
  };
  process.stderr.write = (data, ...args) => {
    lines.stderr.push(data.toString());
    return origStderrWrite(data, ...args);
  };

  return {
    restore() {
      process.stdout.write = origStdoutWrite;
      process.stderr.write = origStderrWrite;
    },
    lines,
  };
}

function parseJsonLines(lines) {
  const results = [];
  for (const line of lines) {
    for (const part of line.split("\n")) {
      const trimmed = part.trim();
      if (!trimmed) continue;
      try {
        results.push(JSON.parse(trimmed));
      } catch {
        // Skip non-JSON lines
      }
    }
  }
  return results;
}

describe("Integration — valid auth", () => {
  it("returns 501 for POST /v1/messages with correct key", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, {
      headers: { authorization: `Bearer ${API_KEY}` },
    });
    assert.equal(res.statusCode, 501);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "not_implemented");
  });

  it("returns 501 for POST /v1/messages/count_tokens with correct key", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, {
      path: "/v1/messages/count_tokens",
      headers: { authorization: `Bearer ${API_KEY}` },
    });
    assert.equal(res.statusCode, 501);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "not_implemented");
  });
});

describe("Integration — auth failures", () => {
  it("returns 401 for missing Authorization", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port);
    assert.equal(res.statusCode, 401);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "authentication_error");
  });

  it("returns 401 for wrong key", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, {
      headers: { authorization: "Bearer wrong-key" },
    });
    assert.equal(res.statusCode, 401);
  });

  it("returns 401 for malformed Authorization (no Bearer prefix)", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, {
      headers: { authorization: API_KEY },
    });
    assert.equal(res.statusCode, 401);
  });

  it("returns 401 for empty Bearer token", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, {
      headers: { authorization: "Bearer " },
    });
    assert.equal(res.statusCode, 401);
  });
});

describe("Integration — method and route errors", () => {
  it("returns 405 with Allow header for GET on valid route", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, { method: "GET" });
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers["allow"], "POST");
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "invalid_request_error");
  });

  it("returns 404 for unknown path", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const res = await request(port, { path: "/v1/unknown" });
    assert.equal(res.statusCode, 404);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "not_found_error");
  });
});

describe("Integration — concurrent requests", () => {
  it("handles 10 simultaneous requests", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    const promises = Array.from({ length: 10 }, () =>
      request(port, { headers: { authorization: `Bearer ${API_KEY}` } }),
    );
    const results = await Promise.all(promises);
    for (const res of results) {
      assert.equal(res.statusCode, 501);
    }
  });
});

describe("Integration — startup failure", () => {
  it("rejects when port is already in use", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;
    await assert.rejects(
      () => startServer({ port, host: "127.0.0.1", apiKey: API_KEY }),
      (err) => {
        assert.equal(err.code, "EADDRINUSE");
        return true;
      },
    );
  });
});

describe("Integration — NDJSON startup log", () => {
  it("startup log is valid NDJSON with host and port", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const addr = server.address();

    const logs = parseJsonLines(output.lines.stdout);
    const startupLog = logs.find((l) => l.event === "startup");

    assert.ok(startupLog, "Expected startup log");
    assert.equal(startupLog.level, "INFO");
    assert.ok(startupLog.timestamp);
    assert.equal(startupLog.host, addr.address);
    assert.equal(startupLog.port, addr.port);
  });
});

describe("Integration — NDJSON request logs", () => {
  it("request log contains all required fields", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    const reqBody =
      '{"model":"claude-sonnet-4-20250514","messages":[{"role":"user","content":"Hello"}]}';
    await request(port, {
      headers: {
        authorization: `Bearer ${API_KEY}`,
        "content-type": "application/json",
      },
      body: reqBody,
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.ok(reqLog, "Expected request log");
    assert.equal(reqLog.level, "INFO");
    assert.ok(reqLog.timestamp);
    assert.equal(reqLog.method, "POST");
    assert.equal(reqLog.url, "/v1/messages");
    assert.equal(reqLog.status, 501);
    assert.equal(typeof reqLog.duration_ms, "number");
    assert.ok(reqLog.duration_ms >= 0);
    assert.equal(typeof reqLog.request_headers, "object");
    assert.equal(typeof reqLog.request_body, "string");
    assert.equal(typeof reqLog.response_headers, "object");
    assert.equal(typeof reqLog.response_body, "string");
  });

  it("request headers are logged correctly", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    await request(port, {
      headers: {
        authorization: `Bearer ${API_KEY}`,
        "content-type": "application/json",
      },
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.equal(reqLog.request_headers["content-type"], "application/json");
    assert.equal(reqLog.request_headers["authorization"], `Bearer ${API_KEY}`);
  });

  it("request body is logged correctly", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    const body = '{"model":"claude-sonnet-4-20250514"}';
    await request(port, {
      headers: {
        authorization: `Bearer ${API_KEY}`,
        "content-type": "application/json",
      },
      body,
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.equal(reqLog.request_body, body);
  });

  it("empty request body is logged as empty string", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    await request(port, {
      headers: { authorization: `Bearer ${API_KEY}` },
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.equal(reqLog.request_body, "");
  });

  it("response fields match actual HTTP response", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    const res = await request(port, {
      headers: { authorization: `Bearer ${API_KEY}` },
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.equal(reqLog.status, res.statusCode);
    assert.equal(reqLog.response_body, res.body);
    assert.equal(
      reqLog.response_headers["content-type"],
      res.headers["content-type"],
    );
  });
});

describe("Integration — NDJSON request error", () => {
  it("request stream error emits request_error log", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1", apiKey: API_KEY });
    const port = server.address().port;

    await new Promise((resolve) => {
      const socket = net.connect(port, "127.0.0.1", () => {
        socket.write(
          "POST /v1/messages HTTP/1.1\r\n" +
            "Host: 127.0.0.1\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: 100\r\n" +
            "\r\n" +
            "partial",
        );
        setTimeout(() => {
          socket.resetAndDestroy();
          setTimeout(resolve, 200);
        }, 50);
      });
      socket.on("error", () => {});
    });

    const stderrLogs = parseJsonLines(output.lines.stderr);
    const errorLog = stderrLogs.find((l) => l.event === "request_error");

    assert.ok(errorLog, "Expected request_error log on stderr");
    assert.equal(errorLog.level, "ERROR");
    assert.equal(errorLog.method, "POST");
    assert.equal(errorLog.url, "/v1/messages");
    assert.ok(errorLog.request_headers);
    assert.equal(errorLog.request_headers["content-type"], "application/json");
    assert.ok(errorLog.message);

    const stdoutLogs = parseJsonLines(output.lines.stdout);
    const requestLog = stdoutLogs.find((l) => l.event === "request");
    assert.equal(requestLog, undefined, "Expected no request log");
  });
});

describe("Integration — graceful shutdown", () => {
  it("exits with code 0 on SIGTERM", async () => {
    const result = await new Promise((resolve, reject) => {
      const child = spawn(
        "node",
        [INDEX_PATH, `--api-key=${API_KEY}`, "--port=0"],
        {
          stdio: ["pipe", "pipe", "pipe"],
        },
      );

      let stdout = "";
      let stderr = "";

      child.stdout.on("data", (data) => {
        stdout += data.toString();
        if (stdout.includes('"event":"startup"')) {
          setTimeout(() => child.kill("SIGTERM"), 50);
        }
      });

      child.stderr.on("data", (data) => {
        stderr += data.toString();
      });

      child.on("close", (code) => {
        clearTimeout(killTimer);
        resolve({ code, stdout, stderr });
      });

      child.on("error", reject);

      const killTimer = setTimeout(() => {
        child.kill("SIGKILL");
        reject(new Error("Timeout waiting for graceful shutdown"));
      }, 10_000);
    });

    assert.equal(result.code, 0);

    const logs = result.stdout
      .split("\n")
      .filter((l) => l.trim())
      .map((l) => {
        try {
          return JSON.parse(l);
        } catch {
          return null;
        }
      })
      .filter(Boolean);

    const shutdownLogs = logs.filter((l) => l.event === "shutdown");
    assert.ok(
      shutdownLogs.some((l) => l.message === "stop accepting new connections"),
    );
    assert.ok(shutdownLogs.some((l) => l.message === "complete"));
  });
});
