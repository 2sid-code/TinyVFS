#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>



// ============================================================================
// File System Geometry & Configurations
// ============================================================================
#define MAX_CLUSTERS 65536  // Maximum number of clusters allowed on the virtual disk
#define CLUSTER_SIZE 65536  // Size of each cluster in bytes (64 KB)
#define PAGE_SIZE 256       // Size of each page in bytes (256 pages per cluster)

// Status codes for file system and disk I/O operations
typedef enum {
    fs_success = 0,
    fs_err_invalid_cluster_amount, // Requested disk size is out of bounds
    fs_err_open_failed,            // Could not open the host file
    fs_err_write_failed,           // Disk write operation failed or incomplete
    fs_out_of_capacity,            // Not enough space on disk
    fs_unknown                     // Catch-all for generic or unhandled errors
} FileStatus;

// Status codes for File Allocation Table (FAT) manipulations
typedef enum {
    ts_success = 0,
    ts_out_of_capacity,            // Table is full, cannot add more entries
    ts_not_found,                  // Target entry name does not exist
    ts_unknown                     // Catch-all for allocation table errors
} TableStatus;

// Status codes for Bitmap operations
typedef enum {
    bm_success = 0,
    bm_out_of_capacity,
    bm_not_found,
    bm_out_of_bounds,
    bm_unknown
} BitmapStatus;


// ============================================================================
// Core Data Structures
// ============================================================================

// Represents a precise hardware-like storage coordinate
// __attribute__((__packed__)) ensures no memory padding is added by the compiler
typedef struct __attribute__((__packed__)) {
    uint16_t clusterAddr;          // Cluster index location
    uint8_t pageAddr;              // Offset page within that cluster
} Address;

// Directory/Allocation Table Entry metadata structure
typedef struct __attribute__((__packed__)) {
    char name[27];                 // Null-terminated entry name (Max 26 chars + \0)
    Address address;               // Physical storage mapping for this entry
    uint16_t pagesOccupied;        // Number of pages this file consumes
} TableEntry;                      // Total struct size = 32 bytes (27 + 2 + 1 + 2)

// Bit position information for bitmap calculations
typedef struct {
    uint32_t byte;                 // Which byte in the bitmap array
    uint8_t bit;                   // Which specific bit (0-7) in that byte
} BitPosition;

// ============================================================================
// Forward Declarations
// ============================================================================
BitmapStatus markBitUsed(int bitNum);
BitmapStatus markBitUnused(int bitNum);
BitmapStatus freeAllocation(Address startAddress, uint16_t lengthPages);

// ============================================================================
// Global RAM Buffers (Mirrors of Virtual Disk)
// ============================================================================
uint8_t loadedCluster[CLUSTER_SIZE];                                // Buffer for general data I/O
uint8_t loadedBitmap[CLUSTER_SIZE];                                 // Holds Cluster 1 (Allocation Bitmap)
TableEntry loadedTable[CLUSTER_SIZE / sizeof(TableEntry)];          // Holds Cluster 0 (Root Directory)

// ============================================================================
// Low-Level Disk Operations
// ============================================================================

/**
 * Creates a raw virtual disk file filled with zeroes.
 * @param path          File path on the host system.
 * @param clusterAmount Total number of 64KB blocks to allocate.
 * @return FileStatus   Status of the creation process.
 */
FileStatus createRawDisk(const char* path, int clusterAmount) {
    if (clusterAmount <= 0 || clusterAmount > MAX_CLUSTERS) {
        return fs_err_invalid_cluster_amount;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) return fs_err_open_failed;

    uint8_t *buffer = calloc(1, CLUSTER_SIZE);
    if (!buffer) {
        fclose(fp);
        return fs_unknown;
    }

    // Sequentially write empty clusters to pad out the disk size
    for (int i = 0; i < clusterAmount; i++) {
        if (fwrite(buffer, 1, CLUSTER_SIZE, fp) != CLUSTER_SIZE) {
            free(buffer);
            fclose(fp);
            return fs_err_write_failed;
        }
    }

    free(buffer);
    fclose(fp);
    return fs_success;
}

/**
 * Reads a specific 64KB cluster from the disk file into RAM.
 */
