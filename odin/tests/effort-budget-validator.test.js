/**
 * Tests for RFC-015: Effort-Based Thinking Budget Resolution — Validator
 *
 * Covers §6.1 scenarios:
 * - Cross-field rejection: effort + no thinking, effort + disabled, effort + budget_tokens, adaptive + no effort
 * - Valid combinations: adaptive + effort, enabled + budget_tokens, disabled (no effort)
 * - AJV enum rejection: invalid effort values
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { validateMessagesRequest } from '../src/validator.js';

function validBase(overrides = {}) {
    return {
        model: 'claude-sonnet-4-20250514',
        messages: [{ role: 'user', content: 'hello' }],
        max_tokens: 4096,
        stream: true,
        ...overrides,
    };
}

describe('RFC-015 Validator — Cross-Field Rejection', () => {
    // §6.1 #1: Effort + no thinking block → rejected
    it('rejects effort without a thinking block', () => {
        const body = validBase({
            output_config: { effort: 'high' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
        assert.match(result.message, /requires a "thinking" block/);
    });

    // §6.1 #2: Effort + disabled thinking → rejected
    it('rejects effort with disabled thinking', () => {
        const body = validBase({
            thinking: { type: 'disabled' },
            output_config: { effort: 'high' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
        assert.match(result.message, /cannot be used with thinking\.type "disabled"/);
    });

    // §6.1 #3: Effort + enabled (budget_tokens present) → rejected
    it('rejects effort with explicit budget_tokens', () => {
        const body = validBase({
            thinking: { type: 'enabled', budget_tokens: 10000 },
            output_config: { effort: 'high' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
        assert.match(result.message, /mutually exclusive/);
    });

    // §6.1 #4: Adaptive without effort → rejected
    it('rejects adaptive thinking without effort', () => {
        const body = validBase({
            thinking: { type: 'adaptive' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
        assert.match(result.message, /requires "output_config\.effort"/);
    });
});

describe('RFC-015 Validator — Valid Combinations', () => {
    // §6.1 #5: Effort + adaptive (valid)
    it('accepts adaptive thinking with effort', () => {
        const body = validBase({
            thinking: { type: 'adaptive' },
            output_config: { effort: 'medium' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });

    it('accepts adaptive thinking with all effort levels', () => {
        for (const effort of ['low', 'medium', 'high']) {
            const body = validBase({
                thinking: { type: 'adaptive' },
                output_config: { effort },
            });
            const result = validateMessagesRequest(body);

            assert.equal(result.valid, true, `effort "${effort}" should be accepted`);
        }
    });

    // §6.1 #6: Enabled + budget_tokens, no effort (unchanged behavior)
    it('accepts enabled thinking with budget_tokens and no effort', () => {
        const body = validBase({
            thinking: { type: 'enabled', budget_tokens: 10000 },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });

    // §6.1 #7: Disabled, no effort (unchanged behavior)
    it('accepts disabled thinking with no effort', () => {
        const body = validBase({
            thinking: { type: 'disabled' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });
});

describe('RFC-015 Validator — AJV Schema Rejection', () => {
    // §6.1 #8: Invalid effort enum value → rejected by AJV
    it('rejects invalid effort enum value', () => {
        const body = validBase({
            thinking: { type: 'adaptive' },
            output_config: { effort: 'ultra' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
    });

    it('rejects non-string effort value', () => {
        const body = validBase({
            thinking: { type: 'adaptive' },
            output_config: { effort: 42 },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
    });
});
