#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <cassert>

// A simple fixed‐size block memory pool.
// Templated on block size and blocks per chunk.
// Thread‐safe allocation/free for uniform objects.
template <std::size_t BlockSize, std::size_t BlocksPerChunk = 1024>
class MemoryPool {
public:
    MemoryPool();
    ~MemoryPool();

    // Allocate one block (returns aligned memory of size >= BlockSize).
    void* Allocate();

    // Free a previously allocated block.
    void  Free(void* ptr);

    // Optionally pre‐allocate N chunks.
    void PreallocateChunks(std::size_t chunkCount);

    // Get total number of allocated blocks.
    std::size_t TotalBlocks() const;

    // Get number of free blocks.
    std::size_t FreeBlocks() const;

private:
    struct Chunk {
        uint8_t data[BlockSize * BlocksPerChunk];
        void*   freeList[BlocksPerChunk];
        std::size_t freeCount;
        Chunk*  next;
        Chunk();
    };

    mutable std::mutex    m_mutex;
    Chunk*                m_chunks;       // linked list of all chunks
    void*                 m_globalFree;   // head of free‐block list
    std::size_t           m_totalBlocks;  // total blocks across chunks

    // Allocate a new chunk and push its blocks into free list.
    void AddChunk();

    // Non‐copyable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
};