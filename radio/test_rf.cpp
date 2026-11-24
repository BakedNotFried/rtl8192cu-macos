// Offline tests for RF programming and its readback checks. No USB transport.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <stdexcept>

#define __packed __attribute__((__packed__))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define IEEE80211_ADDR_LEN 6
#include "upstream/r92creg.h"

struct TestRadio {
    std::array<uint8_t, 65536> mac{};
    uint32_t rf[2][64]{};
    bool read_pending = false;
    int bad_readback_chain = -1;
    bool fail_io = false;
    unsigned writes = 0;

    uint8_t r8(uint16_t reg) const { return mac.at(reg); }
    uint16_t r16(uint16_t reg) const { return r8(reg) | uint16_t(r8(reg + 1)) << 8; }
    uint32_t r32(uint16_t reg) const {
        if (fail_io)
            throw std::runtime_error("Simulated USB transfer failure");
        return r16(reg) | uint32_t(r16(reg + 2)) << 16;
    }
    void w8(uint16_t reg, uint8_t value) { mac.at(reg) = value; ++writes; }
    void w16(uint16_t reg, uint16_t value) {
        w8(reg, value & 0xff);
        w8(reg + 1, value >> 8);
    }
    void store32(uint16_t reg, uint32_t value) {
        w16(reg, value & 0xffff);
        w16(reg + 2, value >> 16);
    }
    void w32(uint16_t reg, uint32_t value) {
        store32(reg, value);
        if (reg == R92C_HSSI_PARAM2(0) && (value & R92C_HSSI_PARAM2_READ_EDGE) == 0)
            read_pending = true;
        for (int chain = 0; chain < 2; ++chain) {
            if (reg == R92C_LSSI_PARAM(chain)) {
                const auto address = MS(value, R92C_LSSI_PARAM_ADDR);
                rf[chain][address] = MS(value, R92C_LSSI_PARAM_DATA);
            }
            if (reg == R92C_HSSI_PARAM2(chain) && read_pending &&
                (value & R92C_HSSI_PARAM2_READ_EDGE)) {
                const unsigned address = MS(value, R92C_HSSI_PARAM2_READ_ADDR);
                uint32_t data = address < 64 ? rf[chain][address] : 0;
                if (bad_readback_chain == chain)
                    data = 0xfffff;
                store32(R92C_HSPI_READBACK(chain), data);
                store32(R92C_LSSI_READBACK(chain), data);
                read_pending = false;
            }
        }
    }
    void delay_us(unsigned) {}
#include "rf.inc"
};

template <typename F> static bool rejected(F action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int main() {
    for (const uint8_t board : {R92C_BOARD_TYPE_DONGLE, R92C_BOARD_TYPE_MINICARD}) {
        TestRadio radio;
        assert(rejected([&] { radio.tune(1); }));
        assert(rejected([&] { radio.baseband_and_rf(board, 0); }));
        assert(radio.writes == 0);
        radio.w8(R92C_TXPAUSE, R92C_TXPAUSE_ALL);
        assert(rejected([&] { radio.baseband_and_rf(7, 0); }));
        radio.w16(R92C_CR, R92C_CR_MACTXEN);
        assert(rejected([&] { radio.baseband_and_rf(board, 0); }));
        radio.w16(R92C_CR, 0);
        radio.baseband_and_rf(board, 0);
        assert(radio.r8(R92C_TXPAUSE) == R92C_TXPAUSE_ALL);
        assert((radio.r16(R92C_CR) & R92C_CR_MACTXEN) == 0);
        assert((radio.r8(R92C_SPS0_CTRL) & 0x09) == 0x09);
        assert((radio.r32(R92C_FPGA0_RFPARAM(0)) & 0x00380038) == 0x00080008);
        assert((radio.r32(R92C_FPGA0_RFMOD) & R92C_RFMOD_JAPAN) == 0);
        assert(radio.r32(R92E_RX_WAIT_CCA) == 0x63db25a0);
        for (const auto& chain : radio.rf)
            assert(chain[R92C_RF_AC] == 0x32d95);
        const auto initialized_writes = radio.writes;
        for (unsigned invalid : {0u, 14u, 250u})
            assert(rejected([&] { radio.tune(invalid); }));
        assert(radio.writes == initialized_writes);
        for (unsigned channel : {1u, 13u, 6u, 11u}) {
            radio.tune(channel);
            for (const auto& chain : radio.rf) {
                assert((chain[R92C_RF_CHNLBW] & 0xfff) == (R92C_RF_CHNLBW_BW20 | channel));
                assert((chain[R92C_RF_CHNLBW] & R92C_RF_CHNLBW_LCSTART) == 0);
            }
        }
        for (int bad_chain : {0, 1}) {
            radio.bad_readback_chain = bad_chain;
            radio.tune(1); // A missing echo is diagnostic during a passive listen.
        }
        radio.fail_io = true;
        assert(rejected([&] { radio.tune(1); }));
    }
}
