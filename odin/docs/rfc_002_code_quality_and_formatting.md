# RFC-002: Code Quality Checking and Code Style Formatting

**Version:** 1.1  
**Author:** Chason Tang  
**Date:** 2026-02-12  
**Status:** Implemented

---

## 1. Summary

Odin currently has no automated code quality enforcement or formatting tooling. All six source files in `src/` follow a consistent style (4-space indentation, single quotes, semicolons, JSDoc comments), but this consistency relies entirely on human discipline. This RFC proposes introducing **ESLint v9** (flat config) for static analysis and **Prettier** for deterministic formatting, wired together via npm scripts. The setup adds only `devDependencies` — zero impact on the zero-runtime-dependency production profile.

## 2. Motivation

### 2.1 Current State

The `odin/src/` directory contains six JavaScript modules totaling ~1,500 lines:

| File | Lines | Responsibility |
|------|-------|---------------|
| `converter.js` | 1,149 | Anthropic ↔ Google format conversion |
| `server.js` | 278 | HTTP server and routing |
| `logger.js` | ~80 | NDJSON request logging |
| `constants.js` | 69 | API endpoints, headers, system instruction |
| `index.js` | 91 | CLI entry point |
| `cloudcode.js` | ~60 | Cloud Code API client |

All files share a uniform style, but enforcement is implicit. There are no linter configs, no formatter configs, no `devDependencies`, and no pre-commit checks.

### 2.2 Why Now

1. **Defect prevention** — Static analysis catches real bugs that manual review misses: unused variables hiding dead code paths, accidental `==` coercions, unreachable code after early returns, and missing `await` on promises. In a proxy that translates between two API formats, a single missed edge case in `converter.js` (1,149 lines) can silently corrupt payloads.

2. **Contributor onboarding** — As the project grows beyond a single author, codified rules eliminate style debates in code review. New contributors get instant feedback via CI and npm scripts, rather than learning conventions by reading existing code.

3. **Formatting determinism** — Manual formatting is inherently inconsistent across editors, sessions, and contributors. Prettier guarantees byte-identical output regardless of input formatting, making diffs minimal and review focused on logic changes.

4. **Minimal cost** — The project is small (6 files), so initial setup and remediation are trivial. Adding tooling now avoids the compounding cost of retrofitting it later when the codebase is larger.

## 3. Goals and Non-Goals

**Goals:**

- Introduce ESLint v9 (flat config) to catch logical errors and enforce code quality rules across all `src/` files.
- Introduce Prettier to enforce deterministic formatting aligned with the existing code style (4-space indent, single quotes, semicolons).
- Adopt the most widely-used code style and lint conventions in the JavaScript ecosystem (`eslint:recommended` + Prettier defaults) to minimize onboarding friction and maximize community alignment.
- Wire both tools into npm scripts (`lint`, `format`, `check`) for local and CI usage.
- Maintain the zero-runtime-dependency profile — all new packages are `devDependencies` only.

**Non-Goals:**

- Adding TypeScript or type checking — the project uses plain JavaScript with JSDoc annotations; type enforcement is a separate concern.
- Introducing a monorepo-level linting configuration — this RFC scopes tooling to `odin/` only.
- Enforcing commit message conventions (e.g., Conventional Commits) — orthogonal to code quality.
- Adding CI pipeline configuration — this RFC establishes local tooling; CI integration is deferred to future work.
- Adding pre-commit git hooks — this is a monorepo containing projects in different languages; a single `.git/hooks/pre-commit` cannot cleanly serve all sub-projects. Per-project quality gates are enforced via npm scripts instead.

## 4. Design

### 4.1 Overview

This RFC introduces two core tools — ESLint (static analysis) and Prettier (formatting) — orchestrated via npm scripts in `package.json`.

```
                          src/*.js
                   (6 modules, ~1,500 lines)
                             │
              ┌──────────────┼───────────────┐
              │   analyzed & formatted by    │
              ▼                              ▼
┌──────────────────────┐        ┌──────────────────────┐
│      ESLint v9       │        │       Prettier       │
│   eslint.config.js   │        │   .prettierrc.json   │
│                      │        │                      │
│ · Logical errors     │        │ · Code formatting    │
│ · Best practices     │        │ · Style consistency  │
│ · Code quality       │        │ · Deterministic      │
└──────────┬───────────┘        └───────────┬──────────┘
           │    ▲  eslint-config-prettier   │
           │    └──── disables conflicts ───┘
           │                                │
           └────────────┬───────────────────┘
                        │ orchestrated by
                        ▼
┌──────────────────────────────────────────────────────┐
│              package.json — npm scripts              │
│                                                      │
│  lint ──────────▶ eslint src/                        │
│  lint:fix ──────▶ eslint src/ --fix                  │
│  format ────────▶ prettier --write src/              │
│  format:check ──▶ prettier --check src/              │
│  check ─────────▶ eslint src/ && prettier --check    │
└──────────────────────────────────────────────────────┘

Developer workflow:

  Edit ──▶ npm run lint:fix ──▶ npm run format ──▶ npm run check ──▶ Commit
```

