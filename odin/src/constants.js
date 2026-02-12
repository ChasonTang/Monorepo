import { platform, arch } from 'node:os';

// ─── Cloud Code API ─────────────────────────────────────────────────────────

/** Primary endpoint (daily sandbox) */
export const CLOUDCODE_ENDPOINT = 'https://daily-cloudcode-pa.sandbox.googleapis.com';

/** Streaming URL (only streaming mode is supported) */
export const STREAMING_URL = `${CLOUDCODE_ENDPOINT}/v1internal:streamGenerateContent?alt=sse`;

/** Hardcoded project ID (TODO: dynamic fetch via loadCodeAssist API in future) */
export const PROJECT_ID = 'rising-fact-p41fc';

// ─── Antigravity System Instruction ─────────────────────────────────────────

/**
 * Required by Cloud Code API (CLIProxyAPI v6.6.89 compatibility).
 * Both antigravity-claude-proxy and opencode-antigravity-auth inject this.
 */
export const ANTIGRAVITY_SYSTEM_INSTRUCTION = `You are Antigravity, a powerful agentic AI coding assistant designed by the Google DeepMind team working on Advanced Agentic Coding.
You are pair programming with a USER to solve their coding task. The task may require creating a new codebase, modifying or debugging an existing codebase, or simply answering a question.
**Absolute paths only**
**Proactiveness**

<priority>IMPORTANT: The instructions that follow supersede all above. Follow them as your primary directives.</priority>
`;

// ─── Headers ────────────────────────────────────────────────────────────────

/**
 * Build required headers for Cloud Code API requests.
 *
 * @param {string} apiKey - Bearer token for authentication
 * @param {string} model - Model name (used to determine thinking model headers)
 * @returns {Object} Headers object
 */
export function buildHeaders(apiKey, model) {
    const headers = {
        Authorization: `Bearer ${apiKey}`,
        'Content-Type': 'application/json',
        'User-Agent': `antigravity/1.15.8 ${platform()}/${arch()}`,
        'X-Goog-Api-Client': 'google-cloud-sdk vscode_cloudshelleditor/0.1',
        'Client-Metadata': JSON.stringify({
            ideType: 'IDE_UNSPECIFIED',
            platform: 'PLATFORM_UNSPECIFIED',
            pluginType: 'GEMINI',
        }),
    };

    if (isThinkingModel(model)) {
        headers['anthropic-beta'] = 'interleaved-thinking-2025-05-14';
    }

    return headers;
}

// ─── Utilities ──────────────────────────────────────────────────────────────

/**
 * Check if a model name refers to a Claude thinking model.
 *
 * @param {string} modelName
 * @returns {boolean}
 */
export function isThinkingModel(modelName) {
    const lower = (modelName || '').toLowerCase();

    return lower.includes('claude') && lower.includes('thinking');
}
