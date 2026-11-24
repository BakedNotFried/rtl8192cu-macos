// Exercise the firmware path through the public ABI without a USB device.
#include "radio.h"
#include "station_hw.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define IEEE80211_ADDR_LEN 6
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#define __packed __attribute__((packed))
#include "upstream/r92creg.h"
#include "upstream/microcode.h"

struct FakeUSB {
    std::array<uint8_t, 65536> regs{};
    std::array<uint8_t, 512> fuse{};
    uint32_t rf[2][64]{};
    bool rf_read_pending = false;
    std::vector<uint8_t> uploaded;
    std::vector<uint8_t> last_tx;
    unsigned queue = 99;
    unsigned writes = 0;
    bool block_page_two = false;
    bool fail_mac = true;

    FakeUSB() {
        set(R92C_SYS_CFG, 0x0c400335, 4);
        set(R92C_HPON_FSM, 0x98853f15, 4);
        set(R92C_APS_FSMCO, R92C_APS_FSMCO_PFM_ALDN, 2);
        fuse.fill(0xff);
        r92c_rom board{};
        board.id = 0x8129;
        board.macaddr[0] = 2;
        board.macaddr[5] = 9;
        const auto *bytes = reinterpret_cast<const uint8_t *>(&board);
        size_t pos = 0;
        static_assert(sizeof(board) % 8 == 0);
        for (size_t block = 0; block < sizeof(board)/8; ++block) {
            fuse[pos++] = uint8_t(block << 4);
            for (size_t i = 0; i < 8; ++i) fuse[pos++] = bytes[8*block+i];
        }
    }

    void set(unsigned reg, uint32_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i)
            regs[reg + i] = uint8_t(value >> (8 * i));
    }
    uint32_t get(unsigned reg, size_t size) const {
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= uint32_t(regs[reg + i]) << (8 * i);
        return value;
    }

    static int read_cb(void *ctx, uint16_t reg, uint8_t *bytes, size_t size) {
        auto &dev = *static_cast<FakeUSB *>(ctx);
        std::memcpy(bytes, dev.regs.data() + reg, size);
        return 0;
    }
    static int bulk_cb(void *ctx, unsigned index, const uint8_t *bytes, size_t size) {
        auto &dev = *static_cast<FakeUSB *>(ctx);
        dev.last_tx.assign(bytes, bytes+size);
        dev.queue = index;
        assert(size >= 32);
        uint16_t checksum = 0;
        for (size_t i = 0; i < 32; i += 2) checksum ^= bytes[i] | uint16_t(bytes[i+1]) << 8;
        assert(checksum == 0);
        return 0;
    }
    static int write_cb(void *ctx, uint16_t reg, const uint8_t *bytes, size_t size) {
        auto &dev = *static_cast<FakeUSB *>(ctx);
        ++dev.writes;
        if (reg >= R92C_FW_START_ADDR && reg < R92C_FW_START_ADDR + R92C_FW_PAGE_SIZE) {
            unsigned page = MS(dev.get(R92C_MCUFWDL, 4), R92C_MCUFWDL_PAGE);
            assert(page * R92C_FW_PAGE_SIZE + reg - R92C_FW_START_ADDR == dev.uploaded.size());
            assert(size <= 196 && reg + size <= R92C_FW_START_ADDR + R92C_FW_PAGE_SIZE);
            if (page == 2 && dev.block_page_two) return 0x1234;
            dev.uploaded.insert(dev.uploaded.end(), bytes, bytes + size);
            if (dev.uploaded.size() == sizeof(rtl8192cT) - sizeof(r92c_fw_hdr))
                dev.regs[R92C_MCUFWDL] |= R92C_MCUFWDL_CHKSUM_RPT;
            return 0;
        }
        if (reg == rtl8192cu_mac[0].reg && dev.fail_mac) return 0x4567;
        std::memcpy(dev.regs.data() + reg, bytes, size);
        if (reg == R92C_EFUSE_CTRL) {
            auto cmd = dev.get(reg, 4);
            assert(!(cmd & R92C_EFUSE_CTRL_VALID)); // reads only
            dev.set(reg, R92C_EFUSE_CTRL_VALID | dev.fuse.at(MS(cmd, R92C_EFUSE_CTRL_ADDR)), 4);
        } else if (reg == R92C_APS_FSMCO && size == 2) {
            dev.set(reg, dev.get(reg, 2) & ~R92C_APS_FSMCO_APFM_ONMAC, 2);
        } else if (reg == R92C_LLT_INIT) {
            dev.set(reg, 0, 4);
        } else if (reg == R92C_CAMCMD) {
            dev.set(reg, 0, 4);
        } else if (reg == R92C_MCUFWDL && dev.uploaded.empty()) {
            dev.regs[reg] &= ~R92C_MCUFWDL_CHKSUM_RPT;
        } else if (reg == R92C_SYS_FUNC_EN && (dev.regs[R92C_MCUFWDL] & R92C_MCUFWDL_RDY)) {
            dev.regs[R92C_MCUFWDL] |= R92C_MCUFWDL_WINTINI_RDY;
        }
        if (reg == R92C_HSSI_PARAM2(0) && size == 4 &&
            !(dev.get(reg, 4) & R92C_HSSI_PARAM2_READ_EDGE)) dev.rf_read_pending = true;
        for (unsigned chain = 0; chain < 2; ++chain) {
            if (reg == R92C_LSSI_PARAM(chain) && size == 4) {
                auto command = dev.get(reg, 4);
                auto address = MS(command, R92C_LSSI_PARAM_ADDR);
                if (address < 64) dev.rf[chain][address] = MS(command, R92C_LSSI_PARAM_DATA);
            }
            if (reg == R92C_HSSI_PARAM2(chain) && size == 4 && dev.rf_read_pending &&
                (dev.get(reg, 4) & R92C_HSSI_PARAM2_READ_EDGE)) {
                auto address = MS(dev.get(reg, 4), R92C_HSSI_PARAM2_READ_ADDR);
                if (address < 64) {
                    dev.set(R92C_LSSI_READBACK(chain), dev.rf[chain][address], 4);
                    dev.set(R92C_HSPI_READBACK(chain), dev.rf[chain][address], 4);
                }
                dev.rf_read_pending = false;
            }
        }
        return 0;
    }
};