FileStatus loadChunk(const char* path, int chunkIndex, uint8_t* outBuffer) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return fs_err_open_failed;

    // Use int64_t to prevent overflow on 32-bit systems when disks exceed ~2GB
    int64_t offset = (int64_t)chunkIndex * CLUSTER_SIZE;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return fs_unknown;
    }

    size_t bytesRead = fread(outBuffer, 1, CLUSTER_SIZE, fp);
    fclose(fp);

    if (bytesRead != CLUSTER_SIZE) {
        return fs_err_write_failed;
    }

    return fs_success;
}

/**
 * Overwrites a specific 64KB cluster on the disk file from RAM.
 */
FileStatus writeChunk(const char* path, int chunkIndex, uint8_t* inBuffer) {
    FILE *fp = fopen(path, "r+b"); // "r+b" prevents file truncation
    if (!fp) return fs_err_open_failed;

    int64_t offset = (int64_t)chunkIndex * CLUSTER_SIZE;
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return fs_unknown;
    }

    size_t bytesWritten = fwrite(inBuffer, 1, CLUSTER_SIZE, fp);
    fclose(fp);

    if (bytesWritten != CLUSTER_SIZE) {
        return fs_err_write_failed;
    }
    return fs_success;
}

// ============================================================================
// System Structure Syncing Wrappers
// ============================================================================

FileStatus readTable(const char* path) {
    return loadChunk(path, 0, (uint8_t*)loadedTable);
}

FileStatus syncTable(const char* path) {
    return writeChunk(path, 0, (uint8_t*)loadedTable);
}

FileStatus syncBitmap(const char* path) {
    return writeChunk(path, 1, (uint8_t*)loadedBitmap);
}

// ============================================================================
// File Table / Directory Operations
// ============================================================================

/**
 * Adds an entry metadata record into the first available empty slot.
 */
TableStatus addTableEntry(TableEntry* inBuffer, TableEntry* entry) {
    int emptyIndex = 0;
    int foundIndex = 0;

    // Scan for an unused slot (name begins with null terminator)
    for (int i = 0; i < CLUSTER_SIZE / sizeof(TableEntry); i++) {
        if (inBuffer[i].name[0] == '\0') {
            emptyIndex = i;
            foundIndex = 1;
            break;
        }
    }

    if (!foundIndex) {
        return ts_out_of_capacity;
    }

    inBuffer[emptyIndex] = *entry;
    return ts_success;
}

/**
 * Removes an entry from the table buffer by matching its name.
 * Reclaims the allocated pages in the bitmap to prevent memory leaks.
 */
TableStatus removeTableEntry(TableEntry* inBuffer, const char* name) {
    int fileIndex = -1;

    // Scan for the target file
    for (int i = 0; i < CLUSTER_SIZE / sizeof(TableEntry); i++) {
        // strncmp ensures we never read past the 27-byte boundary
        if (strncmp(inBuffer[i].name, name, 27) == 0) {
            fileIndex = i;
            break;
        }
    }

    if (fileIndex == -1) {
        return ts_not_found;
    }

    // Reference the found entry
    TableEntry* targetFile = &inBuffer[fileIndex];

    freeAllocation(targetFile->address, targetFile->pagesOccupied);

    // Clear the directory table metadata
    targetFile->name[0] = '\0';
    targetFile->pagesOccupied = 0;
    targetFile->address.clusterAddr = 0xFFFF; // Reset to an invalid cluster
    targetFile->address.pageAddr = 0xFF;      // Reset to an invalid page

    return ts_success;
}

// ============================================================================
// Bitmap / Memory Allocation Operations
// ============================================================================

/**
 * Sets or clears a specific bit in a given byte.
 */
void setBit(uint8_t *byte, int pos, int value) {
    if (byte == NULL || pos < 0 || pos > 7) return;

    if (value == 1) {
        *byte |= (1 << pos);  // Force bit to 1
    } else {
        *byte &= ~(1 << pos); // Force bit to 0
    }
}

/**
 * Reads the state of a specific bit in a given byte (Returns 1 or 0).
 */
int getBit(const uint8_t *byte, int pos) {
    if (byte == NULL || pos < 0 || pos > 7) return -1;

    return (*byte >> pos) & 1;
}

/**
 * Converts a raw page index into a byte/bit coordinate for the bitmap array.
 */
