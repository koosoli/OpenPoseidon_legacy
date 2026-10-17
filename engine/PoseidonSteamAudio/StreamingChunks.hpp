#pragma once

// Chunked decode state for streamed (OGG) waves.  The streaming pump thread
// decodes ~100 ms slices of PCM into a queue; the mixer thread consumes them.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace Poseidon
{
namespace Audio
{
// Steam Audio's own streaming types. The OpenAL backend defines Poseidon::Audio::DecodedChunk,
// ChunkQueue and StreamingBuffers too, with another layout, and both backends link into the game: one
// definition of each inline member won, so chunks lost their generation and the mixer dropped every one
// as stale -- all streamed (.ogg) music and speech was silent. A namespace of their own keeps them apart.
namespace Steam
{

// One slice of decoded PCM (raw bytes in the stream's native format).
struct DecodedChunk
{
    std::vector<uint8_t> pcm;
    // Stream byte offset where this chunk starts.
    int64_t streamOffset = 0;
    // True for the chunk that ends at EOF on a non-looping stream.
    bool isLastChunk = false;
    // StreamingBuffers::generation when the decode started; the mixer drops
    // chunks from before the last Stop.
    uint32_t generation = 0;
};

// Mutex-protected FIFO: the pump thread pushes, the mixer thread pops.  One
// decode in flight per wave keeps it single-producer.
class ChunkQueue
{
  public:
    void Push(DecodedChunk chunk)
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _chunks.emplace_back(std::move(chunk));
    }

    bool TryPop(DecodedChunk& out)
    {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_chunks.empty())
            return false;
        out = std::move(_chunks.front());
        _chunks.pop_front();
        return true;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(_mtx);
        return _chunks.size();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _chunks.clear();
    }

  private:
    mutable std::mutex _mtx;
    std::deque<DecodedChunk> _chunks;
};

// Per-wave streaming state, only used when the wave is streamed.
struct StreamingBuffers
{
    // Chunks kept decoded ahead of the mixer (~400 ms of headroom).
    static constexpr int kNumBuffers = 4;

    // Target chunk duration in milliseconds.
    static constexpr int kChunkMs = 100;

    // PCM byte size of one chunk, always a multiple of the frame size.
    int chunkBytes = 0;

    // Total decoded byte offset into the source stream (pump thread).
    std::atomic<int64_t> decodeOffsetBytes{0};

    // Decoded chunks awaiting the mixer.
    ChunkQueue queue;

    // A decode is in flight (prevents two decodes racing on the codec).
    std::atomic<bool> decodeInFlight{false};

    // The last chunk has been decoded (EOF on a non-looping stream).
    std::atomic<bool> eofReached{false};

    // Teardown signal for an in-flight decode.
    std::atomic<bool> aborted{false};

    // Bumped by Stop (rewind).  A decode that started under an older
    // generation discards its chunk instead of queueing stale audio.
    std::atomic<uint32_t> generation{0};
};

} // namespace Steam
} // namespace Audio
} // namespace Poseidon
