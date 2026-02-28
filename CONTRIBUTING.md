# Contributing to RS2V Custom Server

Thank you for your interest in contributing to the RS2V Custom Server! This document provides guidelines and instructions for contributing to the project.

For detailed build instructions and development workflow, see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). For the project architecture, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Table of Contents

- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Coding Standards](#coding-standards)
- [Commit Messages](#commit-messages)
- [Pull Request Process](#pull-request-process)
- [Issue Reporting](#issue-reporting)
- [Code of Conduct](#code-of-conduct)

---

## Getting Started

### Prerequisites

| Tool | Minimum Version | Notes |
|---|---|---|
| C++ Compiler | GCC 8 / Clang 7 / MSVC 2019 | C++17 support required |
| CMake | 3.15 | Build system |
| Git | 2.20 | Version control |
| .NET SDK | 7.0 | Only if working on scripting (`ENABLE_SCRIPTING=ON`) |

### Setting Up Your Development Environment

```bash
# 1. Fork the repository on GitHub

# 2. Clone your fork
git clone https://github.com/YOUR_USERNAME/smellslikenapalm.git
cd smellslikenapalm

# 3. Add the upstream remote
git remote add upstream https://github.com/Krilliac/smellslikenapalm.git

# 4. Build in debug mode
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build . --parallel

# 5. Run the tests to verify your setup
ctest --verbose
```

### Staying Up to Date

Before starting new work, sync your fork with upstream:

```bash
git fetch upstream
git checkout main
git merge upstream/main
```

---

## Development Workflow

### 1. Check TODO.md

Before starting major work, review [TODO.md](TODO.md) to see what's planned and avoid duplicating effort. Comment on the relevant issue or create one if none exists.

### 2. Create a Feature Branch

```bash
git checkout -b feature/short-description
# or
git checkout -b fix/issue-description
```

Branch naming conventions:
- `feature/` — new functionality
- `fix/` — bug fixes
- `refactor/` — code improvements without behavior changes
- `docs/` — documentation changes
- `test/` — test additions or improvements

### 3. Make Your Changes

- Write code following the [coding standards](#coding-standards)
- Add or update tests for any behavior changes
- Update documentation if your changes affect the public API or configuration

### 4. Test Your Changes

```bash
cd build
cmake --build . --parallel
ctest --verbose
```

Ensure all existing tests pass and any new functionality has test coverage.

### 5. Commit and Push

```bash
git add <specific files>
git commit -m "Add: brief description of changes"
git push origin feature/short-description
```

### 6. Open a Pull Request

Open a PR against the `main` branch on GitHub. See [Pull Request Process](#pull-request-process) for details.

---

## Coding Standards

### Language and Style

| Rule | Standard |
|---|---|
| **Language** | C++17 (`-std=c++17`) |
| **Classes** | `PascalCase` (`NetworkManager`, `PlayerState`) |
| **Functions/Methods** | `PascalCase` for public, `camelCase` for private/internal |
| **Variables** | `snake_case` for locals, `m_camelCase` for members |
| **Constants** | `kPascalCase` (`kMaxPlayers`, `kDefaultPort`) |
| **Namespaces** | `PascalCase` (`Network`, `Security`) |
| **Files** | `PascalCase.cpp` / `PascalCase.h` |
| **Indentation** | 4 spaces (no tabs) |
| **Line length** | 120 characters max |
| **Braces** | Same line for control structures, next line for functions/classes |

### Thread Safety

- All public API methods must be thread-safe unless explicitly documented otherwise.
- Use `std::shared_mutex` for read-heavy shared state.
- Use `std::mutex` for general critical sections.
- Prefer `std::atomic` for counters and flags.
- Follow the lock hierarchy documented in [ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Error Handling

- Use exceptions for unrecoverable errors.
- Use return codes or `std::optional` for expected failure cases.
- Mark non-throwing functions as `noexcept`.
- Use RAII for resource management.

### Performance

- Minimize allocations in hot paths (game loop, network processing).
- Prefer stack allocation (`std::array`) over heap allocation (`std::vector`) for fixed-size data.
- Use `const&` for large objects passed to functions.
- Use move semantics for ownership transfers.

---

## Commit Messages

Follow this format:

```
<Type>: <Short description>

<Optional longer description explaining the "why">
```

### Types

| Type | Use For |
|---|---|
| `Add` | New feature or file |
| `Fix` | Bug fix |
| `Update` | Enhancement to existing feature |
| `Refactor` | Code restructuring without behavior change |
| `Remove` | Removing code or files |
| `Docs` | Documentation changes |
| `Test` | Test additions or changes |
| `Build` | Build system changes |

### Examples

```
Add: player movement validation in SecurityManager

Implements server-side validation of client movement packets
to detect speed hacking. Checks velocity against configurable
max speed thresholds per movement state.
```

```
Fix: race condition in TelemetryManager snapshot creation

The snapshot mutex was not held during metric aggregation,
allowing concurrent reads of partially updated counters.
```

---

## Pull Request Process

### Before Opening a PR

1. Ensure all tests pass (`ctest --verbose`)
2. Ensure your code follows the coding standards
3. Update documentation for any API or configuration changes
4. Rebase on the latest `main` to resolve conflicts

### PR Description Template

When opening a PR, include:

```markdown
## Summary
Brief description of what this PR does and why.

## Changes
- List of specific changes made
- One item per bullet point

## Testing
- How were these changes tested?
- Any new tests added?

## Documentation
- Were docs updated? If not, why not?
```

### Review Process

1. All PRs require at least one code review approval
2. CI checks must pass (build, tests, static analysis)
3. Address all review comments before merging
4. Squash commits if requested by the reviewer

---

## Issue Reporting

### Bug Reports

Use the [Bug Report template](https://github.com/Krilliac/smellslikenapalm/issues/new?template=bug_report.md) and include:

- **Server version** and build configuration (CMake options)
- **Operating system** and hardware specs
- **Steps to reproduce** the issue
- **Relevant log files** and error messages (from `logs/server.log`)
- **Expected vs. actual behavior**
- **Configuration files** (sanitize passwords/tokens)

### Feature Requests

Use the [Feature Request template](https://github.com/Krilliac/smellslikenapalm/issues/new?template=feature_request.md) and include:

- **Problem description** — what limitation or need does this address?
- **Proposed solution** — how should the feature work?
- **Alternatives considered** — what other approaches were evaluated?
- **Use case** — who benefits and how?

---

## Code of Conduct

### Our Standards

- **Be respectful** — treat all contributors with courtesy and professionalism.
- **Be constructive** — provide helpful feedback focused on the code, not the person.
- **Be collaborative** — work together to improve the project.
- **Be patient** — contributors have varying experience levels and time availability.

### Unacceptable Behavior

- Harassment, discrimination, or personal attacks
- Trolling or deliberately inflammatory comments
- Publishing others' private information
- Any conduct that would be inappropriate in a professional setting

### Enforcement

Project maintainers may remove, edit, or reject contributions that violate these standards. Repeated violations may result in a temporary or permanent ban from the project.

---

## Questions?

- Open a [GitHub Discussion](https://github.com/Krilliac/smellslikenapalm/discussions) for questions
- Join the [Discord server](https://discord.gg/sd8HaMc8rh) for real-time help
- File an [issue](https://github.com/Krilliac/smellslikenapalm/issues) for bugs or feature requests

Thank you for contributing!