BitPosition getBitAddr(int bitNum) {
    BitPosition pos;
    pos.byte = bitNum / 8;
    pos.bit = bitNum % 8;
    return pos;
}

/**
 * Marks a specific page index as USED (1).
 */
BitmapStatus markBitUsed(int bitNum) {
    // Prevent out-of-bounds array access (array is size CLUSTER_SIZE)
    if (bitNum < 0 || bitNum >= CLUSTER_SIZE * 8) {
        return bm_out_of_bounds;
    }

    BitPosition pos = getBitAddr(bitNum);
    setBit(&loadedBitmap[pos.byte], pos.bit, 1);
    return bm_success;
}

/**
 * Marks a specific page index as UNUSED (0).
 */
BitmapStatus markBitUnused(int bitNum) {
    if (bitNum < 0 || bitNum >= CLUSTER_SIZE * 8) {
        return bm_out_of_bounds;
    }

    BitPosition pos = getBitAddr(bitNum);
    setBit(&loadedBitmap[pos.byte], pos.bit, 0);
    return bm_success;
}

/**
 * Allocates a contiguous sequence of free pages using the loaded bitmap.
 * @param sizePages     Number of contiguous pages requested.
 * @param totalClusters Total capacity of the disk (prevents scanning beyond physical limits).
 * @return Address      The mapped cluster/page coordinates.
 */
Address falloc(uint16_t sizePages, int totalClusters) {
    Address allocatedAddr;

    // Total physical pages available on the initialized disk
    uint32_t totalUsablePages = totalClusters * 256;

    if (sizePages == 0 || sizePages > totalUsablePages) {
        allocatedAddr.clusterAddr = 0xFFFF;
        allocatedAddr.pageAddr = 0xFF;
        return allocatedAddr;
    }

    uint16_t contiguousPagesFound = 0;
    uint32_t startPageIndex = 0;

    // Skip Cluster 0 (Table) and Cluster 1 (Bitmap) (2 clusters = 512 pages = 64 bytes)
    const int RESERVED_BITMAP_BYTES = 64;

    // Ensure we don't scan beyond the physical disk limits OR our RAM buffer size
    int maxBitmapBytesToCheck = (totalUsablePages / 8) < CLUSTER_SIZE ? (totalUsablePages / 8) : CLUSTER_SIZE;

    // Linear scan through bitmap bytes
    for (int i = RESERVED_BITMAP_BYTES; i < maxBitmapBytesToCheck; i++) {

        // Optimization: Skip byte entirely if all 8 bits are used
        if (loadedBitmap[i] == 0xFF) {
            contiguousPagesFound = 0;
            continue;
        }

        // Deep bit scanning within the byte
        for (int j = 0; j < 8; j++) {
            if (getBit(&loadedBitmap[i], j) == 1) {
                // Sequence broken: reset contiguous tracking
                contiguousPagesFound = 0;
            } else {
                // Sequence started: mark coordinate of initial block
                if (contiguousPagesFound == 0) {
                    startPageIndex = (i * 8) + j;
                }

                contiguousPagesFound++;

                // Successful allocation criteria met
                if (contiguousPagesFound == sizePages) {

                    // Mark every newly allocated bit as USED in the bitmap memory block
                    for (uint32_t k = startPageIndex; k < startPageIndex + sizePages; k++) {
                        markBitUsed(k);
                    }

                    // Map linear page index back to hardware coordinate structure
                    allocatedAddr.clusterAddr = (uint16_t)(startPageIndex / 256);
                    allocatedAddr.pageAddr    = (uint8_t)(startPageIndex % 256);

                    return allocatedAddr;
                }
            }
        }
    }

    // Allocation failure fallback
    allocatedAddr.clusterAddr = 0xFFFF;
    allocatedAddr.pageAddr = 0xFF;
    return allocatedAddr;
}


/**
 * Frees a contiguous block of allocated pages in the bitmap.
 * @param startAddress The hardware coordinate (cluster/page) where the allocation begins.
 * @param lengthPages  The number of contiguous pages (bits) to clear.
 * @return BitmapStatus Status of the operation.
 */
