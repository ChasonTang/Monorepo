# Odin Technical Design

**Document Version:** 2.0  
**Author:** Chason Tang  
**Last Updated:** 2025-02-05  
**Status:** Draft

---

## 1. Executive Summary

Odin is a minimal Node.js proxy server that enables Claude Code CLI to communicate with Google Antigravity Cloud Code API. It exposes an Anthropic-compatible Messages API (`/v1/messages`) and translates requests/responses between Anthropic and Google Generative AI formats.

### 1.1 Design Principles

This design follows a **first-principles approach**:

1. **Minimal Dependencies**: Use native Node.js APIs (zero runtime dependencies)
2. **Protocol Translation Only**: Focus on the core task of format conversion
3. **Explicit Field Mapping**: Only extract fields we understand, naturally ignoring unknown fields
4. **Incremental Validation**: Start simple, add complexity only when proven necessary

### 1.2 Background

Claude Code CLI requires an Anthropic-compatible API endpoint at `ANTHROPIC_BASE_URL`. However, Google Antigravity Cloud Code provides a proprietary API format. This proxy bridges the gap by:

1. Accepting Anthropic Messages API requests from Claude Code
2. Converting them to Google Generative AI format
3. Forwarding to Antigravity Cloud Code API
4. Converting responses back to Anthropic format

### 1.3 Scope

| In Scope | Out of Scope |
|----------|--------------|
| Anthropic ↔ Google format translation | Gemini model support |
| Claude model family only | Multi-account management |
| CLI-provided API key authentication | OAuth authentication flow |
| Streaming (SSE) support | Cross-model conversation switching |
| Single endpoint fallback | WebUI management interface |

---

## 2. Technical Design

### 2.1 Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────┐
│                              Odin                                      │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌───────────────┐    ┌──────────────────┐    ┌──────────────────┐     │
│  │ Claude Code   │───▶│  Native HTTP     │───▶│ Antigravity      │     │
│  │ CLI           │◀───│  Server          │◀───│ Cloud Code API   │     │
│  └───────────────┘    └──────────────────┘    └──────────────────┘     │
│        │                      │                       │                │
│        │ Anthropic            │                       │ Google         │
│        │ Messages API         │                       │ Generative AI  │
│        │                      │                       │                │
│  ┌─────▼──────────────────────▼───────────────────────▼──────────┐     │
│  │                       converter.js                            │     │
│  │  ┌──────────────────┐              ┌──────────────────┐       │     │
│  │  │ anthropicToGoogle│              │ googleToAnthropic│       │     │
│  │  └──────────────────┘              └──────────────────┘       │     │
│  └───────────────────────────────────────────────────────────────┘     │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow

```
Claude Code CLI                      Odin                       Cloud Code API
      │                                │                               │
      │  POST /v1/messages             │                               │
      │  (Anthropic JSON)              │                               │
      │───────────────────────────────▶│                               │
      │                                │  anthropicToGoogle()          │
      │                                │  ─────────────────▶           │
      │                                │                               │
      │                                │  POST /v1internal:streamGenerateContent
      │                                │  (Google JSON + Bearer Token) │
      │                                │──────────────────────────────▶│
      │                                │                               │
      │                                │◀──────────────────────────────│
      │                                │  SSE Stream (Google format)   │
      │                                │                               │
      │                                │  googleToAnthropic()          │
      │                                │  ◀─────────────────           │
      │◀───────────────────────────────│                               │
      │  SSE Stream (Anthropic format) │                               │
      │                                │                               │
```

### 2.3 Directory Structure

```
odin/
├── docs/
│   └── odin_proxy_design.md    # This document
├── src/
│   ├── index.js                # Entry point, CLI argument parsing
│   ├── server.js               # Native HTTP server, route handlers
│   ├── converter.js            # Bidirectional format conversion
│   ├── cloudcode.js            # Cloud Code API client
│   └── constants.js            # API endpoints, headers
├── package.json                # No runtime dependencies
└── README.md
```

### 2.4 Protocol Translation

#### 2.4.1 Request Format Mapping

