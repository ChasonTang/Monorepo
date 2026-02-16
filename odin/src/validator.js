import Ajv from 'ajv';

// ─── JSON Schema Definition ─────────────────────────────────────────────────

/**
 * JSON Schema for the Anthropic Messages API `POST /v1/messages` request body.
 *
 * Derived from the Anthropic API specification. Validates all fields consumed
 * by `anthropicToGoogle()` in converter.js. Unknown fields are permitted
 * (additionalProperties is not restricted) for forward compatibility.
 *
 * @see https://docs.anthropic.com/en/api/messages/create
 */
const messagesRequestSchema = {
    type: 'object',
    required: ['model', 'messages', 'max_tokens', 'stream'],
    properties: {
        // ── Required Fields ──────────────────────────────────────────

        model: {
            type: 'string',
            minLength: 1,
        },

        messages: {
            type: 'array',
            minItems: 1,
            maxItems: 100000,
            items: {
                type: 'object',
                required: ['role', 'content'],
                properties: {
                    role: {
                        type: 'string',
                        enum: ['user', 'assistant'],
                    },
                    content: {
                        oneOf: [
                            { type: 'string' },
                            {
                                type: 'array',
                                minItems: 1,
                                items: {
                                    type: 'object',
                                    required: ['type'],
                                    properties: {
                                        type: { type: 'string' },
                                    },
                                },
                            },
                        ],
                    },
                },
            },
        },

        max_tokens: {
            type: 'integer',
            minimum: 1,
        },

        stream: {
            const: true,
        },

        // ── Optional Fields ──────────────────────────────────────────

        system: {
            oneOf: [
                { type: 'string' },
                {
                    type: 'array',
                    minItems: 1,
                    items: {
                        type: 'object',
                        required: ['type', 'text'],
                        properties: {
                            type: { const: 'text' },
                            text: { type: 'string' },
                        },
                    },
                },
            ],
        },

        temperature: {
            type: 'number',
            minimum: 0,
            maximum: 1,
        },

        top_p: {
            type: 'number',
            minimum: 0,
            maximum: 1,
        },

        top_k: {
            type: 'integer',
            minimum: 0,
        },

        stop_sequences: {
            type: 'array',
            items: { type: 'string' },
        },

        tools: {
            type: 'array',
            items: {
                type: 'object',
                required: ['name'],
                properties: {
                    name: {
                        type: 'string',
                        minLength: 1,
                    },
                    description: {
                        type: 'string',
                    },
                    input_schema: {
                        type: 'object',
                    },
                },
            },
        },

        thinking: {
            type: 'object',
            required: ['type'],
            properties: {
                type: {
                    type: 'string',
                    enum: ['enabled', 'disabled', 'adaptive'],
                },
                budget_tokens: {
                    type: 'integer',
                    minimum: 1024,
                },
            },
            if: { required: ['type'], properties: { type: { const: 'enabled' } } },
            then: { required: ['type', 'budget_tokens'] },
        },
    },
};

// ─── Ajv Compilation ─────────────────────────────────────────────────────────

// Schema compiled once at module load — no per-request overhead
const ajv = new Ajv({ allErrors: true });
const validate = ajv.compile(messagesRequestSchema);

// ─── Error Formatting ────────────────────────────────────────────────────────

/**
 * Format Ajv validation errors into a human-readable message.
 *
 * @param {import('ajv').ErrorObject[]} errors
 * @returns {string}
 */
function formatErrors(errors) {
    // Special-case: stream must be true (Odin-specific)
    const streamError = errors.find(
        (e) =>
            e.instancePath === '/stream' ||
            (e.keyword === 'required' && e.params.missingProperty === 'stream'),
    );
    if (streamError) {
        return 'Only streaming mode is supported. Set "stream": true in your request.';
    }

    // General case: format all errors
    const messages = errors.map((err) => {
        const path = err.instancePath
            ? `"${err.instancePath.slice(1).replace(/\//g, '.')}"`
            : 'request body';

        return `${path} ${err.message}`;
    });

    // Deduplicate and join
    const unique = [...new Set(messages)];

    return unique.length === 1 ? unique[0] : unique.join('; ');
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * Validate an Anthropic Messages API request body.
 *
 * @param {Object} body - Parsed JSON request body
 * @returns {{ valid: true } | { valid: false, message: string }}
 */
export function validateMessagesRequest(body) {
    if (validate(body)) {
        return { valid: true };
    }

    const message = formatErrors(validate.errors);

    return { valid: false, message };
}
