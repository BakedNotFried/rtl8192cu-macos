/*-
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2014 Kevin Lo <kevlo@FreeBSD.org>
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
// RTL8192CU USB-only power sequences from the pinned if_urtwn.c.
void power_on()
{
	uint32_t reg;
	int ntries;

	/* Wait for autoload done bit. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (r8(R92C_APS_FSMCO) & R92C_APS_FSMCO_PFM_ALDN)
			break;
		delay_us(5);
	}
	if (ntries == 1000) {
		throw std::runtime_error("timeout waiting for chip autoload");
	}

	/* Unlock ISO/CLK/Power control register. */
	w8(R92C_RSV_CTRL, 0);
	/* Move SPS into PWM mode. */
	w8(R92C_SPS0_CTRL, 0x2b);
	delay_us(100);

	reg = r8(R92C_LDOV12D_CTRL);
	if (!(reg & R92C_LDOV12D_CTRL_LDV12_EN)) {
		w8(R92C_LDOV12D_CTRL,
		    reg | R92C_LDOV12D_CTRL_LDV12_EN);
		delay_us(100);
		w8(R92C_SYS_ISO_CTRL,
		    r8(R92C_SYS_ISO_CTRL) &
		    ~R92C_SYS_ISO_CTRL_MD2PP);
	}

	/* Auto enable WLAN. */
	w16(R92C_APS_FSMCO,
	    r16(R92C_APS_FSMCO) | R92C_APS_FSMCO_APFM_ONMAC);
	for (ntries = 0; ntries < 1000; ntries++) {
		if (!(r16(R92C_APS_FSMCO) &
		    R92C_APS_FSMCO_APFM_ONMAC))
			break;
		delay_us(5);
	}
	if (ntries == 1000) {
		throw std::runtime_error("timeout waiting for MAC auto ON");
	}

	/* Enable radio, GPIO and LED functions. */
	w16(R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS |
	    R92C_APS_FSMCO_PDN_EN |
	    R92C_APS_FSMCO_PFM_ALDN);
	/* Release RF digital isolation. */
	w16(R92C_SYS_ISO_CTRL,
	    r16(R92C_SYS_ISO_CTRL) & ~R92C_SYS_ISO_CTRL_DIOR);

	/* Initialize MAC. */
	w8(R92C_APSD_CTRL,
	    r8(R92C_APSD_CTRL) & ~R92C_APSD_CTRL_OFF);
	for (ntries = 0; ntries < 1000; ntries++) {
		if (!(r8(R92C_APSD_CTRL) &
		    R92C_APSD_CTRL_OFF_STATUS))
			break;
		delay_us(50);
	}
	if (ntries == 1000) {
		throw std::runtime_error("timeout waiting for MAC initialization");
	}

	/* Enable MAC DMA/WMAC/SCHEDULE/SEC blocks. */
	reg = r16(R92C_CR);
	reg |= R92C_CR_HCI_TXDMA_EN | R92C_CR_HCI_RXDMA_EN |
	    R92C_CR_TXDMA_EN | R92C_CR_RXDMA_EN | R92C_CR_PROTOCOL_EN |
	    R92C_CR_SCHEDULE_EN | R92C_CR_MACRXEN |
	    R92C_CR_ENSEC;
	w8(R92C_TXPAUSE, 0xff);
	w16(R92C_CR, reg & ~R92C_CR_MACTXEN);

	w8(0xfe10, 0x19);
}

