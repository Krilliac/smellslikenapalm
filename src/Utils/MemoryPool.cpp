#include "Utils/MemoryPool.h"

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::Chunk::Chunk()
    : freeCount(BlocksPerChunk), next(nullptr)
{
    // Initialize freeList to each block within data
    for (std::size_t i = 0; i < BlocksPerChunk; ++i) {
        freeList[i] = data + i * BlockSize;
    }
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::MemoryPool()
    : m_chunks(nullptr), m_globalFree(nullptr), m_totalBlocks(0)
{}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::~MemoryPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Free all chunks
    Chunk* c = m_chunks;
    while (c) {
        Chunk* next = c->next;
        delete c;
        c = next;
    }
    m_chunks = nullptr;
    m_globalFree = nullptr;
    m_totalBlocks = 0;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void* MemoryPool<BlockSize, BlocksPerChunk>::Allocate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_globalFree) {
        AddChunk();
    }
    assert(m_globalFree && "Allocation failed: no free blocks");
    // Pop one block
    void* block = m_globalFree;
    m_globalFree = *reinterpret_cast<void**>(block);
    return block;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::Free(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    // Push back onto free list
    *reinterpret_cast<void**>(ptr) = m_globalFree;
    m_globalFree = ptr;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::PreallocateChunks(std::size_t chunkCount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t i = 0; i < chunkCount; ++i) {
        AddChunk();
    }
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
std::size_t MemoryPool<BlockSize, BlocksPerChunk>::TotalBlocks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalBlocks;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
std::size_t MemoryPool<BlockSize, BlocksPerChunk>::FreeBlocks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    // Traverse free list
    void* p = m_globalFree;
    while (p) {
        ++count;
        p = *reinterpret_cast<void**>(p);
    }
    return count;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::AddChunk() {
    // Allocate new chunk
    Chunk* c = new Chunk();
    // Link into chunk list
    c->next = m_chunks;
    m_chunks = c;
    // Push all its blocks onto global free list
    for (std::size_t i = 0; i < BlocksPerChunk; ++i) {
        void* block = c->freeList[i];
        *reinterpret_cast<void**>(block) = m_globalFree;
        m_globalFree = block;
    }
    m_totalBlocks += BlocksPerChunk;
}

// Explicit template instantiation if needed (example for 64‐byte blocks):
// template class MemoryPool<64, 1024>;