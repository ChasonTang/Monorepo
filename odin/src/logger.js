import fs from 'node:fs';
import path from 'node:path';

/**
 * Writes structured request logs in NDJSON format to a file.
 *
 * Uses fs.createWriteStream in write mode:
 * - File is truncated (cleared) when the stream is opened, ensuring each
 *   Odin session starts with a clean log
 * - High performance: leverages Node.js internal buffering to minimize syscalls
 * - Safety: call close() on graceful shutdown to flush the buffer
 */
export class RequestLogger {
    /**
     * @param {string} filePath - Path to the log file
     */
    constructor(filePath) {
        // Ensure the log directory exists
        const dir = path.dirname(filePath);
        fs.mkdirSync(dir, { recursive: true });

        this._stream = fs.createWriteStream(filePath, {
            flags: 'w', // Write mode — clear file on startup
            encoding: 'utf8',
        });

        this._stream.on('error', (err) => {
            console.error(`[Odin] Log file write error: ${err.message}`);
        });
    }

    /**
     * Write a single request log entry.
     *
     * @param {Object} params
     * @param {http.IncomingMessage} params.req - Request object
     * @param {Object|null} params.body - Parsed request body
     * @param {number} params.statusCode - Response status code
     * @param {number} params.startTime - Request start timestamp (Date.now())
     * @param {Object} [params.error] - Error details (Cloud Code or internal)
     * @param {Object} [params.response] - Response SSE events for /v1/messages (when --debug is active)
     * @param {Object} [params.debug] - Debug information (when --debug is active)
     */
    log({ req, body, statusCode, startTime, error, response, debug }) {
        const entry = {
            timestamp: new Date().toISOString(),
            method: req.method,
            path: req.url.split('?')[0],
            url: req.url,
            statusCode,
            durationMs: Date.now() - startTime,
            headers: req.headers,
            body: body ?? null,
        };

        if (error) {
            entry.error = error;
        }

        if (response) {
            entry.response = response;
        }

        if (debug) {
            entry.debug = debug;
        }

        this._stream.write(`${JSON.stringify(entry)}\n`);
    }

    /**
     * Close the write stream and flush the buffer.
     * Call this during graceful shutdown.
     *
     * @returns {Promise<void>}
     */
    close() {
        return new Promise((resolve) => {
            this._stream.end(resolve);
        });
    }
}
