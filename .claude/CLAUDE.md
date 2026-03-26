# ublkpp - Userspace Block Device Driver

C++ library providing RAID0/1/10 support for Linux's userspace block (ublk) driver.

## Claude Workflow

For EVERY non-trivial task (code changes, bug fixes, features):

### 1. **ALWAYS Start with Planning**
- Use `EnterPlanMode` for any code implementation task
- Explore codebase to understand context
- **Ask questions during planning** - if anything is unclear, ask before committing
- Design approach before writing code
- Present plan for approval

**When to plan:** Multi-file changes, bug investigations, new features, refactoring, performance work
**Skip planning:** One-line fixes, docs-only, pure research

**Challenging proposals:**
- Think critically about user-proposed solutions during planning
- If the proposal seems suboptimal: explain concerns, present trade-offs, suggest alternatives, let user decide
- Constructive pushback is valuable - don't blindly accept

### 2. **Execute the Plan**
- Implement approved approach
- Follow development workflow below
- Stay focused on plan

### 3. **ALWAYS Finish with Review & Analysis**
After ANY task, perform self-review:
- Check for race conditions, memory leaks, edge cases
- Verify tests cover changes
- Confirm formatting applied
- Look for security vulnerabilities, performance issues
- Validate error handling
- Report findings, concerns, trade-offs

**This review is MANDATORY** - never skip.

## Quick Reference

Build environment configured in `~/.claude/CLAUDE.md` (local vs SSH).

```bash
# Build Debug (auto-runs tests)
conan build -s:h build_type=Debug --build missing ublkpp

# Release / Coverage / Sanitizers
conan build -s:h build_type=Release --build missing ublkpp
conan build -s:h build_type=Debug -o coverage=True --build missing ublkpp
conan build -s:h build_type=Debug -o sanitize=True --build missing ublkpp

# Format code (ALWAYS after changes)
./apply-clang-format.sh
./apply-clang-format.sh -v  # validate only
```

## Code Conventions

**Style:** 4-space indent, 120-char lines, `#pragma once`, left pointer alignment (`Type* ptr`), C++23

**Naming:**
- Classes: `PascalCase` (Raid1Disk)
- Functions: `snake_case` (async_iov)
- Members: `_snake_case` (_device)
- Constants: `k_snake_case` (k_page_size)
- Macros/Enums: `SCREAMING_SNAKE_CASE`
- Namespaces: lowercase (ublkpp)

**Error Handling:**
- Use `std::expected<T, std::error_condition>`
- Type alias: `io_result = std::expected<int, std::error_condition>`
- Log errors before returning: `DLOGE("...", strerror(errno))`

**Logging:**
- Use SISL macros: `RLOGW`, `DLOGE`, `TLOGD`, `TLOGE`, `LOGINFO`
- Modules: `ublksrv`, `ublk_tgt`, `ublk_raid`, `ublk_drivers`, `libiscsi`

## Development Workflow

**Every code change:**
1. Write code
2. Write tests (UNLESS ublksrv calls, docs, or build config only)
3. Apply clang-format: `./apply-clang-format.sh`
4. Build: `conan build -s:h build_type=Debug --build missing ublkpp` (auto-runs tests)

## Testing Guidelines

**Organization:**
- Location: `src/<component>/tests/`
- Naming: `test_*.cpp` or `*_test.cpp`
- Common utilities: `test_*_common.hpp`

**Framework:** Google Test (GTest/GMock)
- Use `EXPECT_*` for non-fatal, `ASSERT_*` for fatal assertions
- Mock external dependencies with GMock

**Patterns:**
- Unit tests: individual functions in isolation
- Integration tests: component interactions
- Test edge cases: boundaries, errors, race conditions

**Coverage targets:**
- High: 80%, Medium: 65%, Goal: 100% for new code
- Generate: `conan build -o coverage=True`

## RAID Specifics

**Types:** RAID0 (striping), RAID1 (2-way mirroring + bitmap), RAID10 (RAID0 of RAID1 pairs)

**RAID1 Bitmap:**
- 4 KiB pages, 32 KiB chunks (default)
- Memory: Each 4 KiB page tracks 1 GiB data
- Example: 2 TB volume = 8 MiB worst-case (100% dirty), ~0.8 MiB typical (10% dirty)

**Default config:** max_io_size=512KiB, nr_hw_queues=1, qdepth=128, chunk_size=32KiB

## Dependencies

**Core:** `sisl` (logging/options), `ublksrv` ([GitHub](https://github.com/ublk-org/ublksrv)), `iomgr` (test)
**Build:** Conan 2.0, CMake 3.x, C++23 compiler, clang-format

## Project Structure

```
src/
├── driver/   # FSDisk, iSCSIDisk, HomeBlkDisk
├── lib/      # UblkDisk base, utilities
├── metrics/  # IO, FSDisk, RAID metrics
├── raid/     # RAID0, RAID1 (bitmap, superblock)
└── target/   # ublkpp_tgt

include/ublkpp/  # Public headers
example/         # ublkpp_disk
```

## Git Workflow

- Main branch: `main`
- PR-based workflow with descriptive titles
- Update CHANGELOG.md and version in conanfile.py (minor for features, patch for fixes)
