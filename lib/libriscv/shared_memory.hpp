#pragma once
#include <span>
#include <vector>

namespace riscv
{
    struct SharedMem
    {
        bool is_duplicated() const noexcept {
            return m_begin == nullptr;
        }

    private:
        std::vector<uint8_t> m_copied_data;
        uint8_t* m_begin = nullptr;
        uint8_t* m_end   = nullptr;
    };
}
