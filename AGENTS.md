# Agent Instructions for lvgltest

These guidelines apply to the entire repository.

## Scope and priorities
- Keep the overlay lightweight and efficient; avoid adding new runtime dependencies beyond what is already vendored.
- Prefer simple, locally-rendered assets for demos (bars and lottie fallback) instead of external downloads.
- Preserve backward compatibility where noted in `CONTRACT.md` and `config.json`, especially for configuration keys.

## Coding conventions
- Follow existing C style: K&R braces, 4-space indents, and early returns for error handling.
- Do not wrap `#include` directives or imports in `try/catch` (per global rules).
- When touching timing or UDP handling logic, document behavioral changes in `README.md` and `CONTRACT.md`.

## Testing
- If you run tests or builds, record the exact command and include it in the final response.
- Tests are not required unless the change is likely to affect runtime behavior.

## Documentation
- Keep `README.md`, `CONTRACT.md`, and sample configs in sync with code behavior when making functional changes.
- Prefer concise examples that match the default build (bar and lottie assets only).

## PR / Final response
- Summaries should mention user-visible behavior changes first, then refactors.
- Cite files and line ranges in the final response as required by system instructions.
