/**
 * Tests for RFC-019: Function Response Struct Compliance
 *
 * Covers §6 scenarios:
 * - String content, success → { output: content }
 * - String content, error → { error: content }
 * - is_error explicitly false → { output: content }
 * - is_error absent → { output: content }
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { convertContentToParts } from '../src/converter.js';

function toolResultBlock(overrides = {}) {
    return {
        type: 'tool_result',
        tool_use_id: 't1',
        content: 'ok',
        ...overrides,
    };
}

describe('RFC-019 — functionResponse Struct wrapping', () => {
    // §6 #1: String content, success
    it('wraps string content in { output } for success result', () => {
        const parts = convertContentToParts([toolResultBlock({ content: '65 degrees' })]);

        assert.equal(parts.length, 1);
        assert.deepStrictEqual(parts[0].functionResponse.response, {
            output: '65 degrees',
        });
    });

    // §6 #2: String content, error
    it('wraps string content in { error } when is_error is true', () => {
        const parts = convertContentToParts([
            toolResultBlock({ content: 'ENOENT: not found', is_error: true }),
        ]);

        assert.equal(parts.length, 1);
        assert.deepStrictEqual(parts[0].functionResponse.response, {
            error: 'ENOENT: not found',
        });
    });

    // §6 #3: is_error explicitly false
    it('wraps in { output } when is_error is explicitly false', () => {
        const parts = convertContentToParts([toolResultBlock({ content: 'ok', is_error: false })]);

        assert.equal(parts.length, 1);
        assert.deepStrictEqual(parts[0].functionResponse.response, {
            output: 'ok',
        });
    });

    // §6 #4: is_error absent
    it('wraps in { output } when is_error is absent', () => {
        const parts = convertContentToParts([toolResultBlock({ content: 'ok' })]);

        assert.equal(parts.length, 1);
        assert.deepStrictEqual(parts[0].functionResponse.response, {
            output: 'ok',
        });
    });

    it('preserves id and name fields from tool_use_id', () => {
        const parts = convertContentToParts([
            toolResultBlock({ tool_use_id: 'toolu_abc123', content: 'done' }),
        ]);

        assert.equal(parts[0].functionResponse.id, 'toolu_abc123');
        assert.equal(parts[0].functionResponse.name, 'toolu_abc123');
    });
});
