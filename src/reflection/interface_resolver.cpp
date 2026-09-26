#include "reflection/interface_resolver.hpp"

#include "module_registry.hpp"
#include "pe_image.hpp"
#include <Psapi.h>
#include <cstring>
#include <limits>
#include <utility>

namespace izanagi::interfaces {
namespace {

bool executable(const void* ptr) noexcept
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (mbi.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool inside(const void* ptr, std::uintptr_t base, std::size_t size) noexcept
{
    const auto p = reinterpret_cast<std::uintptr_t>(ptr);
    return p >= base && p - base < size;
}

}

Lease::Lease(HMODULE module, void* pointer, std::uintptr_t base,
             std::size_t image_size) noexcept
    : module_(module), pointer_(pointer), base_(base), image_size_(image_size) {}

Lease::~Lease() noexcept { if (module_) FreeLibrary(module_); }

Lease::Lease(Lease&& other) noexcept
    : module_(std::exchange(other.module_, nullptr)),
      pointer_(std::exchange(other.pointer_, nullptr)),
      base_(std::exchange(other.base_, 0)),
      image_size_(std::exchange(other.image_size_, 0)) {}

Lease& Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        if (module_) FreeLibrary(module_);
        module_ = std::exchange(other.module_, nullptr);
        pointer_ = std::exchange(other.pointer_, nullptr);
        base_ = std::exchange(other.base_, 0);
        image_size_ = std::exchange(other.image_size_, 0);
    }
    return *this;
}

Lease Resolve(std::wstring_view module, std::string_view interface_name) noexcept
{
    if (interface_name.empty() || interface_name.size() >= 128 ||
        interface_name.find('\0') != std::string_view::npos) return {};
    const auto cached = modules::Find(module);
    if (!cached) return {};
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(cached->handle), &pinned)) return {};
    const auto release = [&]() noexcept { FreeLibrary(pinned); };
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), pinned, &info, sizeof(info)) ||
        reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll) != cached->base) {
        release();
        return {};
    }
    const auto pe = pe::InspectImage(info.lpBaseOfDll, info.SizeOfImage);
    if (!pe || pe->image_size != cached->image_size || pe->timestamp != cached->timestamp) {
        release();
        return {};
    }
    char name[128]{};
    std::memcpy(name, interface_name.data(), interface_name.size());
    const auto export_ptr = GetProcAddress(pinned, "CreateInterface");
    if (!export_ptr || !inside(reinterpret_cast<const void*>(export_ptr), pe->base,
                               pe->image_size) ||
        !executable(reinterpret_cast<const void*>(export_ptr))) {
        release();
        return {};
    }
    using factory_fn = void* (__cdecl*)(const char*, int*);
    void* result = nullptr;
    try {
        int status = -1;
        result = reinterpret_cast<factory_fn>(export_ptr)(name, &status);
        if (status != 0 || !result ||
            (reinterpret_cast<std::uintptr_t>(result) & (alignof(void*) - 1)) != 0) {
            release();
            return {};
        }
    } catch (...) {
        release();
        return {};
    }
    return Lease(pinned, result, pe->base, pe->image_size);
}

}
