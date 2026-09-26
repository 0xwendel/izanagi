#pragma once

#include <cstddef>
#include <cstdint>

namespace izanagi::entities::detail {

struct ReadStats {
    std::uint64_t queries{};
    std::uint64_t cache_hits{};
    double query_ms{};
};

class ReadBatch final {
public:
    ReadBatch() noexcept;
    ~ReadBatch() noexcept;
    ReadBatch(const ReadBatch&) = delete;
    ReadBatch& operator=(const ReadBatch&) = delete;
private:
    bool previous_{};
};

bool AddAddress(std::uintptr_t base, std::size_t offset,
                std::uintptr_t& result) noexcept;
bool ReadMemory(std::uintptr_t address, void* target, std::size_t size) noexcept;
bool ReadName(std::uintptr_t address, char* target, std::size_t capacity) noexcept;
bool ExecutableAddress(std::uintptr_t address) noexcept;
ReadStats BatchStats() noexcept;

}