| Anthropic Field | Google Field | Conversion |
|-----------------|--------------|------------|
| `model` | `model` (in wrapper) | Pass through |
| `messages` | `contents` | See Content Conversion |
| `messages[].role` | `contents[].role` | `assistant` → `model`, `user` → `user` |
| `messages[].content` | `contents[].parts` | See Content Block Conversion |
| `system` | `systemInstruction.parts` | String → `[{text}]`, Array → filter text blocks |
| `max_tokens` | `generationConfig.maxOutputTokens` | Direct mapping |
| `temperature` | `generationConfig.temperature` | Direct mapping |
| `top_p` | `generationConfig.topP` | Direct mapping |
| `top_k` | `generationConfig.topK` | Direct mapping |
| `stop_sequences` | `generationConfig.stopSequences` | Direct mapping |
| `tools` | `tools[0].functionDeclarations` | See Tool Conversion |
| `thinking.budget_tokens` | `generationConfig.thinkingConfig.thinking_budget` | Direct mapping |

**Note**: Fields not listed (e.g., `cache_control`, `metadata`) are naturally ignored by only extracting known fields.

#### 2.4.2 Content Block Conversion (Anthropic → Google)

| Anthropic Block | Google Part |
|-----------------|-------------|
| `{type: "text", text}` | `{text}` |
| `{type: "image", source: {type: "base64", media_type, data}}` | `{inlineData: {mimeType, data}}` |
| `{type: "tool_use", id, name, input}` | `{functionCall: {id, name, args}}` |
| `{type: "tool_result", tool_use_id, content}` | `{functionResponse: {id, name, response}}` |
| `{type: "thinking", thinking, signature}` | `{text, thought: true, thoughtSignature}` (if signature ≥ 50 chars) |

#### 2.4.3 Response Format Mapping (Google → Anthropic)

| Google Field | Anthropic Field | Conversion |
|--------------|-----------------|------------|
| `candidates[0].content.parts` | `content` | See Part Conversion |
| `candidates[0].finishReason` | `stop_reason` | `STOP` → `end_turn`, `MAX_TOKENS` → `max_tokens` |
| `usageMetadata.promptTokenCount` | `usage.input_tokens` | Subtract `cachedContentTokenCount` |
| `usageMetadata.candidatesTokenCount` | `usage.output_tokens` | Direct mapping |
| `usageMetadata.cachedContentTokenCount` | `usage.cache_read_input_tokens` | Direct mapping |

#### 2.4.4 Part Conversion (Google → Anthropic)

| Google Part | Anthropic Block |
|-------------|-----------------|
| `{text}` | `{type: "text", text}` |
| `{text, thought: true, thoughtSignature}` | `{type: "thinking", thinking, signature}` |
| `{functionCall: {id, name, args}}` | `{type: "tool_use", id, name, input}` |
| `{inlineData: {mimeType, data}}` | `{type: "image", source: {type: "base64", media_type, data}}` |

#### 2.4.5 Tool Definition Conversion

```javascript
// Anthropic format
{
    name: "read_file",
    description: "Read file contents",
    input_schema: { type: "object", properties: {...} }
}

// Google format
{
    name: "read_file",
    description: "Read file contents",
    parameters: { type: "object", properties: {...} }
}
```

**Note**: Claude models require `toolConfig.functionCallingConfig.mode = "VALIDATED"`.

### 2.5 API Endpoints and Headers

#### 2.5.1 Cloud Code API Endpoints

```javascript
// Primary endpoint (daily)
const CLOUDCODE_ENDPOINT = 'https://daily-cloudcode-pa.googleapis.com';

// Streaming URL
const STREAMING_URL = `${CLOUDCODE_ENDPOINT}/v1internal:streamGenerateContent?alt=sse`;
```

#### 2.5.2 Required Headers

```javascript
import { platform, arch } from 'node:os';

const headers = {
    'Authorization': `Bearer ${apiKey}`,
    'Content-Type': 'application/json',
    'User-Agent': `antigravity/1.15.8 ${platform()}/${arch()}`,
    'X-Goog-Api-Client': 'google-cloud-sdk vscode_cloudshelleditor/0.1',
    'Client-Metadata': JSON.stringify({
        ideType: 'IDE_UNSPECIFIED',
        platform: 'PLATFORM_UNSPECIFIED',
        pluginType: 'GEMINI'
    })
};

// For Claude thinking models only
if (isThinkingModel(model)) {
    headers['anthropic-beta'] = 'interleaved-thinking-2025-05-14';
}
```

#### 2.5.3 Cloud Code Request Wrapper

