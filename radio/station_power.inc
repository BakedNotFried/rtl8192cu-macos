/*-
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
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
// RTL8192CU 2T2R, HT20. Per-rate power indices from board eFuse.
#define RTWN_RIDX_CCK1		0
#define RTWN_RIDX_CCK2		1
#define RTWN_RIDX_CCK11		3
#define RTWN_RIDX_OFDM6		4
#define RTWN_RIDX_OFDM54	11
#define RTWN_RIDX_MCS0		12
#define RTWN_RIDX_MCS8		(RTWN_RIDX_MCS0 + 8)
#define RTWN_RIDX_MCS15		27
#define RTWN_RIDX_MAX		27

#define RTWN_POWER_CCK1		0
#define RTWN_POWER_CCK2		1
#define RTWN_POWER_CCK55	2
#define RTWN_POWER_CCK11	3
#define RTWN_POWER_OFDM6	4
#define RTWN_POWER_OFDM9	5
#define RTWN_POWER_OFDM12	6
#define RTWN_POWER_OFDM18	7
#define RTWN_POWER_OFDM24	8
#define RTWN_POWER_OFDM36	9
#define RTWN_POWER_OFDM48	10
#define RTWN_POWER_OFDM54	11
#define RTWN_POWER_MCS(mcs)	(12 + (mcs))
#define RTWN_POWER_COUNT	28

void program_power(int chain, uint16_t power[RTWN_POWER_COUNT])
{
	uint32_t reg;

	/* Write per-CCK rate Tx power. */
	if (chain == 0) {
		reg = r32(R92C_TXAGC_A_CCK1_MCS32);
		reg = RW(reg, R92C_TXAGC_A_CCK1,  power[RTWN_POWER_CCK1]);
		w32(R92C_TXAGC_A_CCK1_MCS32, reg);
		reg = r32(R92C_TXAGC_B_CCK11_A_CCK2_11);
		reg = RW(reg, R92C_TXAGC_A_CCK2,  power[RTWN_POWER_CCK2]);
		reg = RW(reg, R92C_TXAGC_A_CCK55, power[RTWN_POWER_CCK55]);
		reg = RW(reg, R92C_TXAGC_A_CCK11, power[RTWN_POWER_CCK11]);
		w32(R92C_TXAGC_B_CCK11_A_CCK2_11, reg);
	} else {
		reg = r32(R92C_TXAGC_B_CCK1_55_MCS32);
		reg = RW(reg, R92C_TXAGC_B_CCK1,  power[RTWN_POWER_CCK1]);
		reg = RW(reg, R92C_TXAGC_B_CCK2,  power[RTWN_POWER_CCK2]);
		reg = RW(reg, R92C_TXAGC_B_CCK55, power[RTWN_POWER_CCK55]);
		w32(R92C_TXAGC_B_CCK1_55_MCS32, reg);
		reg = r32(R92C_TXAGC_B_CCK11_A_CCK2_11);
		reg = RW(reg, R92C_TXAGC_B_CCK11, power[RTWN_POWER_CCK11]);
		w32(R92C_TXAGC_B_CCK11_A_CCK2_11, reg);
	}
	/* Write per-OFDM rate Tx power. */
	w32(R92C_TXAGC_RATE18_06(chain),
	    SM(R92C_TXAGC_RATE06, power[RTWN_POWER_OFDM6]) |
	    SM(R92C_TXAGC_RATE09, power[RTWN_POWER_OFDM9]) |
	    SM(R92C_TXAGC_RATE12, power[RTWN_POWER_OFDM12]) |
	    SM(R92C_TXAGC_RATE18, power[RTWN_POWER_OFDM18]));
	w32(R92C_TXAGC_RATE54_24(chain),
	    SM(R92C_TXAGC_RATE24, power[RTWN_POWER_OFDM24]) |
	    SM(R92C_TXAGC_RATE36, power[RTWN_POWER_OFDM36]) |
	    SM(R92C_TXAGC_RATE48, power[RTWN_POWER_OFDM48]) |
	    SM(R92C_TXAGC_RATE54, power[RTWN_POWER_OFDM54]));
	/* Write per-MCS Tx power. */
	w32(R92C_TXAGC_MCS03_MCS00(chain),
	    SM(R92C_TXAGC_MCS00,  power[RTWN_POWER_MCS( 0)]) |
	    SM(R92C_TXAGC_MCS01,  power[RTWN_POWER_MCS( 1)]) |
	    SM(R92C_TXAGC_MCS02,  power[RTWN_POWER_MCS( 2)]) |
	    SM(R92C_TXAGC_MCS03,  power[RTWN_POWER_MCS( 3)]));
	w32(R92C_TXAGC_MCS07_MCS04(chain),
	    SM(R92C_TXAGC_MCS04,  power[RTWN_POWER_MCS( 4)]) |
	    SM(R92C_TXAGC_MCS05,  power[RTWN_POWER_MCS( 5)]) |
	    SM(R92C_TXAGC_MCS06,  power[RTWN_POWER_MCS( 6)]) |
	    SM(R92C_TXAGC_MCS07,  power[RTWN_POWER_MCS( 7)]));
	{
		w32(R92C_TXAGC_MCS11_MCS08(chain),
		    SM(R92C_TXAGC_MCS08,  power[RTWN_POWER_MCS( 8)]) |
		    SM(R92C_TXAGC_MCS09,  power[RTWN_POWER_MCS( 9)]) |
		    SM(R92C_TXAGC_MCS10,  power[RTWN_POWER_MCS(10)]) |
		    SM(R92C_TXAGC_MCS11,  power[RTWN_POWER_MCS(11)]));
		w32(R92C_TXAGC_MCS15_MCS12(chain),
		    SM(R92C_TXAGC_MCS12,  power[RTWN_POWER_MCS(12)]) |
		    SM(R92C_TXAGC_MCS13,  power[RTWN_POWER_MCS(13)]) |
		    SM(R92C_TXAGC_MCS14,  power[RTWN_POWER_MCS(14)]) |
		    SM(R92C_TXAGC_MCS15,  power[RTWN_POWER_MCS(15)]));
	}
}
void station_tx_power(int chain, int chan) {
	uint16_t power[RTWN_POWER_COUNT];
	uint16_t cckpow, ofdmpow, htpow, diff, max;
	const struct r92c_txpwr *base;
	int ridx, group;

	/* Determine channel group. */
	auto *rom = &board_cal;
	auto regulatory = MS(rom->rf_opt1, R92C_ROM_RF1_REGULATORY);
	if (chan <= 3)
		group = 0;
	else if (chan <= 9)
		group = 1;
	else
		group = 2;

	/* Get original Tx power based on board type and RF chain. */
	base = &rtl8192cu_txagc[chain];

	memset(power, 0, RTWN_POWER_COUNT * sizeof(power[0]));
	if (regulatory == 0) {
		for (ridx = RTWN_RIDX_CCK1; ridx <= RTWN_RIDX_CCK11; ridx++)
			power[ridx] = base->pwr[0][ridx];
	}
	for (ridx = RTWN_RIDX_OFDM6; ridx <= RTWN_RIDX_MAX; ridx++) {
		if (regulatory == 3) {
			power[ridx] = base->pwr[0][ridx];
			/* Apply vendor limits. */
			max = rom->ht20_max_pwr[group];
			max = (max >> (chain * 4)) & 0xf;
			if (power[ridx] > max)
				power[ridx] = max;
		} else if (regulatory == 1) {
			power[ridx] = base->pwr[group][ridx];
		} else if (regulatory != 2)
			power[ridx] = base->pwr[0][ridx];
	}

	/* Compute per-CCK rate Tx power. */
	cckpow = rom->cck_tx_pwr[chain][group];
	for (ridx = RTWN_RIDX_CCK1; ridx <= RTWN_RIDX_CCK11; ridx++) {
		power[ridx] += cckpow;
		if (power[ridx] > R92C_MAX_TX_PWR)
			power[ridx] = R92C_MAX_TX_PWR;
	}

	htpow = rom->ht40_1s_tx_pwr[chain][group];
	{
		/* Apply reduction for 2 spatial streams. */
		diff = rom->ht40_2s_tx_pwr_diff[group];
		diff = (diff >> (chain * 4)) & 0xf;
		htpow = (htpow > diff) ? htpow - diff : 0;
	}

	/* Compute per-OFDM rate Tx power. */
	diff = rom->ofdm_tx_pwr_diff[group];
	diff = (diff >> (chain * 4)) & 0xf;
	ofdmpow = htpow + diff;	/* HT->OFDM correction. */
	for (ridx = RTWN_RIDX_OFDM6; ridx <= RTWN_RIDX_OFDM54; ridx++) {
		power[ridx] += ofdmpow;
		if (power[ridx] > R92C_MAX_TX_PWR)
			power[ridx] = R92C_MAX_TX_PWR;
	}

	/* Compute per-MCS Tx power. */
	{
		diff = rom->ht20_tx_pwr_diff[group];
		diff = (diff >> (chain * 4)) & 0xf;
		htpow += diff;	/* HT40->HT20 correction. */
	}
	for (ridx = RTWN_RIDX_MCS0; ridx <= RTWN_RIDX_MCS15; ridx++) {
		power[ridx] += htpow;
		if (power[ridx] > R92C_MAX_TX_PWR)
			power[ridx] = R92C_MAX_TX_PWR;
	}

	program_power(chain, power);
}