static void exercise_station(rtl_radio *radio, FakeUSB &dev, bool named) {
    const uint8_t ap[6] = {2,0,0,0,0,1};
    uint8_t own[6], frame[256]{};
    const uint8_t test_ssid[] = {'L','a','b',0,'0','1'};
    assert(rtl_station_prepare_named(radio, ap, 6, 2, FakeUSB::bulk_cb, &dev, own, test_ssid, 0) != 0);
    if (named)
        assert(rtl_station_prepare_named(radio, ap, 6, 2, FakeUSB::bulk_cb, &dev, own, test_ssid, sizeof(test_ssid)) == 0);
    else
        assert(rtl_station_prepare(radio, ap, 6, 2, FakeUSB::bulk_cb, &dev, own) == 0);
    assert(own[0] == 2 && own[5] == 9);
    assert(rtl_radio_tune(radio, 7) != 0);
    frame[0] = 0xb0;
    std::memcpy(frame+4,ap,6);
    std::memcpy(frame+10,own,6);
    std::memcpy(frame+16,ap,6);
    frame[26] = 1;
    frame[4] ^= 1;
    assert(rtl_station_auth(radio,frame,30) != 0 && dev.last_tx.empty());
    frame[4] ^= 1;
    assert(rtl_station_auth(radio,frame,30) == 0 && dev.queue == 0 && dev.last_tx.size() == 62);
    frame[0] = 0;
    frame[24] = 0x11;
    const uint8_t ies[] = {0,6,'C','a','l','N','e','t',1,1,0x82,
                           48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    std::memcpy(frame+28,ies,sizeof(ies));
    if (named) std::memcpy(frame + 30,test_ssid,sizeof(test_ssid));
    frame[34] ^= 1; // differs after embedded NUL in the named test
    assert(rtl_station_assoc(radio,frame,28+sizeof(ies)) != 0 && dev.last_tx.size() == 62);
    frame[34] ^= 1;
    assert(rtl_station_assoc(radio,frame,28+sizeof(ies)) == 0 && dev.queue == 0);
    assert(rtl_station_link(radio,1,100) == 0);
    uint8_t tk[16] = {5}, gtk[16] = {7}, ethernet[64]{};
    std::memcpy(ethernet + 6, own, 6);
    ethernet[12] = 8;
    assert(rtl_station_ethernet(radio,ethernet,sizeof(ethernet)) != 0);
    assert(rtl_station_keys(radio,tk,1,gtk,4) == 0);
    assert(rtl_station_ethernet(radio,ethernet,sizeof(ethernet)) == 0);
    assert(dev.queue == 1 && dev.last_tx[32+1] == 0x41 && dev.last_tx[32+24] == 1);
    unsigned writes = dev.writes;
    assert(rtl_station_keys(radio,tk,1,gtk,4) == 0 && writes == dev.writes);
    assert(rtl_station_ethernet(radio,ethernet,sizeof(ethernet)) == 0 && dev.last_tx[32+24] == 2);
    // Independent hardware-decrypted RX fixture: FromDS, AES, IV + 8-byte MIC.
    uint8_t rx[64] = {8,0x42};
    std::memcpy(rx+4,own,6);
    std::memcpy(rx+10,ap,6);
    rx[24] = 1;
    rx[27] = 0x20;
    rx[32] = 0xaa; rx[33] = 0xaa; rx[34] = 3; rx[38] = 8;
    size_t bytes = 0;
    uint32_t aes = uint32_t(4) << 20;
    assert(rtl_station_receive_ethernet(radio,aes|(1u<<27),rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 0);
    assert(rtl_station_receive_ethernet(radio,aes|R92C_RXDW0_ICVERR,rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 0);
    assert(rtl_station_receive_ethernet(radio,aes,rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 1);
    assert(bytes == 30 && ethernet[12] == 8 && !std::memcmp(ethernet,own,6));
    assert(rtl_station_receive_ethernet(radio,aes,rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 0);
    std::memset(rx+4,0xff,6);
    rx[27] = 0x60;
    rx[24] = 4;
    assert(rtl_station_receive_ethernet(radio,aes,rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 0);
    rx[24] = 5;
    assert(rtl_station_receive_ethernet(radio,aes,rx,sizeof(rx),ethernet,sizeof(ethernet),&bytes) == 1);
}

int main() {
    for (bool fail : {false, true}) {
        FakeUSB dev;
        dev.block_page_two = fail;
        auto *radio = rtl_radio_create({&dev, FakeUSB::read_cb, FakeUSB::write_cb});
        assert(radio);
        assert(rtl_radio_start(radio, 0, 1) != 0 && dev.writes == 0);
        assert(rtl_radio_start(radio, 2, 99) != 0 && dev.writes == 0);
        assert(rtl_radio_start(radio, 2, 1) != 0); // deliberately injected error
        assert(std::strstr(rtl_radio_error(radio), fail ? "0x1234" : "0x4567"));
        assert(rtl_radio_firmware_ready(radio) == !fail);
        assert(dev.uploaded.size() == (fail ? 2 * R92C_FW_PAGE_SIZE : sizeof(rtl8192cT) - sizeof(r92c_fw_hdr)));
        assert(std::memcmp(dev.uploaded.data(), rtl8192cT + sizeof(r92c_fw_hdr), dev.uploaded.size()) == 0);
        assert(rtl_radio_tune(radio, 6) != 0);
        assert(rtl_radio_stop(radio) == 0 && rtl_radio_firmware_ready(radio) == 0);
        unsigned after_stop = dev.writes;
        assert(rtl_radio_stop(radio) == 0 && dev.writes == after_stop);
        rtl_radio_destroy(radio);
    }
    FakeUSB device;
    device.fail_mac = false;
    auto *radio = rtl_radio_create({&device, FakeUSB::read_cb, FakeUSB::write_cb});
    char state[512];
    assert(rtl_radio_snapshot(radio, state, sizeof(state)) != 0);
    if (rtl_radio_start(radio, 2, 6)) {
        std::fprintf(stderr, "Mock startup failed: %s\n", rtl_radio_error(radio));
        return 1;
    }
    unsigned writes_before = device.writes;
    assert(rtl_radio_snapshot(radio, state, 10) != 0);
    assert(rtl_radio_snapshot(radio, state, sizeof(state)) == 0);
    assert(device.writes == writes_before);
    assert(std::strstr(state, "MAP=0100/0000/0000"));
    assert(std::strstr(state, "TXPAUSE=ff"));
    assert((device.get(R92C_RCR, 4) & (R92C_RCR_AMF | R92C_RCR_ACF | R92C_RCR_ADF | R92C_RCR_CBSSID_BCN)) == 0);
    exercise_station(radio, device, false);
    assert(rtl_radio_stop(radio) == 0);
    rtl_radio_destroy(radio);
    FakeUSB named_device;
    named_device.fail_mac = false;
    auto *named_radio = rtl_radio_create({&named_device, FakeUSB::read_cb, FakeUSB::write_cb});
    assert(rtl_radio_start(named_radio, 2, 6) == 0);
    exercise_station(named_radio, named_device, true);
    assert(rtl_radio_stop(named_radio) == 0);
    rtl_radio_destroy(named_radio);
    std::puts("PASS firmware payload, page boundaries, error propagation, and cleanup");
}