```javascript
const payload = {
    project: 'rising-fact-p41fc',  // Default project ID
    model: model,
    request: googleRequest,        // The converted Google-format request
    userAgent: 'antigravity',
    requestType: 'agent',
    requestId: `agent-${crypto.randomUUID()}`
};
```

### 2.6 Core Algorithms

#### 2.6.1 Request Conversion

```javascript
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

    // Convert system instruction
    if (system) {
        googleRequest.systemInstruction = {
            parts: typeof system === 'string'
                ? [{ text: system }]
                : system.filter(b => b.type === 'text').map(b => ({ text: b.text }))
        };
    }

    // Convert messages to contents
    for (const msg of messages) {
        const parts = convertContentToParts(msg.content);
        if (parts.length > 0) {
            googleRequest.contents.push({
                role: msg.role === 'assistant' ? 'model' : 'user',
                parts
            });
        }
    }

    // Generation config
    if (max_tokens) googleRequest.generationConfig.maxOutputTokens = max_tokens;
    if (temperature !== undefined) googleRequest.generationConfig.temperature = temperature;
    if (top_p !== undefined) googleRequest.generationConfig.topP = top_p;
    if (top_k !== undefined) googleRequest.generationConfig.topK = top_k;
    if (stop_sequences?.length) googleRequest.generationConfig.stopSequences = stop_sequences;

    // Thinking config for thinking models
    if (isThinkingModel(model)) {
        googleRequest.generationConfig.thinkingConfig = {
            include_thoughts: true,
            thinking_budget: thinking?.budget_tokens
        };
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
```

#### 2.6.2 Content Block Conversion

```javascript
/**
 * Convert Anthropic content to Google parts.
 * Extracts only known fields, ignoring cache_control etc.
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
```

#### 2.6.3 Response Conversion

```javascript
/**
 * Convert Google response to Anthropic format.
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
```

#### 2.6.4 SSE Stream Conversion

```javascript
/**
 * Stream and convert Google SSE events to Anthropic format.
 *
 * @param {ReadableStream} stream - Response body stream
 * @param {string} model - Model name
 * @yields {string} Anthropic SSE event lines
 */
export async function* streamSSEResponse(stream, model) {
    const messageId = `msg_${randomHex(16)}`;
    let blockIndex = 0;
    let currentBlockType = null;
    let inputTokens = 0;
    let outputTokens = 0;
    let cacheReadTokens = 0;
    let stopReason = null;
    let hasEmittedStart = false;

    const reader = stream.getReader();
    const decoder = new TextDecoder();
    let buffer = '';

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
            if (!line.startsWith('data:')) continue;

            const jsonText = line.slice(5).trim();
            if (!jsonText) continue;

            try {
                const data = JSON.parse(jsonText);
                const innerResponse = data.response || data;

                // Update usage
                const usage = innerResponse.usageMetadata;
                if (usage) {
                    inputTokens = usage.promptTokenCount || inputTokens;
                    outputTokens = usage.candidatesTokenCount || outputTokens;
                    cacheReadTokens = usage.cachedContentTokenCount || cacheReadTokens;
                }

                const parts = innerResponse.candidates?.[0]?.content?.parts || [];

                // Emit message_start on first content
                if (!hasEmittedStart && parts.length > 0) {
                    hasEmittedStart = true;
                    yield formatSSE('message_start', {
                        type: 'message_start',
                        message: {
                            id: messageId,
                            type: 'message',
                            role: 'assistant',
                            content: [],
                            model,
                            stop_reason: null,
                            stop_sequence: null,
                            usage: {
                                input_tokens: inputTokens - cacheReadTokens,
                                output_tokens: 0,
                                cache_read_input_tokens: cacheReadTokens,
                                cache_creation_input_tokens: 0
                            }
                        }
                    });
                }

                // Process parts
                for (const part of parts) {
                    if (part.thought === true) {
                        // Thinking block
                        if (currentBlockType !== 'thinking') {
                            if (currentBlockType !== null) {
                                yield formatSSE('content_block_stop', { type: 'content_block_stop', index: blockIndex });
                                blockIndex++;
                            }
                            currentBlockType = 'thinking';
                            yield formatSSE('content_block_start', {
                                type: 'content_block_start',
                                index: blockIndex,
                                content_block: { type: 'thinking', thinking: '' }
                            });
                        }
                        yield formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: { type: 'thinking_delta', thinking: part.text || '' }
                        });

                        // Emit signature if present
                        if (part.thoughtSignature?.length >= 50) {
                            yield formatSSE('content_block_delta', {
                                type: 'content_block_delta',
                                index: blockIndex,
                                delta: { type: 'signature_delta', signature: part.thoughtSignature }
                            });
                        }

                    } else if (part.text !== undefined) {
                        // Text block
                        if (currentBlockType !== 'text') {
                            if (currentBlockType !== null) {
                                yield formatSSE('content_block_stop', { type: 'content_block_stop', index: blockIndex });
                                blockIndex++;
                            }
                            currentBlockType = 'text';
                            yield formatSSE('content_block_start', {
                                type: 'content_block_start',
                                index: blockIndex,
                                content_block: { type: 'text', text: '' }
                            });
                        }
                        yield formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: { type: 'text_delta', text: part.text }
                        });

                    } else if (part.functionCall) {
                        // Tool use block
                        if (currentBlockType !== null) {
                            yield formatSSE('content_block_stop', { type: 'content_block_stop', index: blockIndex });
                            blockIndex++;
                        }
                        currentBlockType = 'tool_use';
                        stopReason = 'tool_use';

                        const toolId = part.functionCall.id || `toolu_${randomHex(12)}`;
                        yield formatSSE('content_block_start', {
                            type: 'content_block_start',
                            index: blockIndex,
                            content_block: {
                                type: 'tool_use',
                                id: toolId,
                                name: part.functionCall.name,
                                input: {}
                            }
                        });
                        yield formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: {
                                type: 'input_json_delta',
                                partial_json: JSON.stringify(part.functionCall.args || {})
                            }
                        });
                    }
                }

                // Check finish reason
                const finishReason = innerResponse.candidates?.[0]?.finishReason;
                if (finishReason && !stopReason) {
                    stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
                }

            } catch (e) {
                // Skip malformed JSON
            }
        }
    }

    // Close final block
    if (currentBlockType !== null) {
        yield formatSSE('content_block_stop', { type: 'content_block_stop', index: blockIndex });
    }

    // Emit message_delta and message_stop
    yield formatSSE('message_delta', {
        type: 'message_delta',
        delta: { stop_reason: stopReason || 'end_turn', stop_sequence: null },
        usage: { output_tokens: outputTokens }
    });

    yield formatSSE('message_stop', { type: 'message_stop' });
}

function formatSSE(event, data) {
    return `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
}
```

---

## 3. Interface Design

### 3.1 CLI Interface

```
Usage: node src/index.js --api-key=<key> [--port=<port>] [--debug]