**Dependency Overview:**

| Package | Purpose | Role |
|---------|---------|------|
| `eslint` (^9.x) | Static analysis engine | Core linter |
| `@eslint/js` | ESLint's recommended JavaScript rules | Rule preset |
| `globals` | Global variable definitions (e.g., `process`, `console`) | Node.js environment support |
| `prettier` (^3.x) | Opinionated code formatter | Formatting |
| `eslint-config-prettier` | Disables ESLint rules that conflict with Prettier | Integration layer |

Total: **5 devDependencies**. No runtime dependencies added.

### 4.2 Detailed Design

#### 4.2.1 ESLint Configuration

**File:** `odin/eslint.config.js`

ESLint v9 uses the flat config format — a single `eslint.config.js` file exporting an array of configuration objects. No `.eslintrc.*` files, no `extends` chains.

```javascript
import js from '@eslint/js';
import globals from 'globals';
import eslintConfigPrettier from 'eslint-config-prettier';

export default [
    // ── Base: ESLint recommended rules ──────────────────────────────────
    js.configs.recommended,

    // ── Project configuration ───────────────────────────────────────────
    {
        files: ['src/**/*.js'],
        languageOptions: {
            ecmaVersion: 'latest',
            sourceType: 'module',
            globals: {
                ...globals.node,
            },
        },
        rules: {
            // ── Possible Errors ─────────────────────────────────────────
            'no-constant-binary-expression': 'error',
            'no-constructor-return': 'error',
            'no-promise-executor-return': 'error',
            'no-template-curly-in-string': 'warn',
            'no-unreachable-loop': 'error',

            // ── Best Practices ──────────────────────────────────────────
            'eqeqeq': ['error', 'always'],
            'no-eval': 'error',
            'no-implied-eval': 'error',
            'no-throw-literal': 'error',
            'prefer-promise-reject-errors': 'error',
            'no-return-assign': ['error', 'always'],
            'no-self-compare': 'error',
            'no-sequences': 'error',
            'no-unused-expressions': ['error', {
                allowShortCircuit: true,
                allowTernary: true,
            }],

            // ── Variables ───────────────────────────────────────────────
            'no-var': 'error',
            'prefer-const': ['error', { destructuring: 'all' }],
            'no-unused-vars': ['error', {
                argsIgnorePattern: '^_',
                varsIgnorePattern: '^_',
            }],

            // ── ES6+ ───────────────────────────────────────────────────
            'no-useless-rename': 'error',
            'object-shorthand': ['error', 'always'],
            'prefer-arrow-callback': ['error', { allowNamedFunctions: true }],
            'prefer-template': 'warn',
            'symbol-description': 'error',
        },
    },

    // ── Prettier conflict resolution (must be last) ─────────────────────
    eslintConfigPrettier,
];
```

**Design decisions:**

- **`argsIgnorePattern: '^_'`** — Allows intentionally unused parameters (common in callback signatures like `(req, res)` where only `res` is used) to be prefixed with `_` instead of triggering errors.
- **`eqeqeq: 'always'`** — The codebase already uses strict equality consistently; this codifies it.
- **`prefer-const`** — The codebase already favors `const`; enforcing it catches accidental `let` usage for never-reassigned bindings.
- **`eslintConfigPrettier` last** — This must be the final config entry to properly disable all ESLint rules that would conflict with Prettier's formatting.

#### 4.2.2 Prettier Configuration

**File:** `odin/.prettierrc.json`

```json
{
    "singleQuote": true,
    "tabWidth": 4,
    "semi": true,
    "trailingComma": "all",
    "printWidth": 100,
    "arrowParens": "always",
    "endOfLine": "lf"
}
```

**Configuration rationale (derived from existing code style):**

| Option | Value | Rationale |
|--------|-------|-----------|
| `singleQuote` | `true` | All existing files use single quotes consistently |
| `tabWidth` | `4` | All existing files use 4-space indentation |
| `semi` | `true` | All existing files use semicolons |
| `trailingComma` | `"all"` | Modern JS convention; cleaner diffs when adding items to arrays/objects/parameters |
| `printWidth` | `100` | Existing code wraps around 80–100 characters; 100 provides comfortable headroom |
| `arrowParens` | `"always"` | Consistent with existing arrow functions; aids clarity |
| `endOfLine` | `"lf"` | Unix line endings; prevents cross-platform diff noise |

