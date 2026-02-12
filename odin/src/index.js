#!/usr/bin/env node

import { createServer } from './server.js';
import { RequestLogger } from './logger.js';

// ─── CLI Argument Parsing ───────────────────────────────────────────────────

/**
 * Parse CLI arguments in the form --key=value or --flag.
 *
 * @param {string[]} argv - Process arguments (node, script, ...args)
 * @returns {Object} Parsed arguments
 */
function parseArgs(argv) {
    const args = {};
    for (const arg of argv.slice(2)) {
        if (!arg.startsWith('--')) continue;

        const eqIndex = arg.indexOf('=');
        if (eqIndex !== -1) {
            const key = arg.slice(2, eqIndex);
            const value = arg.slice(eqIndex + 1);
            args[key] = value;
        } else {
            args[arg.slice(2)] = true;
        }
    }

    return args;
}

function printUsage() {
    console.error(`Usage: node src/index.js --api-key=<key> [--port=<port>] [--debug] [--log-file=<path>]

Options:
  --api-key=<key>       API key for Antigravity Cloud Code (required)
  --port=<port>         Port to listen on (default: 8080)
  --debug               Enable debug logging (also records response SSE events in log file)
  --log-file=<path>     Custom log file path (default: logs/requests.ndjson)

Examples:
  node src/index.js --api-key="ya29.a0AeO..."
  node src/index.js --api-key="ya29..." --port=3000 --debug
  node src/index.js --api-key="ya29..." --log-file=/tmp/odin.ndjson`);
}

// ─── Main ───────────────────────────────────────────────────────────────────

const args = parseArgs(process.argv);

// Validate required arguments
if (!args['api-key']) {
    console.error('[Odin] Error: --api-key is required\n');
    printUsage();
    process.exit(1);
}

const apiKey = args['api-key'];
const port = parseInt(args['port'], 10) || 8080;
const debug = args['debug'] === true;
const logFile = args['log-file'] || 'logs/requests.ndjson';

// Logger is always created (no longer opt-in)
const logger = new RequestLogger(logFile);

// Create and start server
const server = createServer({ apiKey, debug, logger });

server.listen(port, () => {
    console.error(`[Odin] Server listening on http://localhost:${port}`);
    console.error(`[Odin] Debug mode: ${debug ? 'ON' : 'OFF'}`);
    console.error(`[Odin] Log file: ${logFile}`);
    console.error(`[Odin] Press Ctrl+C to stop`);
});

// Graceful shutdown
process.on('SIGINT', async () => {
    console.error('\n[Odin] Shutting down...');
    await logger.close();
    server.close(() => {
        process.exit(0);
    });
});

process.on('SIGTERM', async () => {
    console.error('[Odin] Received SIGTERM, shutting down...');
    await logger.close();
    server.close(() => {
        process.exit(0);
    });
});