Options:
  --api-key=<key>     API key for Antigravity Cloud Code (required)
  --port=<port>       Port to listen on (default: 8080)
  --debug             Enable debug logging

Examples:
  node src/index.js --api-key="ya29.a0AeO..."
  node src/index.js --api-key="ya29..." --port=3000 --debug
```

### 3.2 Exposed Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/messages` | POST | Main Anthropic Messages API endpoint |
| `/health` | GET | Health check (returns `{"status":"ok"}`) |
| `/` | POST | Silent handler for Claude Code heartbeat |
| `/api/event_logging/batch` | POST | Silent handler for Claude Code telemetry |

### 3.3 Request/Response Examples

**Request (POST /v1/messages):**
```json
{
  "model": "claude-sonnet-4-5-thinking",
  "messages": [
    { "role": "user", "content": "Hello!" }
  ],
  "max_tokens": 4096,
  "stream": true
}
```

**Response (Streaming SSE):**
```
event: message_start
data: {"type":"message_start","message":{"id":"msg_abc123","type":"message","role":"assistant","content":[],"model":"claude-sonnet-4-5-thinking","stop_reason":null,"usage":{"input_tokens":10,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello!"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}

event: message_stop
data: {"type":"message_stop"}
```

### 3.4 Error Handling

| Condition | HTTP Status | Response |
|-----------|-------------|----------|
| Missing API key on startup | N/A | Exit with error message |
| API authentication failed | 401 | `{"type":"error","error":{"type":"authentication_error","message":"..."}}` |
| Rate limited | 429 | `{"type":"error","error":{"type":"rate_limit_error","message":"..."}}` |
| Invalid request | 400 | `{"type":"error","error":{"type":"invalid_request_error","message":"..."}}` |
| Server error | 500 | `{"type":"error","error":{"type":"api_error","message":"..."}}` |

---

## 4. Implementation Plan