No `.prettierignore` file is needed. Prettier v3 automatically respects `.gitignore` (which already includes `logs/`), and `node_modules/` is hardcoded as ignored by Prettier. This avoids maintaining a redundant ignore file.

#### 4.2.3 npm Scripts

The following scripts are added to `odin/package.json`:

```json
{
    "scripts": {
        "start": "node src/index.js",
        "lint": "eslint src/",
        "lint:fix": "eslint src/ --fix",
        "format": "prettier --write src/",
        "format:check": "prettier --check src/",
        "check": "eslint src/ && prettier --check src/"
    }
}
```

| Script | Purpose | Use Case |
|--------|---------|----------|
| `lint` | Run ESLint on all source files | Quick quality check during development |
| `lint:fix` | Run ESLint with auto-fix | Fix auto-fixable issues (e.g., `prefer-const`) |
| `format` | Run Prettier to format all source files | Reformat after edits |
| `format:check` | Check formatting without writing | CI gate — fails if any file is unformatted |
| `check` | Run both ESLint and Prettier check | Single command for CI validation |

**Recommended developer workflow:**

```
  Edit code → npm run lint:fix → npm run format → npm run check → Commit
```

### 4.3 Design Rationale

**Why ESLint v9 + Prettier over Biome?**

| Criterion | ESLint v9 + Prettier | Biome (alternative) |
|-----------|---------------------|---------------------|
| **Ecosystem maturity** | De facto standard; 30M+ weekly npm downloads | Growing but smaller ecosystem |
| **Rule coverage** | 300+ built-in rules + extensive plugin ecosystem | ~200 rules; gaps in Node.js-specific checks |
| **Formatting quality** | Prettier is the gold standard; handles edge cases accumulated over 8+ years | Good but occasionally diverges from community expectations |
| **IDE integration** | First-class support in VS Code, Cursor, WebStorm, etc. | Good VS Code support; less mature in other editors |
| **Configuration** | ESLint v9 flat config is clean; Prettier is near-zero-config | Single config file; simpler initial setup |
| **Performance** | Adequate for 6 files (~200ms total); not a bottleneck | Faster (Rust-based), but irrelevant at this scale |
| **Familiarity** | Near-universal among JavaScript developers | Requires learning a new tool |

**Decision:** ESLint v9 + Prettier. The ecosystem maturity, rule depth, and universal familiarity outweigh Biome's speed advantage (irrelevant at this project's scale). The two-tool approach is well-understood and has a proven integration path via `eslint-config-prettier`.

Biome was seriously considered for its single-tool simplicity and alignment with Odin's minimalism philosophy. It was ultimately rejected because:

1. **Rule coverage gaps** — Biome lacks several Node.js-specific rules that ESLint provides out of the box (e.g., `no-promise-executor-return`, advanced `no-unused-vars` patterns). For a proxy server handling format conversion, these quality checks matter.
2. **Ecosystem gravity** — Every JavaScript developer knows ESLint + Prettier. Biome requires onboarding. For an open-source project seeking contributors, familiarity reduces friction.
3. **Incremental adoption risk** — If we need a custom ESLint rule or plugin in the future (e.g., for request/response schema validation), ESLint's plugin architecture is mature and well-documented. Biome's plugin story is still evolving.

**Why `eslint-config-prettier` instead of ESLint Stylistic?**

ESLint v9 deprecated its built-in formatting rules. Two paths exist for handling style:

1. **Prettier + `eslint-config-prettier`** — Prettier handles all formatting; the config disables conflicting ESLint rules.
2. **`@stylistic/eslint-plugin`** — Community-maintained formatting rules within ESLint.

We chose Prettier because it is opinionated and deterministic — there are no formatting decisions to make or debate. The `@stylistic` plugin requires configuring dozens of individual rules (indent, quotes, semi, spacing, etc.), each with edge cases. Prettier resolves all of them with a single tool.

**Why `devDependencies` are acceptable:**

The project's zero-dependency philosophy applies to the **production runtime**. Development tooling is fundamentally different — it runs on the developer's machine, not in the deployed proxy. All five packages are `devDependencies` and will not be installed in production (`npm install --production` excludes them). The `package.json` will continue to have zero `dependencies`.

## 5. Implementation Plan

### Phase 1: Install Dependencies — 5 minutes

- [x] Run `npm install --save-dev eslint @eslint/js globals prettier eslint-config-prettier` in the `odin/` directory.
- [x] Verify `package.json` now lists all five packages under `devDependencies` with `^` version ranges.
- [x] Verify `dependencies` field remains absent (zero runtime deps preserved).

