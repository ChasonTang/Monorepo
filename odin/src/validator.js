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
    $defs: {
        TextContentBlock: {
            type: 'object',
            required: ['type', 'text'],
            properties: {
                type: { const: 'text' },
                text: { type: 'string' },
            },
        },
        StringOrTextBlockArray: {
            oneOf: [
                { type: 'string' },
                {
                    type: 'array',
                    minItems: 1,
                    items: { $ref: '#/$defs/TextContentBlock' },
                },
            ],
        },
    },
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
                                    discriminator: { propertyName: 'type' },
                                    oneOf: [
                                        { $ref: '#/$defs/TextContentBlock' },
                                        {
                                            properties: {
                                                type: { const: 'thinking' },
                                                thinking: { type: 'string' },
                                                signature: { type: 'string' },
                                            },
                                            required: ['type', 'thinking', 'signature'],
                                        },
                                        {
                                            properties: {
                                                type: { const: 'tool_use' },
                                                id: { type: 'string' },
                                                name: { type: 'string' },
                                                input: { type: 'object' },
                                            },
                                            required: ['type', 'id', 'name', 'input'],
                                        },
                                        {
                                            properties: {
                                                type: { const: 'tool_result' },
                                                tool_use_id: { type: 'string' },
                                                content: { $ref: '#/$defs/StringOrTextBlockArray' },
                                            },
                                            required: ['type', 'tool_use_id'],
                                        },
                                    ],
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

        system: { $ref: '#/$defs/StringOrTextBlockArray' },

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
                required: ['name', 'input_schema'],
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
                        required: ['type'],
                        properties: {
                            type: { const: 'object' },
                        },
                    },
                },
            },
        },

        tool_choice: {
            type: 'object',
            required: ['type'],
            properties: {
                type: {
                    type: 'string',
                    enum: ['auto', 'any', 'none'],
                },
            },
        },

        thinking: {
            type: 'object',
            required: ['type'],
            discriminator: { propertyName: 'type' },
            oneOf: [
                {
                    properties: {
                        type: { const: 'enabled' },
                        budget_tokens: { type: 'integer', minimum: 1024 },
                    },
                    required: ['type', 'budget_tokens'],
                },
                {
                    properties: { type: { const: 'disabled' } },
                    required: ['type'],
                },
                {
                    properties: { type: { const: 'adaptive' } },
                    required: ['type'],
                },
            ],
        },

        output_config: {
            type: 'object',
            properties: {
                effort: {
                    type: 'string',
                    enum: ['low', 'medium', 'high'],
                },
            },
        },
    },
};

// ─── Ajv Compilation ─────────────────────────────────────────────────────────

// Schema compiled once at module load — no per-request overhead
const ajv = new Ajv({ allErrors: true, discriminator: true });
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

    // Special-case: unsupported content block type (discriminator rejects unknown type values)
    const contentBlockError = errors.find(
        (e) =>
            e.keyword === 'discriminator' &&
            e.params?.tag === 'type' &&
            /^\/messages\/\d+\/content\/\d+$/.test(e.instancePath),
    );
    if (contentBlockError) {
        const path = contentBlockError.instancePath.slice(1).replace(/\//g, '.');

        return (
            `Unsupported content block type "${contentBlockError.params.tagValue}" at ${path}. ` +
            `Supported types: text, thinking, tool_use, tool_result.`
        );
    }

    // Special-case: non-text content inside tool_result blocks
    const toolResultContentError = errors.find(
        (e) => e.keyword === 'const' && /\/content\/\d+\/content\/\d+\/type$/.test(e.instancePath),
    );
    if (toolResultContentError) {
        const path = toolResultContentError.instancePath.slice(1).replace(/\//g, '.');

        return `Only "text" content is supported inside tool_result blocks. Error at ${path}.`;
    }

    // Special-case: thinking oneOf produces verbose errors — collapse them
    const thinkingOneOf = errors.find(
        (e) => e.instancePath === '/thinking' && e.keyword === 'oneOf',
    );
    if (thinkingOneOf) {
        const inner = errors.find(
            (e) => e.instancePath.startsWith('/thinking/') && e.keyword === 'required',
        );
        if (inner) {
            return `"thinking.${inner.params.missingProperty}" is required when thinking.type is "enabled"`;
        }

        return '"thinking" must match one of: {type:"enabled", budget_tokens}, {type:"disabled"}, or {type:"adaptive"}';
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

// ─── Cross-Field Validation ──────────────────────────────────────────────────

/**
 * Validate semantic constraints that span multiple top-level fields.
 * Runs after AJV structural validation passes.
 *
 * @param {Object} body - Parsed JSON request body (already structurally valid)
 * @returns {string|null} Error message, or null if valid
 */
function validateCrossFieldConstraints(body) {
    const effort = body.output_config?.effort;

    if (effort) {
        if (!body.thinking) {
            return (
                '"output_config.effort" requires a "thinking" block. ' +
                'Set thinking.type to "adaptive" to use effort-based thinking budget.'
            );
        }

        if (body.thinking.type === 'disabled') {
            return (
                '"output_config.effort" cannot be used with thinking.type "disabled". ' +
                'Either remove output_config.effort or set thinking.type to "adaptive".'
            );
        }

        if (body.thinking.budget_tokens !== undefined) {
            return (
                '"output_config.effort" and "thinking.budget_tokens" are mutually exclusive. ' +
                'Use effort for automatic budget selection, ' +
                'or budget_tokens for explicit control, but not both.'
            );
        }
    }

    if (body.thinking?.type === 'adaptive' && !effort) {
        return (
            'thinking.type "adaptive" requires "output_config.effort" to specify thinking intensity. ' +
            'Add output_config: { effort: "low" | "medium" | "high" } to your request.'
        );
    }

    return null;
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * Validate an Anthropic Messages API request body.
 *
 * @param {Object} body - Parsed JSON request body
 * @returns {{ valid: true } | { valid: false, message: string }}
 */
export function validateMessagesRequest(body) {
    if (!validate(body)) {
        const message = formatErrors(validate.errors);

        return { valid: false, message };
    }

    const crossFieldError = validateCrossFieldConstraints(body);
    if (crossFieldError) {
        return { valid: false, message: crossFieldError };
    }

    return { valid: true };
}