void power_off()
{
	uint32_t reg;

	/* Block all Tx queues. */
	w8(R92C_TXPAUSE, R92C_TXPAUSE_ALL);

	/* Disable RF. */
	w32(R92C_LSSI_PARAM(0), 0);

	w8(R92C_APSD_CTRL, R92C_APSD_CTRL_OFF);

	/* Reset BB state machine. */
	w8(R92C_SYS_FUNC_EN,
	    R92C_SYS_FUNC_EN_USBD | R92C_SYS_FUNC_EN_USBA |
	    R92C_SYS_FUNC_EN_BB_GLB_RST);
	w8(R92C_SYS_FUNC_EN,
	    R92C_SYS_FUNC_EN_USBD | R92C_SYS_FUNC_EN_USBA);

	/* Reset digital sequence. */
	if (r8(R92C_MCUFWDL) & R92C_MCUFWDL_RDY) {
		/* Reset MCU ready status. */
		w8(R92C_MCUFWDL, 0);
		/* If firmware in ram code, do reset. */
		firmware_reset();
	}

	/* Reset MAC and enable 8051. */
	w8(R92C_SYS_FUNC_EN + 1,
	    (R92C_SYS_FUNC_EN_CPUEN | R92C_SYS_FUNC_EN_ELDR |
	    R92C_SYS_FUNC_EN_HWPDN) >> 8);

	/* Reset MCU ready status. */
	w8(R92C_MCUFWDL, 0);

	/* Disable MAC clock. */
	w16(R92C_SYS_CLKR,
	    R92C_SYS_CLKR_ANAD16V_EN | R92C_SYS_CLKR_ANA8M |
	    R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_80M_SSC_DIS |
	    R92C_SYS_CLKR_SYS_EN | R92C_SYS_CLKR_RING_EN | 0x4000);

	/* Disable AFE PLL. */
	w8(R92C_AFE_PLL_CTRL, 0x80);

	/* Gated AFE DIG_CLOCK. */
	w16(R92C_AFE_XTAL_CTRL, 0x880f);

	/* Isolated digital to PON. */
	w8(R92C_SYS_ISO_CTRL,
	    R92C_SYS_ISO_CTRL_MD2PP | R92C_SYS_ISO_CTRL_PA2PCIE |
	    R92C_SYS_ISO_CTRL_PD2CORE | R92C_SYS_ISO_CTRL_IP2MAC |
	    R92C_SYS_ISO_CTRL_DIOP | R92C_SYS_ISO_CTRL_DIOE);

	/*
	 * Pull GPIO PIN to balance level and LED control.
	 */
	/* Disable GPIO[7:0] */
	w16(R92C_GPIO_PIN_CTRL + 2, 0x0000);

	reg = r32(R92C_GPIO_PIN_CTRL) & ~0x0000ff00;
	reg |= ((reg << 8) & 0x0000ff00) | 0x00ff0000;
	w32(R92C_GPIO_PIN_CTRL, reg);

	/* Disable GPIO[10:8] */
	w8(R92C_MAC_PINMUX_CFG, 0x00);
	reg = r16(R92C_GPIO_IO_SEL) & ~0x00f0;
	reg |= (((reg & 0x000f) << 4) | 0x0780);
	w16(R92C_GPIO_IO_SEL, reg);

	/* Disable LED0 & 1 */
	w16(R92C_LEDCFG0, 0x8080);

	/*
	 * Reset digital sequence.
	 */
	/* Disable ELDR clock. */
	w16(R92C_SYS_CLKR,
	    R92C_SYS_CLKR_ANAD16V_EN | R92C_SYS_CLKR_ANA8M |
	    R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_80M_SSC_DIS |
	    R92C_SYS_CLKR_SYS_EN | R92C_SYS_CLKR_RING_EN | 0x4000);

	/* Isolated ELDR to PON. */
	w8(R92C_SYS_ISO_CTRL + 1,
	    (R92C_SYS_ISO_CTRL_DIOR | R92C_SYS_ISO_CTRL_PWC_EV12V) >> 8);

	/*
	 * Disable analog sequence.
	 */
	/* Disable A15 power. */
	w8(R92C_LDOA15_CTRL, R92C_LDOA15_CTRL_OBUF);
	/* Disable digital core power. */
	w8(R92C_LDOV12D_CTRL,
	    r8(R92C_LDOV12D_CTRL) &
	    ~R92C_LDOV12D_CTRL_LDV12_EN);

	/* Enter PFM mode. */
	w8(R92C_SPS0_CTRL, 0x23);

	/* Set USB suspend. */
	w16(R92C_APS_FSMCO,
	    R92C_APS_FSMCO_APDM_HOST | R92C_APS_FSMCO_AFSM_HSUS |
	    R92C_APS_FSMCO_PFM_ALDN);

	/* Lock ISO/CLK/Power control register. */
	w8(R92C_RSV_CTRL, 0x0e);
}
