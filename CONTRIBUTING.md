# Contributing

Thanks for your interest in MimiClaw. This project welcomes focused, high-quality contributions that improve reliability, performance, or documentation.

## Scope

We accept contributions in these areas:

- Core firmware features and bug fixes
- Documentation, examples, and diagrams
- Build and tooling improvements
- Tests and CI enhancements

> **External Peripherals**
> We are not accepting PRs that add or change external peripherals right now.

## Before You Start

- Search existing issues and discussions to avoid duplication.
- Open a short proposal for large or risky changes.
- Keep changes small and reviewable when possible.

## Development Setup

- Install ESP-IDF v5.5+.
- Build targets are in `idf.py`.
- Default config lives in `main/mimi_secrets.h.example`.

## Branching and Commits

- Use a short, descriptive branch name.
- Keep commit history clean and focused.
- Suggested commit style: `docs: ...`, `fix: ...`, `feat: ...`.

## Pull Requests

- Describe the problem and the solution clearly.
- Include testing steps and results.
- Update documentation when behavior changes.

## Code Style

- Match existing naming and formatting.
- Prefer clarity over cleverness.
- Avoid large refactors mixed with functional changes.

## Tests

- Add or update tests when behavior changes.
- If tests are not available, explain why and how you validated the change.

## Documentation

- Keep README and docs in sync with behavior changes.
- Add concise examples for new features.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
