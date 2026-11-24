/* RTL8192CU receive-only prototype. Portions adapted from OpenBSD rtwn.c and
 * if_urtwn.c. Copyright (c) 2010 Damien Bergamini, 2014 Kevin Lo,
 * 2015 Stefan Sperling, 2016 Nathanial Sloss.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "radio.h"
#include "station_hw.h"
#include <algorithm>
#include <iterator>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <time.h>

#define IEEE80211_ADDR_LEN 6
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "upstream/r92creg.h"
#include "upstream/microcode.h"

struct rtl_radio {
    rtl_radio_io io;
    bool changed = false;
    bool active = false;
    bool fw_ready = false;
    uint8_t board_type = 0;
    char diagnostic[240] = {};
    explicit rtl_radio(rtl_radio_io callbacks) : io(callbacks) {}

    void transfer_error(const char *op, uint16_t reg, int status) {
        char message[96];
        std::snprintf(message, sizeof(message), "USB %s at 0x%04x failed (0x%x)", op, reg, static_cast<unsigned>(status));
        throw std::runtime_error(message);
    }
    uint32_t read_register(uint16_t reg, size_t width) {
        uint8_t data[4] = {};
        int err = io.read(io.ctx, reg, data, width);
        if (err) transfer_error("read", reg, err);
        uint32_t result = 0;
        for (size_t i = 0; i < width; ++i)
            result |= uint32_t(data[i]) << (8 * i);
        return result;
    }
    void write_bytes(uint16_t reg, const uint8_t *data, size_t size) {
        int err = io.write(io.ctx, reg, data, size);
        if (err) transfer_error("write", reg, err);
    }
    uint8_t r8(uint16_t reg) { return static_cast<uint8_t>(read_register(reg, 1)); }
    uint16_t r16(uint16_t reg) { return static_cast<uint16_t>(read_register(reg, 2)); }
    uint32_t r32(uint16_t reg) { return read_register(reg, 4); }
    void w8(uint16_t reg, uint32_t value) {
        uint8_t b = static_cast<uint8_t>(value);
        write_bytes(reg, &b, 1);
    }
    void w16(uint16_t reg, uint32_t value) {
        const uint8_t b[] = {uint8_t(value), uint8_t(value >> 8)};
        write_bytes(reg, b, sizeof(b));
    }
    void w32(uint16_t reg, uint32_t value) {
        const uint8_t b[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
        write_bytes(reg, b, sizeof(b));
    }
    static void delay_us(unsigned usec) {
        timespec ts = {static_cast<time_t>(usec / 1000000), static_cast<long>((usec % 1000000) * 1000)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }

    void firmware_reset() {
        w8(R92C_HMETFR + 3, 0x20);
        for (unsigned i = 0; i < 100; ++i) {
            if (!(r16(R92C_SYS_FUNC_EN) & R92C_SYS_FUNC_EN_CPUEN)) return;
            delay_us(50);
        }
        w16(R92C_SYS_FUNC_EN, r16(R92C_SYS_FUNC_EN) & ~R92C_SYS_FUNC_EN_CPUEN);
    }

#include "power.inc"

    uint8_t efuse_read(unsigned address) {
        if (address >= 512) throw std::runtime_error("malformed board eFuse record");
        // Clearing VALID issues a read. Never issue an eFuse write command.
        uint32_t reg = r32(R92C_EFUSE_CTRL);
        w32(R92C_EFUSE_CTRL, RW(reg, R92C_EFUSE_CTRL_ADDR, address) & ~R92C_EFUSE_CTRL_VALID);
        for (int i = 0; i < 100; ++i) {
            reg = r32(R92C_EFUSE_CTRL);
            if (reg & R92C_EFUSE_CTRL_VALID) return MS(reg, R92C_EFUSE_CTRL_DATA);
            delay_us(5);
        }
        throw std::runtime_error("board eFuse read timed out");
    }

    r92c_rom board_rom() {
        uint16_t isolation = r16(R92C_SYS_ISO_CTRL);
        if (!(isolation & R92C_SYS_ISO_CTRL_PWC_EV12V))
            w16(R92C_SYS_ISO_CTRL, isolation | R92C_SYS_ISO_CTRL_PWC_EV12V);
        uint16_t functions = r16(R92C_SYS_FUNC_EN);
        if (!(functions & R92C_SYS_FUNC_EN_ELDR)) w16(R92C_SYS_FUNC_EN, functions | R92C_SYS_FUNC_EN_ELDR);
        uint16_t clock = r16(R92C_SYS_CLKR);
        constexpr auto efuse_clock = R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M;
        if ((clock & efuse_clock) != efuse_clock) w16(R92C_SYS_CLKR, clock | efuse_clock);
        r92c_rom rom;
        std::memset(&rom, 0xff, sizeof(rom));
        auto *bytes = reinterpret_cast<uint8_t *>(&rom);
        unsigned address = 0;
        while (address < 512) {
            uint8_t entry = efuse_read(address++);
            if (entry == 0xff) break;
            for (unsigned word = 0; word < 4; ++word) {
                if (entry & (1u << word)) continue;
                unsigned offset = (entry >> 4) * 8u + word * 2;
                if (offset + 1 >= sizeof(rom)) throw std::runtime_error("invalid eFuse block");
                bytes[offset] = efuse_read(address++);
                bytes[offset + 1] = efuse_read(address++);
            }
        }
        if (bytes[0] != 0x29 || bytes[1] != 0x81)
            throw std::runtime_error("unexpected board ROM signature");
        return rom;
    }

    void dma_init(unsigned out_count) {
        // Pinned urtwn LLT layout: 248 reserved pages plus a receive ring.
        constexpr unsigned reserved = 248;
        constexpr unsigned boundary = reserved + 1;
        for (unsigned i = 0; i < 256; ++i) {
            unsigned next = i + 1;
            if (i == reserved) next = 255;
            if (i == 255) next = boundary;
            w32(R92C_LLT_INIT, SM(R92C_LLT_INIT_OP, R92C_LLT_INIT_OP_WRITE) |
                SM(R92C_LLT_INIT_ADDR, i) | SM(R92C_LLT_INIT_DATA, next));
            unsigned tries = 0;
            while (MS(r32(R92C_LLT_INIT), R92C_LLT_INIT_OP) != R92C_LLT_INIT_OP_NO_ACTIVE) {
                if (++tries == 20) throw std::runtime_error("DMA list setup timed out");
                delay_us(5);
            }
        }
        unsigned high = 12, normal = out_count >= 2 ? 2 : 0, low = out_count == 3 ? 2 : 0;
        w8(R92C_RQPN_NPQ, normal);
        w32(R92C_RQPN, SM(R92C_RQPN_PUBQ, (reserved - high - low - normal)) |
            SM(R92C_RQPN_HPQ, high) | SM(R92C_RQPN_LPQ, low) | R92C_RQPN_LD);
        for (auto reg : {R92C_TXPKTBUF_BCNQ_BDNY, R92C_TXPKTBUF_MGQ_BDNY,
                R92C_TXPKTBUF_WMAC_LBK_BF_HD, R92C_TRXFF_BNDY, R92C_TDECTRL + 1})
            w8(reg, boundary);
        const uint16_t queue_maps[] = {R92C_TRXDMA_CTRL_QMAP_HQ, R92C_TRXDMA_CTRL_QMAP_HQ_NQ, R92C_TRXDMA_CTRL_QMAP_3EP};
        w16(R92C_TRXDMA_CTRL, (r16(R92C_TRXDMA_CTRL) & ~R92C_TRXDMA_CTRL_QMAP_M) | queue_maps[out_count - 1]);
        w16(R92C_TRXFF_BNDY + 2, 0x2800 - 1);
        w8(R92C_PBP, SM(R92C_PBP_PSRX, R92C_PBP_128) | SM(R92C_PBP_PSTX, R92C_PBP_128));
        w8(R92C_RX_DRVINFO_SZ, 4);
        w32(R92C_HISR, 0xffffffff);
        w32(R92C_HIMR, 0xffffffff);
        // Gen1 USB can aggregate at the DMA and USB stages independently.
        // Use the unaggregated mode of Linux rtl8xxxu_gen1_init_aggregation
        // so one accepted beacon is sufficient for a bulk completion.
        w8(R92C_USB_SPECIAL_OPTION, r8(R92C_USB_SPECIAL_OPTION) & ~R92C_USB_SPECIAL_OPTION_AGG_EN);
        w8(R92C_TRXDMA_CTRL, r8(R92C_TRXDMA_CTRL) & ~R92C_TRXDMA_CTRL_RXDMA_AGG_EN);
    }

    void firmware() {
        constexpr size_t header = sizeof(r92c_fw_hdr);
        static_assert(header == 32);
        size_t length = sizeof(rtl8192cT) - header;
        // Realtek signature 0x88c1 and RAM payload size in the pinned image.
        if (LE_READ_2(rtl8192cT) != 0x88c1 || LE_READ_2(rtl8192cT + 12) != static_cast<int>(length))
            throw std::runtime_error("invalid embedded firmware");
        if (r8(R92C_MCUFWDL) & R92C_MCUFWDL_RAM_DL_SEL) {
            w8(R92C_MCUFWDL, 0);
            firmware_reset();
        }
        w8(R92C_MCUFWDL, r8(R92C_MCUFWDL) | R92C_MCUFWDL_EN);
        w32(R92C_MCUFWDL, r32(R92C_MCUFWDL) & ~R92C_MCUFWDL_ROM_DLEN);
        w8(R92C_MCUFWDL, r8(R92C_MCUFWDL) | R92C_MCUFWDL_CHKSUM_RPT);
        delay_us(50);
        const uint8_t *payload = rtl8192cT + header;
        for (unsigned page = 0; length; ++page) {
            w32(R92C_MCUFWDL, RW(r32(R92C_MCUFWDL), R92C_MCUFWDL_PAGE, page));
            size_t remaining = std::min<size_t>(length, R92C_FW_PAGE_SIZE);
            length -= remaining;
            uint16_t dest = R92C_FW_START_ADDR;
            while (remaining) {
                const size_t size = remaining > 196 ? 196 : remaining > 4 ? 4 : 1;
                write_bytes(dest, payload, size);
                dest += size;
                payload += size;
                remaining -= size;
            }
        }
        wait_firmware(R92C_MCUFWDL_CHKSUM_RPT, "firmware checksum timed out", 5);
        w8(R92C_MCUFWDL, r8(R92C_MCUFWDL) & ~R92C_MCUFWDL_EN);
        w8(R92C_MCUFWDL + 1, 0);
        w32(R92C_MCUFWDL, (r32(R92C_MCUFWDL) & ~R92C_MCUFWDL_WINTINI_RDY) | R92C_MCUFWDL_RDY);
        uint16_t cpu = r16(R92C_SYS_FUNC_EN);
        w16(R92C_SYS_FUNC_EN, cpu & ~R92C_SYS_FUNC_EN_CPUEN);
        w16(R92C_SYS_FUNC_EN, cpu | R92C_SYS_FUNC_EN_CPUEN);
        wait_firmware(R92C_MCUFWDL_WINTINI_RDY, "firmware boot timed out", 10);
        fw_ready = true;
    }

    void wait_firmware(uint32_t flag, const char *error, unsigned interval) {
        for (unsigned i = 0; i < 1000; ++i) {
            if (r32(R92C_MCUFWDL) & flag) return;
            delay_us(interval);
        }
        throw std::runtime_error(error);
    }

#include "rf.inc"
#include "station.inc"

    void start(unsigned out_count, unsigned channel) {
        if (changed) throw std::runtime_error("stop before starting another session");
        if (channel < 1 || channel > 13 || out_count < 1 || out_count > 3)
            throw std::runtime_error("invalid endpoint count or receive channel");
        uint32_t silicon = r32(R92C_SYS_CFG);
        if (!(silicon & R92C_SYS_CFG_TYPE_92C) || (silicon & R92C_SYS_CFG_TRP_VAUX_EN) ||
            MS(r32(R92C_HPON_FSM), R92C_HPON_FSM_CHIP_BONDING_ID) == R92C_HPON_FSM_CHIP_BONDING_ID_92C_1T2R)
            throw std::runtime_error("unsupported silicon (requires production RTL8192C 2T2R)");
        changed = true;
        r92c_rom board = board_rom();
        board_cal = board;
        uint8_t pa = efuse_read(0x1fa);
        power_on();
        dma_init(out_count);
        // Match the upstream MAC identity programming even for an unassociated
        // receiver. The address stays in the adapter; diagnostics omit it.
        for (unsigned i = 0; i < IEEE80211_ADDR_LEN; ++i)
            w8(R92C_MACID + i, board.macaddr[i]);
        firmware();
        for (auto &entry : rtl8192cu_mac) w8(entry.reg, entry.val);
        w8(R92C_TXPAUSE, 0xff);
        w16(R92C_CR, r16(R92C_CR) & ~R92C_CR_MACTXEN);
        board_type = MS(board.rf_opt1, R92C_ROM_RF1_BOARD_TYPE);
        baseband_and_rf(board_type, pa);
        tune(channel);

        // Unassociated station, accepting broadcast beacons only.
        w8(R92C_MSR, (r8(R92C_MSR) & ~R92C_MSR_NETTYPE_MASK) | R92C_MSR_NETTYPE_NOLINK);
        w16(R92C_BCN_CTRL, (R92C_BCN_CTRL_DIS_TSF_UDT0 << 8) | R92C_BCN_CTRL_DIS_TSF_UDT0);
        // OpenBSD rtwn_rxfilter_init + rtwn_newstate(SCAN): clear both
        // BSSID checks and set the initial scan gain on both RF chains.
        // AMF would bypass the subtype map, so leave it clear for beacons.
        w32(R92C_RCR, R92C_RCR_AAP | R92C_RCR_APM | R92C_RCR_AM | R92C_RCR_AB |
            R92C_RCR_HTC_LOC_CTRL | R92C_RCR_APP_PHYSTS |
            R92C_RCR_APP_ICV | R92C_RCR_APP_MIC);
        w32(R92C_MAR, 0xffffffff);
        w32(R92C_MAR + 4, 0xffffffff);
        w16(R92C_RXFLTMAP0, 1u << 8);   // management subtype 8 = beacon
        w16(R92C_RXFLTMAP1, 0);
        w16(R92C_RXFLTMAP2, 0);
        for (unsigned chain = 0; chain < 2; ++chain) {
            const uint16_t reg = R92C_OFDM0_AGCCORE1(chain);
            w32(reg, RW(r32(reg), R92C_OFDM0_AGCCORE1_GAIN, 0x20));
        }
        // OpenBSD urtwn_r92c_power_on enables the two WMAC blocks together.
        // Restore that normal MAC clock configuration after setting no-link
        // mode and the beacon-only receive map. All transmit queues stay
        // paused and the user-space transport never submits an outbound frame.
        if (r8(R92C_TXPAUSE) != R92C_TXPAUSE_ALL)
            throw std::runtime_error("transmit queues must stay paused during listening");
        w16(R92C_CR, r16(R92C_CR) | R92C_CR_MACTXEN | R92C_CR_MACRXEN);
        active = true;
    }
};

template<class F>
static int guarded(rtl_radio *radio, F fn) {
    if (!radio) return -1;
    radio->diagnostic[0] = 0;
    try { fn(); return 0; }
    catch (const std::exception &error) {
        std::snprintf(radio->diagnostic, sizeof(radio->diagnostic), "%s", error.what());
        return -1;
    }
}

extern "C" rtl_radio *rtl_radio_create(rtl_radio_io io) {
    return io.read && io.write ? new(std::nothrow) rtl_radio(io) : nullptr;
}
extern "C" int rtl_radio_start(rtl_radio *radio, unsigned out_count, unsigned channel) {
    return guarded(radio, [&] { radio->start(out_count, channel); });
}
extern "C" int rtl_radio_tune(rtl_radio *radio, unsigned channel) {
    return guarded(radio, [&] {
        if (!radio->active || radio->station.phase != rtl_radio::StationState::idle ||
            channel < 1 || channel > 13) throw std::runtime_error("invalid tune request");
        radio->tune(channel);
    });
}
extern "C" int rtl_radio_stop(rtl_radio *radio) {
    return guarded(radio, [&] {
        if (!radio->changed) return;
        radio->active = false;
        radio->clear_station();
        radio->power_off();
        radio->fw_ready = false;
        radio->changed = false;
    });
}
extern "C" const char *rtl_radio_error(const rtl_radio *radio) { return radio ? radio->diagnostic : "no radio"; }
extern "C" int rtl_radio_firmware_ready(const rtl_radio *radio) { return radio && radio->fw_ready; }
extern "C" int rtl_radio_snapshot(rtl_radio *radio, char *text, size_t size) {
    return guarded(radio, [&] {
        if (!radio->active || !text || size < 512) throw std::runtime_error("snapshot needs active radio and 512-byte buffer");
        // All offsets below are readable status/configuration registers. Unlike
        // RF indirect addressing, sampling these registers needs no writes.
        std::snprintf(text, size,
            "board=%u CR=%04x TXPAUSE=%02x MSR=%02x RCR=%08x MAP=%04x/%04x/%04x "
            "DMA=%04x USB=%02x RXPKT=%08x DMA_STATUS=%08x FIFO=%08x PTR=%08x "
            "RFMOD=%08x RFPARAM=%08x TRXPATH=%08x CCA=%08x AGC=%02x/%02x "
            "MGNT_DOK=%u TX_OVERFLOW=%u",
            unsigned(radio->board_type), radio->r16(R92C_CR), radio->r8(R92C_TXPAUSE),
            radio->r8(R92C_MSR), radio->r32(R92C_RCR),
            radio->r16(R92C_RXFLTMAP0), radio->r16(R92C_RXFLTMAP1), radio->r16(R92C_RXFLTMAP2),
            radio->r16(R92C_TRXDMA_CTRL), radio->r8(R92C_USB_SPECIAL_OPTION),
            radio->r32(R92C_RXPKT_NUM), radio->r32(R92C_RXDMA_STATUS),
            radio->r32(R92C_TRXFF_STATUS), radio->r32(R92C_RXFF_PTR),
            radio->r32(R92C_FPGA0_RFMOD), radio->r32(R92C_FPGA0_RFPARAM(0)),
            radio->r32(R92C_OFDM0_TRXPATHENA), radio->r32(R92E_RX_WAIT_CCA),
            radio->r8(R92C_OFDM0_AGCCORE1(0)), radio->r8(R92C_OFDM0_AGCCORE1(1)),
            unsigned((radio->r32(R92C_HISR) & R92C_IMR_MGNTDOK) != 0),
            unsigned((radio->r32(R92C_HISR) & R92C_IMR_TXFOVW) != 0));
    });
}
extern "C" void rtl_radio_destroy(rtl_radio *radio) {
    if (radio) radio->clear_station();
    delete radio;
}
#include "station_api.inc"