**Done when:** `npm ls --dev` shows all five packages installed.

### Phase 2: Add Configuration Files — 10 minutes

- [x] Create `odin/eslint.config.js` with the configuration from §4.2.1.
- [x] Create `odin/.prettierrc.json` with the configuration from §4.2.2.
- [x] Update `odin/package.json` to add the npm scripts from §4.2.3.

**Done when:** `npm run check` executes without configuration errors (it may report lint/format violations — that is expected).

### Phase 3: Remediate Existing Code — 20 minutes

- [x] Run `npm run lint:fix` to auto-fix any ESLint violations.
- [x] Run `npm run format` to reformat all source files with Prettier.
- [x] Run `npm run check` and verify zero violations.
- [x] Manually review the diff to confirm no behavioral changes were introduced by auto-fixing.

**Done when:** `npm run check` exits with code 0, and `git diff` shows only whitespace/style changes (no logic changes).

## 6. Testing Strategy

The tooling itself is well-tested upstream (ESLint and Prettier have extensive test suites). Our testing focuses on verifying correct configuration and integration.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | ESLint runs without config errors | `npm run lint` | Exits 0 or reports only code violations (no config parse errors) |
| 2 | Prettier runs without config errors | `npm run format:check` | Exits 0 or reports only formatting differences |
| 3 | Clean check after remediation | `npm run check` | Exits 0 — zero lint errors, zero format violations |
| 4 | ESLint catches real bugs | Introduce `let x = 1;` (never reassigned) | `prefer-const` error reported |
| 5 | ESLint catches equality issues | Introduce `if (a == b)` | `eqeqeq` error reported |
| 6 | Prettier enforces style | Save a file with double quotes | `npm run format:check` fails; `npm run format` rewrites to single quotes |
| 7 | Auto-fix works | `npm run lint:fix && npm run format` | All auto-fixable issues resolved |
| 8 | No runtime impact | `npm install --production && npm ls` | Zero packages installed; `npm run start` works |
| 9 | Behavioral equivalence | Run proxy before and after formatting | Request/response payloads are identical |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Prettier reformats existing code, creating a large diff | High | Low | This is a one-time cost. After the initial formatting commit, all future diffs are minimal. Use a dedicated commit (`chore: apply prettier formatting`) so `git blame` can skip it (`--ignore-rev`). |
| ESLint reports false positives on valid patterns | Low | Low | The configuration uses well-tested recommended rules. Any false positives can be suppressed with `// eslint-disable-next-line` or rule adjustment. |
| `converter.js` auto-fix introduces behavioral changes | Low | High | Phase 3 includes manual diff review. ESLint's `--fix` only applies safe transforms (e.g., `let` → `const`). Prettier changes only whitespace. Neither modifies logic. |
| Developer friction from new tooling | Low | Low | ESLint errors are actionable (not stylistic noise, since Prettier handles style). The `lint:fix` command resolves most issues automatically, and `npm run format` handles all formatting in one step. |
| Dependency version conflicts in monorepo | Low | Medium | All packages are scoped to `odin/`'s `devDependencies`. No hoisting or sharing with other monorepo packages. |

## 8. Future Work

- **CI pipeline integration** — Add an `npm run check` step to the CI workflow (GitHub Actions, etc.) to block merges with lint or format violations.
- **JSDoc validation** — Add `eslint-plugin-jsdoc` to enforce JSDoc completeness and correctness, complementing the existing JSDoc annotations in the codebase.
- **Import sorting** — Add `eslint-plugin-import` or Prettier's `@trivago/prettier-plugin-sort-imports` to enforce consistent import ordering.
- **Biome migration evaluation** — Revisit Biome in 6–12 months as its rule set and plugin ecosystem mature. If it reaches parity with ESLint for our use cases, consolidating to a single tool would reduce configuration complexity.

## 9. References

- [ESLint v9 Flat Config documentation](https://eslint.org/docs/latest/use/configure/configuration-files) — Official guide for the flat config format used in this RFC.
- [Prettier documentation](https://prettier.io/docs/) — Configuration options and rationale.
- [`eslint-config-prettier` README](https://github.com/prettier/eslint-config-prettier) — Explains which ESLint rules are disabled and why.
- `odin/src/` — All six source files that will be covered by the new tooling.
- `odin/package.json` — Package manifest (modification target for scripts and devDependencies).

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-12 | Chason Tang | Remove `.prettierignore` — Prettier v3 respects `.gitignore` by default |
| 1.0 | 2026-02-12 | Chason Tang | Initial version |
