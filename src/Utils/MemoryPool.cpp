#include "Utils/MemoryPool.h"
#include "Utils/Logger.h"

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::Chunk::Chunk()
    : freeCount(BlocksPerChunk), next(nullptr)
{
    Logger::Trace("[MemoryPool::Chunk::Chunk] Entry: BlockSize=%zu, BlocksPerChunk=%zu", BlockSize, BlocksPerChunk);
    // Initialize freeList to each block within data
    for (std::size_t i = 0; i < BlocksPerChunk; ++i) {
        freeList[i] = data + i * BlockSize;
        Logger::Trace("[MemoryPool::Chunk::Chunk] Initialized freeList[%zu] = %p", i, (void*)freeList[i]);
    }
    Logger::Debug("[MemoryPool::Chunk::Chunk] Chunk initialized with %zu blocks, each %zu bytes", BlocksPerChunk, BlockSize);
    Logger::Trace("[MemoryPool::Chunk::Chunk] Exit: chunk construction complete");
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::MemoryPool()
    : m_chunks(nullptr), m_globalFree(nullptr), m_totalBlocks(0)
{
    Logger::Trace("[MemoryPool::MemoryPool] Entry: constructor called, BlockSize=%zu, BlocksPerChunk=%zu", BlockSize, BlocksPerChunk);
    Logger::Info("[MemoryPool::MemoryPool] Memory pool created with BlockSize=%zu, BlocksPerChunk=%zu", BlockSize, BlocksPerChunk);
    Logger::Trace("[MemoryPool::MemoryPool] Exit: m_chunks=nullptr, m_globalFree=nullptr, m_totalBlocks=0");
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
MemoryPool<BlockSize, BlocksPerChunk>::~MemoryPool() {
    Logger::Trace("[MemoryPool::~MemoryPool] Entry: destructor called, m_totalBlocks=%zu", m_totalBlocks);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[MemoryPool::~MemoryPool] Mutex acquired, freeing all chunks");
    // Free all chunks
    Chunk* c = m_chunks;
    std::size_t chunkCount = 0;
    while (c) {
        Chunk* next = c->next;
        Logger::Trace("[MemoryPool::~MemoryPool] Deleting chunk %zu at %p", chunkCount, (void*)c);
        delete c;
        c = next;
        chunkCount++;
    }
    m_chunks = nullptr;
    m_globalFree = nullptr;
    Logger::Debug("[MemoryPool::~MemoryPool] Freed %zu chunks, total blocks was %zu", chunkCount, m_totalBlocks);
    m_totalBlocks = 0;
    Logger::Info("[MemoryPool::~MemoryPool] Memory pool destroyed, all %zu chunks freed", chunkCount);
    Logger::Trace("[MemoryPool::~MemoryPool] Exit: destructor complete");
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void* MemoryPool<BlockSize, BlocksPerChunk>::Allocate() {
    Logger::Trace("[MemoryPool::Allocate] Entry: BlockSize=%zu", BlockSize);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_globalFree) {
        Logger::Debug("[MemoryPool::Allocate] Free list is empty, allocating new chunk");
        AddChunk();
    } else {
        Logger::Debug("[MemoryPool::Allocate] Free list has available blocks, m_globalFree=%p", m_globalFree);
    }
    assert(m_globalFree && "Allocation failed: no free blocks");
    // Hardening: assert() is compiled out in release builds. If the free list is still
    // empty here (e.g. AddChunk could not grow the pool), bail out non-fatally instead of
    // dereferencing a null block pointer below.
    if (!m_globalFree) {
        Logger::Error("[MemoryPool::Allocate] No free blocks available after AddChunk; returning nullptr");
        Logger::Trace("[MemoryPool::Allocate] Exit: returning nullptr (allocation failed)");
        return nullptr;
    }
    // Pop one block
    void* block = m_globalFree;
    m_globalFree = *reinterpret_cast<void**>(block);
    Logger::Debug("[MemoryPool::Allocate] Allocated block at %p, next free=%p", block, m_globalFree);
    Logger::Trace("[MemoryPool::Allocate] Exit: returning block=%p", block);
    return block;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::Free(void* ptr) {
    Logger::Trace("[MemoryPool::Free] Entry: ptr=%p", ptr);
    if (!ptr) {
        Logger::Debug("[MemoryPool::Free] Null pointer passed, no-op");
        Logger::Trace("[MemoryPool::Free] Exit: no-op (null ptr)");
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    // Push back onto free list
    *reinterpret_cast<void**>(ptr) = m_globalFree;
    m_globalFree = ptr;
    Logger::Debug("[MemoryPool::Free] Block at %p returned to free list, new head=%p", ptr, m_globalFree);
    Logger::Trace("[MemoryPool::Free] Exit: block freed");
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::PreallocateChunks(std::size_t chunkCount) {
    Logger::Trace("[MemoryPool::PreallocateChunks] Entry: chunkCount=%zu", chunkCount);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[MemoryPool::PreallocateChunks] Preallocating %zu chunks (%zu blocks each, %zu bytes per block)",
                  chunkCount, BlocksPerChunk, BlockSize);
    for (std::size_t i = 0; i < chunkCount; ++i) {
        Logger::Trace("[MemoryPool::PreallocateChunks] Adding chunk %zu of %zu", i + 1, chunkCount);
        AddChunk();
    }
    Logger::Info("[MemoryPool::PreallocateChunks] Preallocated %zu chunks, total blocks now=%zu",
                 chunkCount, m_totalBlocks);
    Logger::Trace("[MemoryPool::PreallocateChunks] Exit: preallocation complete");
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
std::size_t MemoryPool<BlockSize, BlocksPerChunk>::TotalBlocks() const {
    Logger::Trace("[MemoryPool::TotalBlocks] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[MemoryPool::TotalBlocks] m_totalBlocks=%zu", m_totalBlocks);
    Logger::Trace("[MemoryPool::TotalBlocks] Exit: returning %zu", m_totalBlocks);
    return m_totalBlocks;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
std::size_t MemoryPool<BlockSize, BlocksPerChunk>::FreeBlocks() const {
    Logger::Trace("[MemoryPool::FreeBlocks] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    // Traverse free list
    void* p = m_globalFree;
    while (p) {
        ++count;
        p = *reinterpret_cast<void**>(p);
    }
    Logger::Debug("[MemoryPool::FreeBlocks] Traversed free list, found %zu free blocks out of %zu total", count, m_totalBlocks);
    Logger::Trace("[MemoryPool::FreeBlocks] Exit: returning %zu", count);
    return count;
}

template <std::size_t BlockSize, std::size_t BlocksPerChunk>
void MemoryPool<BlockSize, BlocksPerChunk>::AddChunk() {
    Logger::Trace("[MemoryPool::AddChunk] Entry: current m_totalBlocks=%zu", m_totalBlocks);
    // Allocate new chunk
    Chunk* c = new Chunk();
    Logger::Debug("[MemoryPool::AddChunk] New chunk allocated at %p", (void*)c);
    // Link into chunk list
    c->next = m_chunks;
    m_chunks = c;
    Logger::Debug("[MemoryPool::AddChunk] Chunk linked into chain, head=%p", (void*)m_chunks);
    // Push all its blocks onto global free list
    for (std::size_t i = 0; i < BlocksPerChunk; ++i) {
        void* block = c->freeList[i];
        *reinterpret_cast<void**>(block) = m_globalFree;
        m_globalFree = block;
        Logger::Trace("[MemoryPool::AddChunk] Pushed block %zu at %p onto free list", i, block);
    }
    m_totalBlocks += BlocksPerChunk;
    Logger::Info("[MemoryPool::AddChunk] Chunk added: %zu new blocks, total blocks now=%zu", BlocksPerChunk, m_totalBlocks);
    Logger::Trace("[MemoryPool::AddChunk] Exit: chunk addition complete");
}

// Explicit template instantiation if needed (example for 64-byte blocks):
// template class MemoryPool<64, 1024>;
