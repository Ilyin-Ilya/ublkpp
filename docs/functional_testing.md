# Functional Testing Guide

## Overview

The functional test suite drives the ublkpp RAID and disk implementations through real `io_uring`
I/O paths — without the ublk kernel module — by using a custom [fio](https://github.com/axboe/fio)
external ioengine.

```
fio job file
     │
     │  ioengine=external:libublkpp_fio_engine.so
     ▼
libublkpp_fio_engine.so          (custom fio external engine)
     │  disk_type / disk_files / raid_chunk_size options
     ▼
disk_handle ───────────────► make_fs_disk / make_raid0_disk / make_raid1_disk
     │
     ▼
MockUblksrv
(io_uring SQE/CQE loop)
     │
     └──────────────────────► backing image files (*.img)
                                  (regular files on disk)
```

Each test writes data via `fio`, then reads it back with MD5 verification (`verify=md5
verify_fatal=1`). No kernel ublk module, no block device node, and no root privileges are needed.

---

## File Layout

```
src/tests/fio_engine/
├── CMakeLists.txt          # Build + CTest registration + `functional` target
├── ublkpp_fio_engine.cpp   # Engine implementation (init/queue/getevents/event/cleanup)
└── jobs/
    ├── fsdisk_rw.fio       # File-backed disk write-verify
    ├── raid0_rw.fio        # RAID0 write-verify
    ├── raid1_rw.fio        # RAID1 write-verify
    ├── raid10_rw.fio       # RAID10 write-verify
    ├── raid0_cross_stripe.fio   # RAID0 cross-stripe boundary (bs=96k)
    └── raid10_cross_stripe.fio  # RAID10 cross-stripe boundary (bs=96k)
```

Backing images land in `<build_dir>/src/tests/fio_engine/test_data/`.

---

## Running Tests

### Method 1 — Full Conan build (also runs unit tests)

```bash
conan build -s:h build_type=Debug --build missing ublkpp
```

### Method 2 — CMake `functional` target (functional tests only, no Conan re-run)

After a successful build, use the custom CMake target to re-run only functional tests:

```bash
cmake --build build/Debug --target functional
```

This invokes `ctest -L Functional` internally and streams output to the terminal.

### Method 3 — Raw ctest (most flexible)

```bash
# All functional tests
ctest --test-dir build/Debug -L Functional --output-on-failure --verbose

# Single test
ctest --test-dir build/Debug -R FunctionalRAID1 --verbose

# Parallel execution (all functional tests, 4 jobs)
ctest --test-dir build/Debug -L Functional -j4 --output-on-failure
```

> **Note:** The `FunctionalCleanup` fixture always runs first (it removes stale backing images),
> even when selecting a single test by name.

---

## Existing Jobs

| CTest name | Job file | Topology | bs | Size | What it exercises |
|---|---|---|---|---|---|
| `FunctionalFSDisk` | `fsdisk_rw.fio` | File-backed disk (1 file) | 4k–64k | 64 MiB | Baseline I/O, single-device path |
| `FunctionalRAID0` | `raid0_rw.fio` | RAID0 (2 files) | 4k–64k | 64 MiB | Stripe distribution, chunk alignment |
| `FunctionalRAID1` | `raid1_rw.fio` | RAID1 (2 files) | 4k–64k | 64 MiB | Mirror replication, bitmap tracking |
| `FunctionalRAID10` | `raid10_rw.fio` | RAID10 (4 files) | 4k–64k | 64 MiB | Full RAID10 stack (stripe + mirror) |
| `FunctionalRAID0CrossStripe` | `raid0_cross_stripe.fio` | RAID0 (2 files) | 96k fixed | 96 MiB | Cross-stripe splitting, iovec accumulation |
| `FunctionalRAID10CrossStripe` | `raid10_cross_stripe.fio` | RAID10 (4 files) | 96k fixed | 96 MiB | Cross-stripe through RAID0 + RAID1 layers |

### Cross-stripe tests explained

With `chunk_size=32k` and two RAID0 devices, a 96 KiB I/O (3 × chunk) maps as:

```
[0 .. 32k)   → device 0   (iovec #1 for dev0)
[32k .. 64k) → device 1   (iovec #1 for dev1, issued immediately)
[64k .. 96k) → device 0   (iovec #2 for dev0, issued with 2-iovec scatter)
```

Device 0 therefore accumulates two non-contiguous iovecs before `__distribute` issues the
combined call. This exercises the iovec-accumulation/wrap path that straight intra-chunk I/O
never reaches. The RAID10 variant adds RAID1 replication of that 2-iovec scatter-gather into
each mirror pair.

---

## How the Engine Works

### Initialisation (`ublkpp_init`)

1. Parses the custom fio options (`disk_type`, `disk_files`, `raid_chunk_size`).
2. Pre-allocates backing files via `posix_fallocate` (falls back to `ftruncate` on tmpfs).
3. Constructs the requested disk type:
   - **`fsdisk`** — `make_fs_disk(path)`
   - **`raid0`** — `make_raid0_disk(uuid, chunk_size, [make_fs_disk(...)...])`
   - **`raid1`** — `make_raid1_disk(uuid, make_fs_disk(a), make_fs_disk(b), "", true)`; `assume_clean`
     is passed because the sparse backing images read zero, skipping the new-array initial sync
   - **`raid10`** — `make_raid0_disk(uuid, chunk_size, [raid1_pair_a, raid1_pair_b])` (two RAID1 pairs
     striped together, likewise `assume_clean`)
4. Wraps the disk in `MockUblksrv` (io_uring SQE/CQE loop, no kernel module).
5. Allocates a tag array (size = `iodepth`) for in-flight I/O tracking.

UUIDs are derived deterministically from the backing-file paths using UUIDv5 (SHA1, OID
namespace), so the superblock check passes across fio job sections that reopen the same files.

### Custom fio options

| Option | Type | Default | Description |
|---|---|---|---|
| `disk_type` | string | `fsdisk` | Disk topology: `fsdisk`, `raid0`, `raid1`, `raid10` |
| `disk_files` | string | *(required)* | Colon-separated backing file paths |
| `raid_chunk_size` | int | `32768` | RAID chunk size in bytes |

### Environment variables (set by CMake)

| Variable | Used by |
|---|---|
| `ENGINE_SO` | Passed as `ioengine=external:${ENGINE_SO}` in job files |
| `TEST_FILE` | Single file-backed disk path |
| `TEST_FILE_A` … `TEST_FILE_D` | RAID backing paths (A/B for RAID0/1, A–D for RAID10) |
| `ASAN_OPTIONS=verify_asan_link_order=0` | ASan builds only — suppresses link-order warning |
| `LD_PRELOAD=<engine.so>` | Release/tcmalloc builds — works around static TLS block exhaustion |

### Completion handling

`ublkpp_queue` submits one io_uring SQE per fio `io_u` and returns `FIO_Q_QUEUED`.

`ublkpp_getevents` calls `MockUblksrv::poll(min_events, timeout)`. Because `poll()` drains
all available CQEs at once, it may return more completions than fio's `max_events`. Surplus
completions are cached in `EngineData::completion_cache` (a `std::deque`) and returned on
the next `getevents` call, preventing the iodepth=32 hang that occurs when completions are
silently dropped.

---

## Adding a New Job

### Step 1 — Write the `.fio` job file

```ini
# src/tests/fio_engine/jobs/my_new_test.fio
[global]
ioengine=external:${ENGINE_SO}
disk_type=raid0
disk_files=${TEST_FILE_A}:${TEST_FILE_B}
raid_chunk_size=32768
bsrange=4k-64k
iodepth=32
verify=md5
verify_fatal=1
time_based=0
direct=0

[write-verify]
rw=write
size=64m
```

### Step 2 — Register in `CMakeLists.txt`

Add the backing files to the `FunctionalCleanup` command:

```cmake
add_test(NAME FunctionalCleanup
    COMMAND ${CMAKE_COMMAND} -E rm -f
        ...
        ${TEST_DIR}/my_new_a.img ${TEST_DIR}/my_new_b.img  # <-- add here
)
```

Define the environment set, add the test, and set its properties:

```cmake
set(MY_NEW_ENV "ENGINE_SO=${ENGINE_SO}"
               "TEST_FILE_A=${TEST_DIR}/my_new_a.img"
               "TEST_FILE_B=${TEST_DIR}/my_new_b.img"
               ${FUNCTIONAL_ENV})

add_test(NAME FunctionalMyNew
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/my_new_test.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalMyNew PROPERTIES
    ENVIRONMENT "${MY_NEW_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 180
    LABELS "Functional"
)
```

### Step 3 — Build and run

```bash
conan build -s:h build_type=Debug --build missing ublkpp
# or, after a prior build:
cmake --build build/Debug --target functional
```

---

## Testing Different Parameters

### Different chunk size

Override `raid_chunk_size` directly in the job file or pass it on the fio command line:

```bash
fio --ioengine=external:build/Debug/.../libublkpp_fio_engine.so \
    --disk_type=raid0 --disk_files=a.img:b.img \
    --raid_chunk_size=65536 --rw=write --size=128m --verify=md5
```

### Higher iodepth

```ini
iodepth=128
```

Note: the engine pre-allocates a tag array of size `iodepth`, so larger values use more
memory but otherwise work without code changes.

### Larger I/O sizes

Increase `size` in the job file. The engine sizes backing files as `size + start_offset +
64 MiB` (metadata headroom), so no manual pre-allocation is needed.

### New topology

Add the topology case to `ublkpp_init` in `ublkpp_fio_engine.cpp` following the existing
`fsdisk`/`raid0`/`raid1`/`raid10` pattern, then add a job file and CMakeLists entry as
above.

---

## Timeouts

| Test | Timeout |
|---|---|
| `FunctionalFSDisk` | 120 s |
| `FunctionalRAID0` | 120 s |
| `FunctionalRAID1` | 180 s |
| `FunctionalRAID10` | 180 s |
| `FunctionalRAID0CrossStripe` | 180 s |
| `FunctionalRAID10CrossStripe` | 180 s |

RAID1 and RAID10 are slower because every write is replicated to both mirror members and the
dirty bitmap is updated.

---

## Coverage

The engine is compiled as a shared library (`libublkpp_fio_engine.so`) that fio loads via
`dlopen()`. After I/O is complete, fio calls `ublkpp_cleanup()` and then `dlclose()` — which
unmaps the `.so` before the process-level `atexit()` handlers run. This means gcov's normal
at-exit flushing never fires for code inside the engine.

To capture coverage data from the functional tests, the engine calls `__gcov_dump()` explicitly
at the end of `ublkpp_cleanup()`, while the `.so` is still mapped. This is gated on the
`BUILD_COVERAGE` preprocessor macro, which CMake sets only in coverage builds:

```bash
conan build -s:h build_type=Debug -o coverage=True --build missing ublkpp
```