BitmapStatus freeAllocation(Address startAddress, uint16_t lengthPages) {
    // 1. Prevent operations on the failure/invalid address (0xFFFF)
    if (startAddress.clusterAddr == 0xFFFF) {
        return bm_out_of_bounds;
    }

    // 2. Convert the hardware coordinate back into a linear page index
    // Since there are 256 pages per cluster, we multiply the cluster index by 256
    uint32_t startPageIndex = (startAddress.clusterAddr * 256) + startAddress.pageAddr;

    // 3. Loop through and free each allocated page in the loaded bitmap
    for (uint32_t i = 0; i < lengthPages; i++) {
        BitmapStatus status = markBitUnused(startPageIndex + i);

        // If we accidentally try to clear a bit out of bounds, halt and return the error
        if (status != bm_success) {
            return status;
        }
    }

    return bm_success;
}

// ============================================================================
// File Read/Write Operations
// ============================================================================

FileStatus createFile(char* path, char* filename, uint8_t *data, uint16_t size, int totalClusters) {

    if (size == 0) return fs_success;

    // Calculate pages needed for file
    uint16_t pagesNeeded = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Allocate space on virtual disk
    Address fileAddr = falloc(pagesNeeded, totalClusters);
    if (fileAddr.clusterAddr == 0xFFFF) return fs_out_of_capacity;

    // Create Table Entry
    TableEntry newFile;
    memset(newFile.name, 0, sizeof(newFile.name)); // Clear garbage
    strncpy(newFile.name, filename, 26);
    newFile.address = fileAddr;
    newFile.pagesOccupied = pagesNeeded;

    if (addTableEntry(loadedTable, &newFile) != ts_success) {
        freeAllocation(fileAddr, pagesNeeded);
        return fs_unknown;
    }

    // Calculate absolute byte offset in the host file
    int64_t absoluteOffset = ((int64_t)fileAddr.clusterAddr * CLUSTER_SIZE) +
                             (fileAddr.pageAddr * PAGE_SIZE);

    // Open host file and write the raw data
    FILE *fp = fopen(path, "r+b");
    if (!fp) return fs_err_open_failed;

    fseek(fp, absoluteOffset, SEEK_SET);
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);

    if (written != size) return fs_err_write_failed;

    // Save updated metadata to disk
    syncTable(path);
    syncBitmap(path);

    return fs_success;

}

/**
 * Reads a file from the virtual disk into a provided buffer.
 * Note: outBuffer must be large enough to hold (pagesOccupied * 256) bytes.
 */
FileStatus readFile(char* path, char* filename, uint8_t* outBuffer, size_t* bytesRead) {
    TableEntry* target = NULL;

    // Find the file in the loaded directory table
    for (int i = 0; i < CLUSTER_SIZE / sizeof(TableEntry); i++) {
        if (strncmp(loadedTable[i].name, filename, 27) == 0) {
            target = &loadedTable[i];
            break;
        }
    }

    if (target == NULL) return fs_unknown; // File not found

    // Calculate absolute byte offset
    int64_t absoluteOffset = ((int64_t)target->address.clusterAddr * CLUSTER_SIZE) +
                             (target->address.pageAddr * PAGE_SIZE);

    // Read full pages based on what was allocated
    size_t bytesToRead = target->pagesOccupied * PAGE_SIZE;

    // Open host file and read
    FILE *fp = fopen(path, "rb");
    if (!fp) return fs_err_open_failed;

    fseek(fp, absoluteOffset, SEEK_SET);
    *bytesRead = fread(outBuffer, 1, bytesToRead, fp);
    fclose(fp);

    return fs_success;
}

// Helper to get required buffer size for exporting to Python
int getFileSize(char* path, char* filename) {
    readTable(path);
    for (int i = 0; i < CLUSTER_SIZE / sizeof(TableEntry); i++) {
        if (strncmp(loadedTable[i].name, filename, 27) == 0) {
            return loadedTable[i].pagesOccupied * PAGE_SIZE;
        }
    }
    return -1; // File not found
}

// High-level delete wrapper
FileStatus deleteFile(char* path, char* filename) {
    readTable(path);
    loadChunk(path, 1, loadedBitmap);
    
    if (removeTableEntry(loadedTable, filename) != ts_success) {
        return fs_unknown;
    }
    
    syncTable(path);
    syncBitmap(path);
    return fs_success;
}