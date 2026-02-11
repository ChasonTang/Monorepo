# RFC-{NNN}: {Feature Title}

**Version:** 1.0  
**Author:** {Name}  
**Date:** {YYYY-MM-DD}  
**Status:** Proposed | Implemented

---

## 1. Summary

{One paragraph, max 150 words. What is being proposed, why, and the core idea. A reader should understand the full picture in 30 seconds.}

## 2. Motivation

{What problem exists today? Be specific — reference real behavior, error messages, user complaints, or performance data. Avoid vague statements like "the system is not good enough."

What value does solving this bring? Describe the expected benefits — e.g., improved developer experience, better performance, reduced complexity, or new capabilities unlocked.}

## 3. Goals and Non-Goals

**Goals:**
- {Concrete, measurable outcome 1}
- {Concrete, measurable outcome 2}

**Non-Goals:**
- {What this proposal explicitly does NOT address, and a one-sentence reason why}

## 4. Design

### 4.1 Overview

{High-level description of the approach. Include an architecture diagram (ASCII, Mermaid, or reference to an image file) showing the components involved and how they interact.}

### 4.2 Detailed Design

{The core of the document. Break into subsections as needed. Each subsection should cover one coherent aspect of the design.}

#### 4.2.1 {Aspect 1: e.g., Data Model / API / Algorithm / State Machine}

{Describe the design with enough detail that another engineer could implement it. Include code snippets, schemas, or protocol definitions where they add clarity.}

#### 4.2.2 {Aspect 2}

{Continue as needed.}

### 4.3 Design Rationale

{For each significant design decision, explain why this approach was chosen. Focus on the key trade-offs and constraints that shaped the design. If alternative approaches were considered, briefly mention them and the reason they were ruled out — exhaustive comparison is not required.}

## 5. Interface Changes *(Optional)*

{Describe any changes to public APIs, CLIs, configuration, file formats, or protocols. Skip this section if no public interfaces are affected.}

**Before:**
```
{existing interface}
```

**After:**
```
{proposed interface}
```

## 6. Backward Compatibility & Migration *(Optional)*

{Describe any backward compatibility implications and migration paths for existing users. Skip this section if the change is fully backward-compatible or if this is a new feature with no prior behavior.}

- **Breaking changes:** {List any behaviors, APIs, or configurations that will stop working or change semantics.}
- **Migration path:** {How should existing users adapt? Provide concrete steps or code examples.}

## 7. Implementation Plan

{Break the work into phases. Each phase should be independently shippable or at least independently verifiable.}

### Phase 1: {Name} — {Estimated Time}

- [ ] {Task with enough detail to be a work item}
- [ ] {Task}

**Done when:** {Specific acceptance criteria}

### Phase 2: {Name} — {Estimated Time}

- [ ] {Task}

**Done when:** {Specific acceptance criteria}

## 8. Testing Strategy

{Describe the overall testing approach: what types of testing are planned (unit, integration, end-to-end, manual), what areas require the most coverage, and any testing infrastructure or tooling needed.}

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | {Happy path} | {Input} | {Expected} |
| 2 | {Edge case} | {Input} | {Expected} |
| 3 | {Error case} | {Input} | {Expected} |

## 9. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| {What could go wrong} | High / Med / Low | High / Med / Low | {Concrete action, not "we will monitor"} |

## 10. Future Work

{Ideas that are related but explicitly out of scope for this proposal. Recording them here prevents scope creep while preserving institutional memory.}

- {Idea 1 — brief rationale for deferral}
- {Idea 2}

## 11. References

- {Related documents, code paths, external links, prior art}

---

## Changelog

{Entries are listed in reverse chronological order (newest first).  
Only substantive design changes (scope, approach, interface, etc.) require a new entry and version bump. Implementation progress updates (e.g., checking off tasks) do not.}

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | {YYYY-MM-DD} | {Name} | Initial version |
