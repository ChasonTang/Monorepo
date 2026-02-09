import crypto from 'node:crypto';

import { ANTIGRAVITY_SYSTEM_INSTRUCTION, isThinkingModel } from './constants.js';

// ─── Utilities ──────────────────────────────────────────────────────────────

/**
 * Generate a random hex string.
 *
 * @param {number} bytes - Number of random bytes
 * @returns {string} Hex-encoded random string
 */
function randomHex(bytes) {
    return crypto.randomBytes(bytes).toString('hex');
}

// ─── Content Block Conversion (Anthropic → Google) ──────────────────────────

/**
 * Extract text content from a tool_result content field.
 *
 * @param {string|Array|*} content
 * @returns {string}
 */
function extractTextContent(content) {
    if (typeof content === 'string') return content;
    if (Array.isArray(content)) {
        return content
            .filter(c => c.type === 'text')
            .map(c => c.text)
            .join('\n');
    }
    return '';
}

/**
 * Convert Anthropic content blocks to Google parts.
 * Extracts only known fields, ignoring cache_control etc.
 *
 * @param {string|Array|*} content - Anthropic content (string or block array)
 * @returns {Array} Google parts array
 */
function convertContentToParts(content) {
    if (typeof content === 'string') {
        return [{ text: content }];
    }

    if (!Array.isArray(content)) {
        return [{ text: String(content) }];
    }

    const parts = [];

    for (const block of content) {
        switch (block.type) {
            case 'text':
                if (block.text?.trim()) {
                    parts.push({ text: block.text });
                }
                break;

            case 'image':
                if (block.source?.type === 'base64') {
                    parts.push({
                        inlineData: {
                            mimeType: block.source.media_type,
                            data: block.source.data
                        }
                    });
                }
                break;

            case 'tool_use':
                parts.push({
                    functionCall: {
                        id: block.id,
                        name: block.name,
                        args: block.input || {}
                    }
                });
                break;

            case 'tool_result':
                // NOTE: name uses tool_use_id as a simplification. Claude matches
                // by id field so this works. See Future Improvements (§6.1) for
                // proper function name lookup.
                parts.push({
                    functionResponse: {
                        id: block.tool_use_id,
                        name: block.tool_use_id,
                        response: { result: extractTextContent(block.content) }
                    }
                });
                break;

            case 'thinking':
                // Only include thinking blocks with valid signatures
                if (block.signature?.length >= 50) {
                    parts.push({
                        text: block.thinking,
                        thought: true,
                        thoughtSignature: block.signature
                    });
                }
                break;
        }
    }

    return parts;
}

// ─── Request Conversion (Anthropic → Google) ────────────────────────────────

/**
 * Convert Anthropic Messages API request to Google Generative AI format.
 * Only extracts known fields, naturally ignoring cache_control and other
 * Anthropic-specific fields.
 *
 * @param {Object} anthropicRequest - Anthropic format request
 * @returns {Object} Google format request
 */
export function anthropicToGoogle(anthropicRequest) {
    const { model, messages, system, max_tokens, temperature,
            top_p, top_k, stop_sequences, tools, thinking } = anthropicRequest;

    const googleRequest = {
        contents: [],
        generationConfig: {}
    };

    // Convert system instruction with Antigravity identity injection
    // Both reference implementations require this for CLIProxyAPI v6.6.89 compatibility
    const systemParts = [{ text: ANTIGRAVITY_SYSTEM_INSTRUCTION }];

    if (system) {
        const userSystemParts = typeof system === 'string'
            ? [{ text: system }]
            : system.filter(b => b.type === 'text').map(b => ({ text: b.text }));
        systemParts.push(...userSystemParts);
    }

    googleRequest.systemInstruction = {
        role: 'user',
        parts: systemParts
    };

    // Convert messages to contents
    for (const msg of messages) {
        const parts = convertContentToParts(msg.content);
        // Google API requires at least one part per content message.
        // This can happen when all thinking blocks are filtered out (unsigned).
        if (parts.length === 0) {
            parts.push({ text: '.' });
        }
        googleRequest.contents.push({
            role: msg.role === 'assistant' ? 'model' : 'user',
            parts
        });
    }

    // Generation config
    if (max_tokens) googleRequest.generationConfig.maxOutputTokens = max_tokens;
    if (temperature !== undefined) googleRequest.generationConfig.temperature = temperature;
    if (top_p !== undefined) googleRequest.generationConfig.topP = top_p;
    if (top_k !== undefined) googleRequest.generationConfig.topK = top_k;
    if (stop_sequences?.length) googleRequest.generationConfig.stopSequences = stop_sequences;

    // Thinking config for thinking models
    if (isThinkingModel(model)) {
        const thinkingBudget = thinking?.budget_tokens;
        googleRequest.generationConfig.thinkingConfig = {
            include_thoughts: true,
            ...(thinkingBudget ? { thinking_budget: thinkingBudget } : {})
        };

        // Validate max_tokens > thinking_budget (API requirement)
        if (thinkingBudget && max_tokens && max_tokens <= thinkingBudget) {
            googleRequest.generationConfig.maxOutputTokens = thinkingBudget + 8192;
        }
    }

    // Tools
    if (tools?.length) {
        googleRequest.tools = [{
            functionDeclarations: tools.map(tool => ({
                name: tool.name,
                description: tool.description || '',
                parameters: tool.input_schema || { type: 'object' }
            }))
        }];
        googleRequest.toolConfig = {
            functionCallingConfig: { mode: 'VALIDATED' }
        };
    }

    return googleRequest;
}

// ─── Response Conversion (Google → Anthropic) ───────────────────────────────

/**
 * Convert Google response to Anthropic format.
 *
 * @param {Object} googleResponse - Google Generative AI response
 * @param {string} model - Model name for the response
 * @returns {Object} Anthropic Messages API response
 */
export function googleToAnthropic(googleResponse, model) {
    const response = googleResponse.response || googleResponse;
    const candidates = response.candidates || [];
    const firstCandidate = candidates[0] || {};
    const parts = firstCandidate.content?.parts || [];

    const content = [];
    let hasToolCalls = false;

    for (const part of parts) {
        if (part.text !== undefined) {
            if (part.thought === true) {
                content.push({
                    type: 'thinking',
                    thinking: part.text,
                    signature: part.thoughtSignature || ''
                });
            } else {
                content.push({ type: 'text', text: part.text });
            }
        } else if (part.functionCall) {
            content.push({
                type: 'tool_use',
                id: part.functionCall.id || `toolu_${randomHex(12)}`,
                name: part.functionCall.name,
                input: part.functionCall.args || {}
            });
            hasToolCalls = true;
        }
    }

    // Determine stop reason
    const finishReason = firstCandidate.finishReason;
    let stopReason = 'end_turn';
    if (finishReason === 'MAX_TOKENS') stopReason = 'max_tokens';
    else if (hasToolCalls) stopReason = 'tool_use';

    // Usage calculation
    const usage = response.usageMetadata || {};
    const cachedTokens = usage.cachedContentTokenCount || 0;

    return {
        id: `msg_${randomHex(16)}`,
        type: 'message',
        role: 'assistant',
        content: content.length > 0 ? content : [{ type: 'text', text: '' }],
        model,
        stop_reason: stopReason,
        stop_sequence: null,
        usage: {
            input_tokens: (usage.promptTokenCount || 0) - cachedTokens,
            output_tokens: usage.candidatesTokenCount || 0,
            cache_read_input_tokens: cachedTokens,
            cache_creation_input_tokens: 0
        }
    };
}
