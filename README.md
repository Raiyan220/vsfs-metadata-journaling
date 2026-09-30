# VSFS Metadata Journaling Engine

[![Language: C](https://img.shields.io/badge/Language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Course: CSE321](https://img.shields.io/badge/Course-CSE321%20Operating%20Systems-orange.svg)]()
[![Standard: POSIX](https://img.shields.io/badge/API-POSIX%20I%2FO-yellow.svg)]()
[![Build: GCC](https://img.shields.io/badge/Compiler-GCC-green.svg)]()

A robust, crash-consistent Write-Ahead Logging (WAL) metadata journaling engine implemented for a Very Simple File System (**VSFS**) raw disk image (`vsfs.img`).

This system enforces ACID-style atomicity and durability guarantees across simulated power failures and torn writes during file allocation. It combines sequential transaction logging, two-pass lookahead replay verification, and strict block-level synchronization.

---

## Author

- **Md. Al-Raiyan** (Student ID: 22201562) — *Core Architecture, WAL Protocol, Replay Engine & Consistency Validation*

---

## On-Disk Architecture & Geometry

The filesystem operates directly on an 85-block raw block device with uniform **4096-byte blocks** (total 340 KB).

```text
=============================================================================================================
                                        VSFS PHYSICAL DISK LAYOUT (85 BLOCKS)
=============================================================================================================
 [Block 0]        [Blocks 1 – 16]     [Block 17]     [Block 18]     [Blocks 19 – 20]     [Blocks 21 – 84]
+------------+   +----------------+  +------------+ +------------+ +------------------+ +-------------------+
| Superblock |   | Metadata Log   |  | Inode Bmap | | Data Bmap  | | Inode Table      | | Data Blocks       |
| 1 Block    |   | 16 Blocks      |  | 1 Block    | | 1 Block    | | 2 Blocks         | | 64 Blocks         |
| Magic/Geom |   | State + WAL    |  | Inode Free | | Data Free  | | 64 Inodes Total  | | File/Dir Storage  |
+------------+   +----------------+  +------------+ +------------+ +------------------+ +-------------------+

```

### Region Specifications

| Block Range | Region | Size | Description |
| --- | --- | --- | --- |
| **Block 0** | Superblock | 4 KB | Contains filesystem signature (`0x56534653`), block geometry, and layout offsets. |
| **Blocks 1–16** | Journal Log | 64 KB | Dedicated WAL area. Block 1 maintains log runtime state (`verify_id: 0x4A524E4C`, `active_bytes`). Blocks 2–16 store transaction headers, disk chunk wrappers, and atomic seals. |
| **Block 17** | Inode Bitmap | 4 KB | 1-bit allocation flags tracking allocated vs. free inodes. |
| **Block 18** | Data Bitmap | 4 KB | 1-bit allocation flags tracking allocated vs. free data blocks. |
| **Blocks 19–20** | Inode Table | 8 KB | Array of 64 inodes (32 inodes/block @ 128 bytes each). Inode 0 is permanently assigned to the Root Directory `/`. |
| **Blocks 21–84** | Data Blocks | 256 KB | 64 contiguous data blocks allocated for directory entries and file contents. Block 21 holds root directory payloads. |

---

## Core Engineering & Protocols

### 1. Write-Ahead Logging (WAL) Protocol

Every file allocation initiated via `create <filename>` writes changes to the journal before writing them to fixed disk locations:

1. **Inode Bitmap Block:** Marks the newly allocated Inode ID bit as active.
2. **Inode Table Block(s):** Initializes file metadata (mode, links, timestamps) and updates Root Inode size.
3. **Directory Data Block:** Records the `(inode_id, name)` dirent pair in the root directory.
4. **Transaction Seal (`ENTRY_TYPE_SEAL`):** Commits the entire transaction sequence atomically to mark it valid and ready for replay.

### 2. Atomic Replay with Lookahead Verification (`install`)

To guard against torn writes or power failures during journal appending, `process_sync` implements a **two-pass lookahead verification**:

* **Pass 1 (Scan):** Scans the log stream forward looking for an `ENTRY_TYPE_SEAL` marker. If a seal is absent (indicating a torn or partial transaction), the parser halts and safely ignores the incomplete payload.
* **Pass 2 (Replay):** Replays all verified payload wrappers (`ENTRY_TYPE_META`) directly to their final on-disk addresses (`target_disk_idx`) using `pwrite`, then invokes `fsync()`.
* **Log Reset:** Resets `active_bytes = sizeof(struct log_state_block)` to safely reclaim the journal space.

### 3. The Split-Buffer Collision Fix

* **Problem:** Root Inode 0 resides in Block 19. New files with Inode IDs 1–31 also reside in Block 19. Maintaining separate in-memory disk buffers for the directory update (modifying Inode 0) and the new file inode caused buffer collisions during replay, destroying the root directory size update.
* **Resolution:** When `target_blk_idx == fs_head.inode_start`, the engine synchronizes the root inode via `memcpy` into the target inode block buffer before logging, writing a unified Block 19 record instead of conflicting split entries.

---

## Capacity Bounds & Invariants

* **Journal Capacity Limit:** Exactly **4 pending transactions** without running `install`.
* Available log data space: 15 blocks (Block 1 is reserved for journal metadata).
* Per-transaction footprint: ~3.01 blocks (3 data blocks + headers).
* A 5th concurrent transaction is safely rejected with: `Log space exhausted. Run install.`


* **Disk Allocation Limit:** Exactly **64 files** (Inode 0 for root + 63 user files).
* Attempting to create a 64th user file is rejected with: `Error: ID limit reached.`


* **Consistency Verification:** The filesystem passes all `./validator` structural checks at every boundary and state transition.

---

## Build & Usage

### Compilation

Ensure you have GCC installed, then build the toolchain:

```bash
gcc -Wall -Wextra -O2 mkfs.c -o mkfs
gcc -Wall -Wextra -O2 validator.c -o validator
gcc -Wall -Wextra -O2 journal.c -o journal

```

### Basic Workflow

```bash
# 1. Format a fresh disk image
./mkfs

# 2. Check initial clean state
./validator

# 3. Create a file via write-ahead logging
./journal create "notes.txt"

# 4. Install / Replay committed journal to disk
./journal install

# 5. Confirm structural consistency
./validator

```

---

## Automated Verification Scripts

You can use these bash loops to stress-test the system's boundary limits.

### 1. Journal Capacity Boundary Test

Exhausts the 16-block journal area to validate graceful transaction rejection:

```bash
rm -f vsfs.img && ./mkfs
for i in {1..6}; do
    ./journal create "j_file_$i"
done
./validator

```

### 2. Disk Saturation Test

Cycles `create` and `install` alternatingly to fill all 64 inodes and check boundary stops:

```bash
rm -f vsfs.img && ./mkfs
for i in {1..70}; do
   output=$(./journal create "file_$i")
   if [[ "$output" == *"ID limit"* ]]; then
      echo "Boundary reached safely at File $i:$output"
      break
   fi
   ./journal install > /dev/null
done
./validator

```

```

```
