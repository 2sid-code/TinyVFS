# TinyVFS

A tiny virtual file system built in C, with a Python CLI to drive it. TinyVFS is a hands-on exploration of how filesystems work under the hood. Cluster allocation, directory tables, bitmap tracking packed into a single binary disk image you can poke at, break, and learn from.

> [WARNING]
> **ALPHA SOFTWARE - DISK FORMAT IS UNSTABLE**
> TinyVFS is in active development. The on-disk format (cluster layout, table structure, bitmap encoding) **may change without notice between versions.** Virtual disks created with the current version are **not guaranteed to be readable by future versions.** Do not rely on TinyVFS disks for anything you cannot afford to lose. Always keep originals of any files you import.

---

## Table of Contents

- [How It Works](#how-it-works)
- [Requirements](#requirements)
- [Setup](#setup)
- [Usage](#usage)
- [Disk Layout](#disk-layout)
- [Limitations](#limitations)
- [License](#license)

---

## How It Works

TinyVFS simulates a block-storage device inside a single binary file on your host OS. The design is intentionally straightforward: the C engine (`src/vfs.c`) handles cluster reads/writes, bitmap-based page allocation, and a flat root directory table, all the moving parts of a real filesystem, kept small enough to read in an afternoon. The Python CLI (`tinyvfs.py`) wraps the compiled shared library via `ctypes` and exposes a clean command-line interface.

```
┌─────────────────────────────────────────────────────┐
│                  Virtual Disk File                  │
│  ┌────────────┬────────────┬────────────────────┐   │
│  │ Cluster 0  │ Cluster 1  │ Clusters 2–N       │   │
│  │ Root Table │   Bitmap   │   File Data        │   │
│  └────────────┴────────────┴────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

Each cluster is 64 KB. The root directory table (Cluster 0) stores up to 2048 file entries. The allocation bitmap (Cluster 1) tracks which 256-byte pages are in use. Every design choice is visible and traceable. There's nothing hiding behind layers of abstraction.

---

## Requirements

- **GCC** (or any C99-compatible compiler)
- **Python 3.8+**
- **pip**
- A POSIX-compatible OS (Linux or macOS). Windows is untested.

---

## Setup

### 1. Clone the repository

```bash
git clone https://github.com/your-username/tinyvfs.git
cd tinyvfs
```

### 2. Build the C shared library

```bash
make
```

This compiles `src/vfs.c` into `libtinyvfs.so` in the project root. The Python CLI expects the `.so` file to be in the same directory as `tinyvfs.py`.

### 3. Install Python dependencies

```bash
pip install -r requirements.txt
```

### 4. Verify the setup

```bash
python tinyvfs.py --help
```

You should see the TinyVFS command list printed to your terminal.

---

## Usage

All commands follow the pattern:

```bash
python tinyvfs.py <command> [arguments] [options]
```

---

### `create` - Format a new virtual disk

```bash
python tinyvfs.py create <disk> [--size BYTES]
```

Creates a new empty virtual disk image at the given path.

| Argument | Description |
|----------|-------------|
| `disk` | Output path for the new disk file (e.g. `my.bin`) |
| `--size` | Disk size in bytes (default: 1048576 / 1 MB). Rounded down to the nearest 64 KB cluster. |

**Example:**
```bash
python tinyvfs.py create my.bin --size 4194304   # 4 MB disk
```

---

### `import` - Write a host file into the VFS

```bash
python tinyvfs.py import <disk> <host_path> <vfs_path>
```

Reads a file from your OS and stores it inside the virtual disk under the given VFS filename.

| Argument | Description |
|----------|-------------|
| `disk` | Path to an existing virtual disk |
| `host_path` | Path to the file on your host OS |
| `vfs_path` | Name to store it under inside the VFS (max 26 characters) |

**Example:**
```bash
python tinyvfs.py import my.bin ./photo.jpg photo.jpg
```

---

### `export` - Read a VFS file back to the host OS

```bash
python tinyvfs.py export <disk> <vfs_path> <host_path>
```

Copies a file from the virtual disk to a path on your host OS.

| Argument | Description |
|----------|-------------|
| `disk` | Path to an existing virtual disk |
| `vfs_path` | Name of the file inside the VFS |
| `host_path` | Destination path on your host OS |

**Example:**
```bash
python tinyvfs.py export my.bin photo.jpg ./recovered_photo.jpg
```

---

### `ls` - List all files in the VFS

```bash
python tinyvfs.py ls <disk>
```

Prints a table of all files stored in the root directory, including their allocated page count and physical location on the disk.

**Example:**
```bash
python tinyvfs.py ls my.bin
```

```
Filename                     | Size (Pages) | Location (C/P)
-----------------------------------------------------------------
photo.jpg                    | 14           | 2/0
notes.txt                    | 1            | 2/14
```

---

### `rm` - Delete a file from the VFS

```bash
python tinyvfs.py rm <disk> <vfs_path>
```

Removes a file's directory entry and frees its allocated pages in the bitmap.

| Argument | Description |
|----------|-------------|
| `disk` | Path to an existing virtual disk |
| `vfs_path` | Name of the file inside the VFS to delete |

**Example:**
```bash
python tinyvfs.py rm my.bin photo.jpg
```

---

## Disk Layout

| Cluster | Purpose |
|---------|---------|
| 0 | Root directory table (up to 2048 × 32-byte entries) |
| 1 | Allocation bitmap (1 bit per 256-byte page) |
| 2+ | File data |

- **Page size:** 256 bytes
- **Cluster size:** 64 KB (256 pages)
- **Max clusters:** 65,536 (4 GB theoretical maximum)
- **Max filename length:** 26 characters
- **Max files:** 2,048 entries in the root directory

Files are stored contiguously. There is no fragmentation support. If a large enough contiguous run of free pages cannot be found, the write will fail. This constraint is a useful illustration of why real-world filesystems invest in extent trees and free-space management.

---

## Limitations

TinyVFS keeps its scope tight by design. Understanding *why* these constraints exist is half the point.

- **Flat root directory only.** There are no subdirectories. A good starting point for exploring how directory trees are typically built on top of simpler structures.
- **Contiguous allocation only.** Heavily fragmented disks may fail to store files even when total free space is sufficient. Deleting files and re-importing can help.
- **No journaling or crash recovery.** A process kill mid-write may corrupt the disk image, exactly the kind of failure that motivated journaling in production filesystems.
- **No duplicate filename detection.** Importing two files with the same VFS name will create two entries; the second will shadow the first on reads.
- **26-character filename limit.** Longer names are silently truncated.
- **Alpha disk format.** See the warning at the top of this document.

---

## License

MIT License. See `LICENSE` for details.

---

*A note on this README: This document was drafted with AI assistance and reviewed by a human. All project code was written entirely by hand.*

---