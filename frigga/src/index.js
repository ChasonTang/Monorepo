#!/usr/bin/env node

import { parseArgs, resolveArgs } from "./cli.js";
import { startServer } from "./server.js";

const args = parseArgs(process.argv);
const config = resolveArgs(args);

try {
  await startServer(config);
} catch (err) {
  process.stderr.write(`Error: ${err.message}\n`);
  process.exit(1);
}
