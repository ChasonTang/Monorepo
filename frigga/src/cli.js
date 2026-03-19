import { DEFAULT_PORT, DEFAULT_HOST } from "./constants.js";

/**
 * Parse CLI arguments from argv.
 * Supports --key=value and --flag forms.
 * @param {string[]} argv - process.argv
 * @returns {Record<string, string | true>}
 */
export function parseArgs(argv) {
  const args = {};
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith("--")) continue;
    const eqIndex = arg.indexOf("=");
    if (eqIndex === -1) {
      args[arg.slice(2)] = true;
    } else {
      args[arg.slice(2, eqIndex)] = arg.slice(eqIndex + 1);
    }
  }
  return args;
}

/**
 * Print usage text to stderr.
 */
export function printUsage() {
  process.stderr.write(
    "Usage: node src/index.js [--port=<port>] [--host=<host>] [--log-body]\n" +
      "\n" +
      "Options:\n" +
      "  --port      HTTP listen port (default: 3000)\n" +
      "  --host      HTTP listen address (default: 127.0.0.1)\n" +
      "  --log-body  Include request body in NDJSON logs (default: off)\n",
  );
}

/**
 * Validate and extract arguments with defaults.
 * @param {Record<string, string | true>} args
 * @returns {{ port: number, host: string, logBody: boolean }}
 */
export function resolveArgs(args) {
  const portStr = args["port"];
  let port = DEFAULT_PORT;
  if (portStr !== undefined && portStr !== true) {
    port = Number(portStr);
    if (!Number.isInteger(port) || port < 0 || port > 65535) {
      printUsage();
      process.stderr.write(
        `Error: --port must be an integer between 0 and 65535\n`,
      );
      process.exit(1);
    }
  }

  const host =
    args["host"] && args["host"] !== true ? args["host"] : DEFAULT_HOST;

  const logBody = args["log-body"] === true;

  return { port, host, logBody };
}
