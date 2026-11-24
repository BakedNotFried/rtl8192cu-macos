/*
 * Derived from OpenBSD if_urtwn.c and rtwn.c. See upstream/revision.json.
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2014 Kevin Lo <kevlo@FreeBSD.org>
 * Copyright (c) 2015 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2016 Nathanial Sloss <nathanialsloss@yahoo.com.au>
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

uint32_t rf_channel_template[2]{};
bool rf_ready = false;

void write_rf(unsigned chain, uint16_t address, uint32_t value) {
    w32(R92C_LSSI_PARAM(chain),
        SM(R92C_LSSI_PARAM_ADDR, static_cast<uint32_t>(address)) |
        SM(R92C_LSSI_PARAM_DATA, value));
    delay_us(1);
}

uint32_t read_rf(unsigned chain, uint8_t address) {
    const uint32_t common = r32(R92C_HSSI_PARAM2(0));
    const uint32_t selected = chain == 0 ? common : r32(R92C_HSSI_PARAM2(chain));
    w32(R92C_HSSI_PARAM2(0), common & ~R92C_HSSI_PARAM2_READ_EDGE);
    delay_us(1000);
    w32(R92C_HSSI_PARAM2(chain),
        RW(selected, R92C_HSSI_PARAM2_READ_ADDR, static_cast<uint32_t>(address)) |
        R92C_HSSI_PARAM2_READ_EDGE);
    delay_us(1000);
    w32(R92C_HSSI_PARAM2(0), common | R92C_HSSI_PARAM2_READ_EDGE);
    delay_us(1000);
    const bool pi_mode = (r32(R92C_HSSI_PARAM1(chain)) & R92C_HSSI_PARAM1_PI) != 0;
    return MS(r32(pi_mode ? R92C_HSPI_READBACK(chain) : R92C_LSSI_READBACK(chain)),
              R92C_LSSI_READBACK_DATA);
}

void init_rf_chain(unsigned chain) {
    const uint32_t env_mask = 0x10u << (16 * chain);
    const uint32_t saved_env = r32(R92C_FPGA0_RFIFACESW(0)) & env_mask;
    const uint16_t output = R92C_FPGA0_RFIFACEOE(chain);
    const uint16_t param = R92C_HSSI_PARAM2(chain);
    w32(output, r32(output) | 0x100000);
    delay_us(50);
    w32(output, r32(output) | 0x10);
    delay_us(50);
    w32(param, r32(param) & ~R92C_HSSI_PARAM2_ADDR_LENGTH);
    delay_us(50);
    w32(param, r32(param) & ~R92C_HSSI_PARAM2_DATA_LENGTH);
    delay_us(50);

    // This two-chain table contains only RF addresses and the 50 ms delay opcode.
    const auto& program = rtl8192ce_rf_prog[chain];
    for (int i = 0; i < program.count; ++i) {
        const uint16_t reg = program.regs[i];
        if (reg == 0xfe) {
            delay_us(50000);
        } else {
            if (reg > 0x3f)
                throw std::runtime_error("Unknown RTL8192CU RF table opcode");
            write_rf(chain, reg, program.vals[i]);
            delay_us(5);
        }
    }
    w32(R92C_FPGA0_RFIFACESW(0), (r32(R92C_FPGA0_RFIFACESW(0)) & ~env_mask) | saved_env);
    rf_channel_template[chain] = read_rf(chain, R92C_RF_CHNLBW);
    // The RF-A table programs channel 1. RF-B has no initial channel table entry.
    if (rf_channel_template[chain] == R92C_LSSI_READBACK_DATA_M ||
        (chain == 0 && MS(rf_channel_template[chain], R92C_RF_CHNLBW_CHNL) != 1)) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "Invalid initial RF-%c channel template 0x%05x from register 0x18",
                      'A' + static_cast<int>(chain), rf_channel_template[chain]);
        throw std::runtime_error(message);
    }
}

void baseband_and_rf(uint8_t board_type, uint8_t pa_setting) {
    rf_ready = false;
    if (board_type > R92C_BOARD_TYPE_COMBO)
        throw std::runtime_error("Unrecognized RTL8192CU board type");
    if ((r16(R92C_CR) & R92C_CR_MACTXEN) || r8(R92C_TXPAUSE) != R92C_TXPAUSE_ALL)
        throw std::runtime_error("Passive radio setup requires disabled MAC transmit and paused queues");

    w16(R92C_SYS_FUNC_EN, r16(R92C_SYS_FUNC_EN) | R92C_SYS_FUNC_EN_BBRSTB |
        R92C_SYS_FUNC_EN_BB_GLB_RST | R92C_SYS_FUNC_EN_DIO_RF);
    w16(R92C_AFE_PLL_CTRL, 0xdb83);
    w8(R92C_RF_CTRL, R92C_RF_CTRL_EN | R92C_RF_CTRL_RSTB | R92C_RF_CTRL_SDMRSTB);
    w8(R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_USBA | R92C_SYS_FUNC_EN_USBD |
        R92C_SYS_FUNC_EN_BB_GLB_RST | R92C_SYS_FUNC_EN_BBRSTB);
    w8(R92C_LDOHCI12_CTRL, 0x0f);
    w8(0x15, 0xe9);
    w8(R92C_AFE_XTAL_CTRL + 1, 0x80);

    const auto& bb = board_type == R92C_BOARD_TYPE_MINICARD ?
                     rtl8192ce_bb_prog : rtl8192cu_bb_prog;
    for (int i = 0; i < bb.count; ++i) {
        w32(bb.regs[i], bb.vals[i]);
        delay_us(1);
    }
    for (int i = 0; i < bb.agccount; ++i) {
        w32(R92C_OFDM0_AGCRSSITABLE, bb.agcvals[i]);
        delay_us(1);
    }
    for (unsigned chain = 0; chain < 2; ++chain)
        init_rf_chain(chain);
    w32(R92C_FPGA0_RFMOD, r32(R92C_FPGA0_RFMOD) | R92C_RFMOD_CCK_EN);
    w32(R92C_FPGA0_RFMOD, r32(R92C_FPGA0_RFMOD) | R92C_RFMOD_OFDM_EN);

    // Only the non-transmitting LC calibration path is applicable to passive RX.
    if (r8(R92C_OFDM1_LSTF + 3) & 0x70)
        throw std::runtime_error("Unexpected continuous-transmit mode during RF calibration");
    write_rf(0, R92C_RF_CHNLBW, rf_channel_template[0] | R92C_RF_CHNLBW_LCSTART);
    delay_us(100000);
    w8(0xfe40, 0xe0);
    w8(0xfe41, 0x8d);
    w8(0xfe42, 0x80);
    for (unsigned chain = 0; chain < 2; ++chain) {
        if ((pa_setting & (1u << chain)) == 0) {
            write_rf(chain, R92C_RF_IPA, 0x0f406);
            write_rf(chain, R92C_RF_IPA, 0x4f406);
            write_rf(chain, R92C_RF_IPA, 0x8f406);
            write_rf(chain, R92C_RF_IPA, 0xcf406);
        }
    }
    if ((pa_setting & 0x10) == 0)
        w8(0x16, (r8(0x16) & 0x0f) | 0x90);
    w8(R92C_GPIO_MUXCFG, r8(R92C_GPIO_MUXCFG) & ~R92C_GPIO_MUXCFG_ENBT);
    w8(0x15, 0xe9);

    // Gen1 RTL8192CU RF wake, as described in Linux rtl8xxxu_gen1_enable_rf.
    // Both analog receive paths must leave preamble power-save after calibration.
    w8(R92C_SPS0_CTRL, r8(R92C_SPS0_CTRL) | 0x09);
    constexpr uint32_t rx_code_mask = 0x00380038;
    constexpr uint32_t rx_awake = 0x00080008;
    w32(R92C_FPGA0_RFPARAM(0), (r32(R92C_FPGA0_RFPARAM(0)) & ~rx_code_mask) | rx_awake);
    w32(R92C_FPGA0_RFMOD, r32(R92C_FPGA0_RFMOD) & ~R92C_RFMOD_JAPAN);
    w32(R92E_RX_WAIT_CCA, 0x63db25a0);
    for (unsigned chain = 0; chain < 2; ++chain)
        write_rf(chain, R92C_RF_AC, 0x32d95);

    rf_ready = true;
}

void tune(unsigned channel) {
    if (channel < 1 || channel > 13)
        throw std::runtime_error("Passive HT20 scan supports channels 1 through 13");
    if (!rf_ready)
        throw std::runtime_error("RTL8192CU RF must be initialized before tuning");

    w8(R92C_BWOPMODE, r8(R92C_BWOPMODE) | R92C_BWOPMODE_20MHZ);
    w32(R92C_FPGA0_RFMOD, r32(R92C_FPGA0_RFMOD) & ~R92C_RFMOD_40MHZ);
    w32(R92C_FPGA1_RFMOD, r32(R92C_FPGA1_RFMOD) & ~R92C_RFMOD_40MHZ);
    w32(R92C_FPGA0_ANAPARAM2, r32(R92C_FPGA0_ANAPARAM2) | R92C_FPGA0_ANAPARAM2_CBW20);
    const uint32_t ht20_channel = R92C_RF_CHNLBW_BW20 | channel;
    for (unsigned chain = 0; chain < 2; ++chain) {
        // The cached initialization value may include the transient LC trigger.
        const uint32_t preserved = rf_channel_template[chain] & ~(0xfffu | R92C_RF_CHNLBW_LCSTART);
        write_rf(chain, R92C_RF_CHNLBW, preserved | ht20_channel);
    }
    // rtlwifi's 8192c channel switch allows 10 ms for the synthesizer to settle.
    // OpenBSD programs both paths without imposing a register echo as an RX criterion.
    delay_us(10000);
    const uint32_t path_a = read_rf(0, R92C_RF_CHNLBW);
    const uint32_t path_b = read_rf(1, R92C_RF_CHNLBW);
    const bool a_matches = (path_a & 0xfff) == ht20_channel;
    // RTL8192CU's reference driver selects bandwidth on A. For B, only
    // compare the channel number; a mismatch leaves that path unverified.
    const bool b_matches = MS(path_b, R92C_RF_CHNLBW_CHNL) == channel;
    if (!a_matches || !b_matches) {
        std::fprintf(stderr, "RF channel %u readback: A=0x%05x (%s, expected low 12 bits 0x%03x), "
                     "B=0x%05x (%s, expected low 10 bits 0x%03x)\n",
                     channel, path_a, a_matches ? "matched" : "unverified", ht20_channel,
                     path_b, b_matches ? "matched" : "unverified", channel);
    }
}
