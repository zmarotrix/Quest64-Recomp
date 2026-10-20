#ifndef __Q64_DEVPANEL_GAME_MEMORY_H__
#define __Q64_DEVPANEL_GAME_MEMORY_H__

#include <cstdint>
#include <cstring>

#include "librecomp/addresses.hpp"

// Reads and writes N64 virtual addresses (0x80000000+) in recompiled RDRAM.
// RDRAM is stored as native-endian 32-bit words, so 16-bit and 8-bit accesses flip the low
// address bits, matching the MEM_H/MEM_B macros in recomp.h.
// These run on the dev panel's thread while the game runs. That's fine for tweaking values,
// but multi-field edits aren't atomic from the game's point of view.
namespace q64::devpanel {
    class GameMemory {
    public:
        void set_rdram(uint8_t* rdram) { rdram_ = rdram; }
        bool ready() const { return rdram_ != nullptr; }

        static bool valid(uint32_t addr, uint32_t size = 1) {
            return addr >= 0x80000000u && (uint64_t)(addr - 0x80000000u) + size <= recomp::mem_size;
        }

        uint8_t read_u8(uint32_t addr) const { return *(uint8_t*)(rdram_ + ((addr ^ 3) - 0x80000000u)); }
        uint16_t read_u16(uint32_t addr) const { return *(uint16_t*)(rdram_ + ((addr ^ 2) - 0x80000000u)); }
        uint32_t read_u32(uint32_t addr) const { return *(uint32_t*)(rdram_ + (addr - 0x80000000u)); }
        float read_f32(uint32_t addr) const {
            uint32_t bits = read_u32(addr);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        void write_u8(uint32_t addr, uint8_t value) { *(uint8_t*)(rdram_ + ((addr ^ 3) - 0x80000000u)) = value; }
        void write_u16(uint32_t addr, uint16_t value) { *(uint16_t*)(rdram_ + ((addr ^ 2) - 0x80000000u)) = value; }
        void write_u32(uint32_t addr, uint32_t value) { *(uint32_t*)(rdram_ + (addr - 0x80000000u)) = value; }
        void write_f32(uint32_t addr, float value) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            write_u32(addr, bits);
        }

    private:
        uint8_t* rdram_ = nullptr;
    };
}

#endif
