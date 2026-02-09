import fs from 'node:fs';
import path from 'node:path';

/**
 * Writes structured request logs in NDJSON format to a file.
 *
 * Uses fs.createWriteStream in append mode:
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
            flags: 'a',          // Append mode
            encoding: 'utf8'
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
     */
    log({ req, body, statusCode, startTime }) {
        const entry = {
            timestamp: new Date().toISOString(),
            method: req.method,
            path: req.url.split('?')[0],
            url: req.url,
            statusCode,
            durationMs: Date.now() - startTime,
            headers: req.headers,
            body: body ?? null
        };

        this._stream.write(JSON.stringify(entry) + '\n');
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