### Phase 1: Core Infrastructure (2 hours)

**Task 1.1: Project Setup**
- [ ] Create `package.json` with `"type": "module"` (no runtime dependencies)
- [ ] Implement CLI argument parsing in `index.js`
- [ ] Implement native HTTP server in `server.js`

**Task 1.2: Constants**
- [ ] Define Cloud Code endpoint
- [ ] Define required headers
- [ ] Implement `isThinkingModel()` utility

**Acceptance Criteria:**
- Server starts and listens on configured port
- Health endpoint returns `{"status":"ok"}`

### Phase 2: Protocol Conversion (3 hours)

**Task 2.1: Request Converter**
- [ ] Implement `anthropicToGoogle()` function
- [ ] Implement `convertContentToParts()` for all block types
- [ ] Handle system instructions, generation config, tools

**Task 2.2: Response Converter**
- [ ] Implement `googleToAnthropic()` function
- [ ] Handle all part types (text, thinking, functionCall)

**Acceptance Criteria:**
- Can convert all documented content block types
- Round-trip conversion produces valid format

### Phase 3: Cloud Code Client (2 hours)

**Task 3.1: API Client**
- [ ] Implement `sendRequest()` with proper headers
- [ ] Build Cloud Code request wrapper

**Task 3.2: SSE Stream Handler**
- [ ] Implement `streamSSEResponse()` generator
- [ ] Real-time conversion of SSE events

**Acceptance Criteria:**
- Can make authenticated requests to Cloud Code API
- Streaming responses are converted correctly

### Phase 4: Integration (1 hour)

**Task 4.1: Wire Everything Together**
- [ ] Implement `/v1/messages` route handler
- [ ] Handle streaming vs non-streaming mode
- [ ] Error handling and response formatting

**Acceptance Criteria:**
- Claude Code CLI can connect and have a conversation
- Streaming works without lag or data loss

---

## 5. Testing

### 5.1 Manual Testing

```bash
# 1. Start Odin
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080

# 2. Configure Claude Code
export ANTHROPIC_BASE_URL=http://localhost:8080
export ANTHROPIC_API_KEY=dummy
export ANTHROPIC_MODEL=claude-sonnet-4-5-thinking

# 3. Start Claude Code
claude

# 4. Test conversation
> Hello, how are you?
> What is 2 + 2?
```

### 5.2 Test Cases

| Scenario | Expected Behavior |
|----------|-------------------|
| Simple text message | Response with text content |
| Streaming response | SSE events in correct order |
| Thinking model | Response includes thinking blocks |
| Tool use | Tool calls returned correctly |
| Tool result follow-up | Conversation continues after tool execution |

---

## 6. Items To Verify

These items may or may not be necessary. They should be verified during integration testing:

| Item | Status | Notes |
|------|--------|-------|
| Tool schema sanitization | ⏳ To verify | Google API may have stricter schema validation |
| Antigravity system instruction injection | ⏳ To verify | Referenced in antigravity-claude-proxy, unclear if required |
| Empty parts placeholder | ⏳ To verify | API may require non-empty parts array |
| Session ID for caching | ⏳ To verify | May improve prompt cache hit rate |

---

## 7. Appendix

### 7.1 Utility Functions

```javascript
import crypto from 'node:crypto';

function randomHex(bytes) {
    return crypto.randomBytes(bytes).toString('hex');
}

function isThinkingModel(modelName) {
    const lower = (modelName || '').toLowerCase();
    return lower.includes('claude') && lower.includes('thinking');
}
```

### 7.2 API Key Source

The API key can be extracted from the Antigravity database:

- **macOS**: `~/Library/Application Support/Antigravity/User/globalStorage/state.vscdb`
- **Windows**: `%APPDATA%/Antigravity/User/globalStorage/state.vscdb`
- **Linux**: `~/.config/Antigravity/User/globalStorage/state.vscdb`

Query: `SELECT value FROM ItemTable WHERE key = 'antigravityAuthStatus'`

The returned JSON contains an `apiKey` field.

### 7.3 References

1. [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)
2. [antigravity-claude-proxy](../antigravity-claude-proxy/) - Reference implementation (for reference only)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 2.0 | 2025-02-05 | Chason Tang | Redesign from first principles: native Node.js, simplified structure, removed unnecessary workarounds |
| 1.0 | 2025-02-05 | Chason Tang | Initial version |

---

*End of Technical Design Document*
