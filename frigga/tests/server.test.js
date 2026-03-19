import { describe, it, afterEach } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";

import { startServer } from "../src/server.js";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

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

describe("Integration — method and route errors", () => {
  it("returns 405 with Allow header for GET on valid route", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;
    const res = await request(port, { method: "GET" });
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers["allow"], "POST");
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "invalid_request_error");
  });

  it("returns 404 for unknown path", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;
    const res = await request(port, { path: "/v1/unknown" });
    assert.equal(res.statusCode, 404);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, "not_found_error");
  });
});

describe("Integration — startup failure", () => {
  it("rejects when port is already in use", async () => {
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;
    await assert.rejects(
      () => startServer({ port, host: "127.0.0.1" }),
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
    server = await startServer({ port: 0, host: "127.0.0.1" });
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

describe("Integration — NDJSON request logs (local errors)", () => {
  it("404 log omits request_body and has no response_headers", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;

    await request(port, {
      path: "/v1/unknown",
      headers: { "content-type": "application/json" },
      body: '{"model":"claude-sonnet-4-20250514"}',
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
    assert.equal(reqLog.url, "/v1/unknown");
    assert.equal(reqLog.status, 404);
    assert.equal(typeof reqLog.duration_ms, "number");
    assert.equal(typeof reqLog.request_headers, "object");
    assert.equal(reqLog.request_body, undefined);
    assert.equal(reqLog.response_headers, undefined);
  });

  it("405 log omits request_body and has no response_headers", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;

    await request(port, { method: "GET" });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.ok(reqLog, "Expected request log");
    assert.equal(reqLog.status, 405);
    assert.equal(reqLog.request_body, undefined);
    assert.equal(reqLog.response_headers, undefined);
  });

  it("request headers are logged correctly on local error", async () => {
    output = captureOutput();
    server = await startServer({ port: 0, host: "127.0.0.1" });
    const port = server.address().port;

    await request(port, {
      path: "/v1/unknown",
      headers: {
        "content-type": "application/json",
        authorization: "Bearer test-key",
      },
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");

    assert.equal(reqLog.request_headers["content-type"], "application/json");
    assert.equal(reqLog.request_headers["authorization"], "Bearer test-key");
  });
});

describe("Integration — --log-body flag", () => {
  it("--log-body does not affect local error path (404 still omits body)", async () => {
    output = captureOutput();
    server = await startServer({
      port: 0,
      host: "127.0.0.1",
      logBody: true,
    });
    const port = server.address().port;

    await request(port, {
      path: "/v1/unknown",
      headers: { "content-type": "application/json" },
      body: '{"model":"claude-sonnet-4-20250514"}',
    });

    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });

    const logs = parseJsonLines(output.lines.stdout);
    const reqLog = logs.find((l) => l.event === "request");
    assert.ok(reqLog, "Expected request log");
    assert.equal(reqLog.status, 404);
    assert.equal(reqLog.request_body, undefined);
  });
});

describe("Integration — graceful shutdown", () => {
  it("exits with code 0 on SIGTERM", async () => {
    const result = await new Promise((resolve, reject) => {
      const child = spawn("node", [INDEX_PATH, "--port=0"], {
        stdio: ["pipe", "pipe", "pipe"],
      });

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
