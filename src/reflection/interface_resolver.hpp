#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace izanagi::interfaces {

// possui uma referência ao módulo; o resultado da fábrica pertence à engine.
class Lease final {
public:
    Lease() noexcept = default;
    Lease(HMODULE module, void* pointer, std::uintptr_t base,
          std::size_t image_size) noexcept;
    ~Lease() noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    explicit operator bool() const noexcept { return module_ && pointer_; }
    void* get() const noexcept { return pointer_; }
    std::uintptr_t base() const noexcept { return base_; }
    std::size_t image_size() const noexcept { return image_size_; }

private:
    HMODULE module_{};
    void* pointer_{};
    std::uintptr_t base_{};
    std::size_t image_size_{};
};

Lease Resolve(std::wstring_view module, std::string_view interface_name) noexcept;

}
