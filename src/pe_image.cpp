#include "pe_image.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace izanagi::pe {
namespace {

bool readable(const std::uintptr_t start, const std::size_t size) noexcept
{
    if (size == 0 || start > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }
    const auto end = start + size;
    auto pos = start;
    while (pos < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(pos), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (mbi.Protect & 0xff) == PAGE_EXECUTE) {
            return false;
        }
        const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (region > pos || mbi.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region) {
            return false;
        }
        const auto next = region + mbi.RegionSize;
        if (next <= pos) {
            return false;
        }
        pos = (std::min)(next, end);
    }
    return true;
}

template <typename T>
bool read_header(const std::uintptr_t base, const std::size_t size,
                 const std::size_t offset, T& out) noexcept
{
    if (offset > size || sizeof(T) > size - offset ||
        base > (std::numeric_limits<std::uintptr_t>::max)() - offset ||
        !readable(base + offset, sizeof(T))) {
        return false;
    }
    std::memcpy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    return true;
}

}

std::optional<Metadata> InspectImage(const void* image, const std::size_t mapped_size) noexcept
{
    if (image == nullptr || mapped_size < sizeof(IMAGE_DOS_HEADER)) {
        return std::nullopt;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - mapped_size) {
        return std::nullopt;
    }
    IMAGE_DOS_HEADER dos{};
    if (!read_header(base, mapped_size, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0) {
        return std::nullopt;
    }
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!read_header(base, mapped_size, nt_offset, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0 ||
        nt.OptionalHeader.SizeOfImage > mapped_size ||
        nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage) {
        return std::nullopt;
    }
    const auto sections_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                                 nt.FileHeader.SizeOfOptionalHeader;
    if (sections_offset < nt_offset || sections_offset > nt.OptionalHeader.SizeOfImage ||
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) >
            (nt.OptionalHeader.SizeOfImage - sections_offset) / sizeof(IMAGE_SECTION_HEADER)) {
        return std::nullopt;
    }
    return Metadata{base, nt.OptionalHeader.SizeOfImage,
                    nt.OptionalHeader.AddressOfEntryPoint,
                    nt.FileHeader.TimeDateStamp, nt.FileHeader.NumberOfSections};
}

}
