/**
 * Tests for RFC-015: Effort-Based Thinking Budget Resolution — Converter
 *
 * Covers §6.2 scenarios:
 * - Adaptive + effort (low/medium/high) → resolved thinking_budget from lookup table
 * - Enabled + budget_tokens only → unchanged behavior
 * - Disabled thinking → unchanged behavior
 * - Effort budget ≥ max_tokens → maxOutputTokens bumped to 64000
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { anthropicToGoogle } from '../src/converter.js';
import { CLAUDE_THINKING_MAX_OUTPUT_TOKENS } from '../src/constants.js';

function baseRequest(overrides = {}) {
    return {
        messages: [{ role: 'user', content: 'hello' }],
        system: 'You are helpful.',
        max_tokens: 4096,
        ...overrides,
    };
}

describe('RFC-015 Converter — Effort Budget Resolution', () => {
    // §6.2 #1: Adaptive + effort low → thinking_budget: 2048
    it('resolves effort "low" to thinking_budget 2048', () => {
        const req = baseRequest({
            thinking: { type: 'adaptive' },
            output_config: { effort: 'low' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.generationConfig.thinkingConfig, {
            include_thoughts: true,
            thinking_budget: 2048,
        });
    });

    // §6.2 #2: Adaptive + effort medium → thinking_budget: 8192
    it('resolves effort "medium" to thinking_budget 8192', () => {
        const req = baseRequest({
            thinking: { type: 'adaptive' },
            output_config: { effort: 'medium' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.generationConfig.thinkingConfig, {
            include_thoughts: true,
            thinking_budget: 8192,
        });
    });

    // §6.2 #3: Adaptive + effort high → thinking_budget: 16384
    it('resolves effort "high" to thinking_budget 16384', () => {
        const req = baseRequest({
            thinking: { type: 'adaptive' },
            output_config: { effort: 'high' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.generationConfig.thinkingConfig, {
            include_thoughts: true,
            thinking_budget: 16384,
        });
    });

    // §6.2 #4: Enabled + budget_tokens only → thinking_budget from budget_tokens directly
    it('passes through budget_tokens for enabled thinking (no effort)', () => {
        const req = baseRequest({
            thinking: { type: 'enabled', budget_tokens: 10000 },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.generationConfig.thinkingConfig, {
            include_thoughts: true,
            thinking_budget: 10000,
        });
    });

    // §6.2 #5: Disabled thinking → include_thoughts: false, thinking_budget: undefined
    it('sets include_thoughts false for disabled thinking', () => {
        const req = baseRequest({
            thinking: { type: 'disabled' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.generationConfig.thinkingConfig, {
            include_thoughts: false,
            thinking_budget: undefined,
        });
    });

    // §6.2 #6: Effort budget ≥ max_tokens → maxOutputTokens bumped to 64000
    it('bumps maxOutputTokens when effort budget exceeds max_tokens', () => {
        const req = baseRequest({
            max_tokens: 1024,
            thinking: { type: 'adaptive' },
            output_config: { effort: 'medium' },
        });
        const result = anthropicToGoogle(req);

        assert.equal(result.generationConfig.thinkingConfig.thinking_budget, 8192);
        assert.equal(result.generationConfig.maxOutputTokens, CLAUDE_THINKING_MAX_OUTPUT_TOKENS);
    });

    it('does not bump maxOutputTokens when max_tokens exceeds effort budget', () => {
        const req = baseRequest({
            max_tokens: 32000,
            thinking: { type: 'adaptive' },
            output_config: { effort: 'low' },
        });
        const result = anthropicToGoogle(req);

        assert.equal(result.generationConfig.thinkingConfig.thinking_budget, 2048);
        assert.equal(result.generationConfig.maxOutputTokens, 32000);
    });
});
