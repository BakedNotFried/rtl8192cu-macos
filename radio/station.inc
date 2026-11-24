// RTL8192CU station mode. Included inside rtl_radio; the complete register
// and descriptor definitions, original notices, and hardware sequences are
// pinned under upstream/. This path is independent of passive scanning.
r92c_rom board_cal{};
struct StationState {
    enum Phase { idle, selected, auth_sent, assoc_sent, linked, keyed, fault } phase = idle;
    uint8_t ap[6]{}, tx_rate = 0;
    uint8_t ssid[32]{};
    size_t ssid_len = 0;
    rtl_station_write_bulk write = nullptr;
    void *ctx = nullptr;
    uint8_t tk[16]{}, gtk[16]{}, gtk_id = 0;
    uint64_t tx_pn = 0, rx_pair[17]{}, rx_group[17]{};
} station;

static uint16_t read_le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | uint16_t(p[1]) << 8);
}
static uint32_t read_le32(const uint8_t *p) {
    return uint32_t(read_le16(p)) | uint32_t(read_le16(p + 2)) << 16;
}
static void require_station(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

#include "station_power.inc"

void clear_station() {
    // The driver briefly retains TK/GTK only for safe reinstallation checks.
    auto *memory = reinterpret_cast<volatile unsigned char *>(&station);
    for (size_t i = 0; i < sizeof(station); ++i) memory[i] = 0;
    station = StationState{};
}

void prepare_station(const uint8_t *bssid, unsigned channel, unsigned rate,
                     rtl_station_write_bulk write, void *ctx, uint8_t *own,
                     const uint8_t *ssid, size_t ssid_len) {
    require_station(active && station.phase == StationState::idle && bssid && own && write,
        "station requires an active, unassigned radio and transport");
    require_station(ssid && ssid_len >= 1 && ssid_len <= sizeof(station.ssid),
        "exact SSID must contain 1 through 32 bytes");
    static constexpr uint8_t zero[6]{};
    require_station(!(bssid[0] & 1) && std::memcmp(bssid, zero, 6) &&
        !(board_cal.macaddr[0] & 1) && std::memcmp(board_cal.macaddr, zero, 6) &&
        std::memcmp(bssid, board_cal.macaddr, 6), "invalid station or AP identity");
    require_station(channel >= 1 && channel <= 13, "invalid 2.4 GHz channel");
    // Basic rates are advertised in 500 kb/s units; descriptor index is the
    // position in this legacy hardware rate table. Use AP's selected basic rate.
    static constexpr uint8_t legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    auto chosen = std::find(std::begin(legacy), std::end(legacy), rate);
    require_station(chosen != std::end(legacy), "unsupported AP basic rate");
    unsigned group = channel <= 3 ? 0 : channel <= 9 ? 1 : 2;
    for (unsigned chain = 0; chain < 2; ++chain)
        require_station(board_cal.cck_tx_pwr[chain][group] <= R92C_MAX_TX_PWR &&
            board_cal.ht40_1s_tx_pwr[chain][group] <= R92C_MAX_TX_PWR, "invalid board TX calibration");
    station.phase = StationState::fault; // a partial transition requires stop
    tune(channel);
    station_tx_power(0, channel);
    station_tx_power(1, channel);
    std::memcpy(station.ap, bssid, 6);
    std::memcpy(station.ssid, ssid, ssid_len);
    station.ssid_len = ssid_len;
    std::memcpy(own, board_cal.macaddr, 6);
    station.tx_rate = static_cast<uint8_t>(chosen - legacy);
    station.write = write;
    station.ctx = ctx;

    w32(R92C_BSSID, read_le32(bssid));
    w16(R92C_BSSID + 4, read_le16(bssid + 4));
    // Pre-association in rtwn_newstate(SCAN/AUTH): no hardware BSSID checks
    // until RUN. The station still checks the exact AP and own destination.
    w32(R92C_RCR, r32(R92C_RCR) & ~(R92C_RCR_AAP | R92C_RCR_CBSSID_BCN | R92C_RCR_CBSSID_DATA));
    // Association response, disassociation, authentication and deauthentication.
    w16(R92C_RXFLTMAP0, (1u << 1) | (1u << 10) | (1u << 11) | (1u << 12));
    w16(R92C_RXFLTMAP2, 0);
    w32(R92C_RRSR, RW(r32(R92C_RRSR), R92C_RRSR_RATE_BITMAP, (1u << station.tx_rate)));
    w16(R92C_RL, SM(R92C_RL_SRL, 7u) | SM(R92C_RL_LRL, 7u));
    // USB defaults from OpenBSD rtwn_edca_init. The per-AC EDCA register
    // table is already loaded by rtl8192cu_mac during radio startup.
    for (uint16_t reg : {R92C_SPEC_SIFS, R92C_MAC_SPEC_SIFS, R92C_SIFS_CCK,
                         R92C_SIFS_OFDM, R92C_RESP_SIFS_CCK, R92C_RESP_SIFS_OFDM})
        w16(reg, 0x100a);
    w32(R92C_EDCA_RANDOM_GEN, arc4random());
    w8(R92C_ACKTO, 0x40);
    w8(R92C_HWSEQ_CTRL, 0xff);
    for (unsigned chain = 0; chain < 2; ++chain) {
        auto reg = R92C_OFDM0_AGCCORE1(chain);
        w32(reg, RW(r32(reg), R92C_OFDM0_AGCCORE1_GAIN, 0x32u));
    }
    w32(R92C_CAMCMD, R92C_CAMCMD_POLLING | R92C_CAMCMD_CLR);
    wait_cam();
    // Unpause the chip scheduler for station operation. A bit mask that
    // pauses "beacon/high/management" still gates the management queue even
    // when the individual management bit is clear.
    w8(R92C_TXPAUSE, 0);
    station.phase = StationState::selected;
}

void submit_station(const uint8_t *mpdu, size_t size, bool data, bool aes) {
    require_station(size <= 2352, "oversize station frame");
    constexpr size_t header = sizeof(r92c_tx_desc_usb);
    static_assert(header == 32);
    uint8_t packet[header + 2352]{};
    r92c_tx_desc_usb desc{};
    desc.txdw0 = SM(R92C_TXDW0_PKTLEN, size) | SM(R92C_TXDW0_OFFSET, header) |
        R92C_TXDW0_OWN | R92C_TXDW0_FSG | R92C_TXDW0_LSG;
    auto queue = data ? R92C_TXDW1_QSEL_BE : R92C_TXDW1_QSEL_MGNT;
    auto raid = station.tx_rate < 4 ? R92C_RAID_11B : R92C_RAID_11BG;
    desc.txdw1 = SM(R92C_TXDW1_QSEL, queue) | SM(R92C_TXDW1_RAID, raid);
    if (data) desc.txdw1 |= R92C_TXDW1_AGGBK;
    if (aes) desc.txdw1 |= SM(R92C_TXDW1_CIPHER, R92C_TXDW1_CIPHER_AES);
    desc.txdseq = R92C_TXDW3_HWSEQEN;
    desc.txdw4 = R92C_TXDW4_HWSEQ | R92C_TXDW4_DRVRATE;
    desc.txdw5 = SM(R92C_TXDW5_DATARATE, station.tx_rate) | R92C_TXDW5_RETRY_LIMIT_ENABLE |
        SM(R92C_TXDW5_DATA_RETRY_LIMIT, 7);
    std::memcpy(packet, &desc, header);
    uint16_t checksum = 0;
    for (unsigned i = 0; i < header; i += 2) checksum ^= read_le16(packet + i);
    packet[28] = uint8_t(checksum);
    packet[29] = uint8_t(checksum >> 8);
    std::memcpy(packet + header, mpdu, size);
    int result = station.write(station.ctx, data ? 1 : 0, packet, header + size);
    require_station(result == 0, "station USB bulk write failed");
}

void check_header(const uint8_t *p, size_t n, uint8_t subtype, uint8_t flags) {
    require_station(p && n >= 24 && p[0] == subtype && p[1] == flags &&
        (p[22] & 15) == 0 && !std::memcmp(p + 4, station.ap, 6) &&
        !std::memcmp(p + 10, board_cal.macaddr, 6) && !std::memcmp(p + 16, station.ap, 6),
        "frame does not match station/AP session");
}

void send_auth(const uint8_t *p, size_t n) {
    require_station(station.phase == StationState::selected || station.phase == StationState::auth_sent,
        "unexpected authentication request");
    check_header(p, n, 0xb0, 0);
    require_station(n == 30 && read_le16(p + 24) == 0 && read_le16(p + 26) == 1 &&
        read_le16(p + 28) == 0, "invalid open-system authentication request");
    submit_station(p, n, false, false);
    station.phase = StationState::auth_sent;
}
void send_assoc(const uint8_t *p, size_t n) {
    require_station(station.phase == StationState::auth_sent || station.phase == StationState::assoc_sent,
        "unexpected association request");
    check_header(p, n, 0, 0);
    require_station(n >= 28 && n <= 256 && (read_le16(p+24) & 0x13) == 0x11 &&
        read_le16(p + 26) != 0, "invalid association request");
    bool ssid = false, rates = false, rsn = false;
    static constexpr uint8_t psk_ccmp[] = {1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2};
    for (size_t i = 28; i < n;) {
        require_station(n-i >= 2 && size_t(p[i+1]) <= n-i-2, "truncated association element");
        auto type = p[i], length = p[i+1];
        const uint8_t *value = p + i + 2;
        if (type == 0) {
            require_station(!ssid && length == station.ssid_len && !std::memcmp(value,station.ssid,length),
                "SSID differs from selected network");
            ssid = true;
        } else if (type == 48) {
            require_station(!rsn && (length == 18 || length == 20) &&
                !std::memcmp(value, psk_ccmp, sizeof(psk_ccmp)), "requires WPA2-PSK/CCMP");
            rsn = true;
        } else if (type == 1 || type == 50) {
            require_station(length && length <= 16, "invalid supported rates element");
            if (type == 1) rates = true;
        } else throw std::runtime_error("unexpected association element");
        i += length + 2;
    }
    require_station(ssid && rates && rsn, "incomplete WPA2 association request");
    submit_station(p, n, false, false);
    station.phase = StationState::assoc_sent;
}

void station_link(unsigned aid, unsigned interval) {
    require_station(station.phase == StationState::assoc_sent && aid >= 1 && aid <= 2007 &&
        interval >= 1 && interval <= 65535, "invalid association result");
    w8(R92C_MSR, (r8(R92C_MSR) & ~R92C_MSR_NETTYPE_MASK) | R92C_MSR_NETTYPE_INFRA);
    // REG_BCN_PSR_RPT AID field, see rtl92cu_set_hw_reg(HW_VAR_AID).
    constexpr uint16_t aid_reg = 0x06a8;
    w16(aid_reg, (r16(aid_reg) & 0xc000) | aid);
    w16(R92C_BCN_INTERVAL, interval);
    w32(R92C_RCR, r32(R92C_RCR) | R92C_RCR_CBSSID_BCN | R92C_RCR_CBSSID_DATA);
    w8(R92C_BCN_CTRL, r8(R92C_BCN_CTRL) & ~R92C_BCN_CTRL_DIS_TSF_UDT0);
    w16(R92C_RXFLTMAP2, (1u << 0) | (1u << 8)); // legacy/QoS data
    station.phase = StationState::linked;
}

static constexpr uint8_t snap_prefix[] = {0xaa,0xaa,3,0,0,0};
void send_eapol(const uint8_t *p, size_t n) {
    require_station(station.phase == StationState::linked || station.phase == StationState::keyed,
        "association required for EAPOL");
    check_header(p, n, 8, 1);
    require_station(n >= 36 && n <= 2048 && !std::memcmp(p+24, snap_prefix, 6) &&
        p[30] == 0x88 && p[31] == 0x8e && p[33] == 3 && (size_t(p[34])*256 + p[35]) == n-36,
        "expected complete EAPOL-Key frame");
    submit_station(p, n, true, false);
}

void wait_cam() {
    for (unsigned i = 0; i < 100; ++i) {
        if (!(r32(R92C_CAMCMD) & R92C_CAMCMD_POLLING)) return;
        delay_us(5);
    }
    throw std::runtime_error("hardware key memory did not become ready");
}
void cam_word(unsigned word, uint32_t data) {
    w32(R92C_CAMWRITE, data);
    w32(R92C_CAMCMD, R92C_CAMCMD_POLLING | R92C_CAMCMD_WRITE | word);
    wait_cam();
}
void program_key(unsigned slot, unsigned id, const uint8_t *peer, const uint8_t *key) {
    cam_word(R92C_CAM_CTL0(slot), 0);
    for (unsigned i = 0; i < 4; ++i) cam_word(R92C_CAM_KEY(slot, i), read_le32(key + i*4));
    cam_word(R92C_CAM_CTL1(slot), read_le32(peer + 2));
    cam_word(R92C_CAM_CTL0(slot), SM(R92C_CAM_MACLO, uint32_t(read_le16(peer))) |
        R92C_CAM_VALID | SM(R92C_CAM_ALGO, R92C_CAM_ALGO_AES) | SM(R92C_CAM_KEYID, id));
}
void station_keys(const uint8_t *tk, unsigned id, const uint8_t *gtk, uint64_t rsc) {
    const bool installed = station.phase == StationState::keyed;
    require_station((station.phase == StationState::linked || installed) && tk && gtk && id <= 3 &&
        rsc < (uint64_t(1) << 48), "invalid station key installation");
    const bool new_pair = !installed || std::memcmp(station.tk, tk, 16);
    const bool new_group = !installed || std::memcmp(station.gtk, gtk, 16);
    // A peer may retransmit message 3. Preserve PNs and skip CAM writes if
    // the material is unchanged. Never roll back any receive replay counter.
    if (!new_pair && !new_group && id == station.gtk_id) return;
    station.phase = StationState::fault;
    if (new_pair) {
        program_key(4, 0, station.ap, tk);
        std::memcpy(station.tk, tk, 16);
        station.tx_pn = 0;
        std::fill(std::begin(station.rx_pair), std::end(station.rx_pair), 0);
    }
    if (new_group || id != station.gtk_id) {
        static constexpr uint8_t zero[6]{};
        if (installed && id != station.gtk_id) cam_word(R92C_CAM_CTL0(station.gtk_id), 0);
        program_key(id, id, zero, gtk);
        std::memcpy(station.gtk, gtk, 16);
        if (new_group) std::fill(std::begin(station.rx_group), std::end(station.rx_group), rsc);
        station.gtk_id = static_cast<uint8_t>(id);
    }
    w16(R92C_SECCFG, R92C_SECCFG_TXUCKEY_DEF | R92C_SECCFG_RXUCKEY_DEF |
        R92C_SECCFG_TXENC_ENA | R92C_SECCFG_RXENC_ENA | R92C_SECCFG_TXBCKEY_DEF | R92C_SECCFG_RXBCKEY_DEF);
    station.phase = StationState::keyed;
}
void send_ethernet(const uint8_t *eth, size_t n) {
    require_station(station.phase == StationState::keyed && eth && n >= 14 && n <= 1514 &&
        !std::memcmp(eth+6, board_cal.macaddr, 6) && ((unsigned(eth[12]) << 8) | eth[13]) >= 0x600,
        "protected Ethernet requires installed keys and own source address");
    require_station(station.tx_pn < ((uint64_t(1) << 48)-1), "CCMP transmit counter exhausted");
    uint64_t pn = ++station.tx_pn; // reserve before USB I/O, even on a failed transfer
    uint8_t frame[24 + 8 + 8 + 1500]{};
    frame[0] = 8;
    frame[1] = 0x41;
    std::memcpy(frame + 4, station.ap, 6);
    std::memcpy(frame + 10, board_cal.macaddr, 6);
    std::memcpy(frame + 16, eth, 6);
    frame[24] = uint8_t(pn); frame[25] = uint8_t(pn >> 8);
    frame[27] = 0x20;
    for (unsigned i = 2; i < 6; ++i) frame[26+i] = uint8_t(pn >> (8*i));
    std::memcpy(frame + 32, snap_prefix, 6);
    std::memcpy(frame + 38, eth+12, n-12);
    submit_station(frame, n+26, true, true);
}

bool receive_ethernet(uint32_t dw0, const uint8_t *p, size_t n,
                      uint8_t *eth, size_t cap, size_t *written) {
    require_station(written && eth && p, "invalid station receive buffer");
    *written = 0;
    if (station.phase != StationState::keyed || n < 48 || n > 1600 ||
        (p[0] != 8 && p[0] != 0x88) || (p[1] & 0xc7) != 0x42 ||
        (p[22] & 0xf) || std::memcmp(p+10, station.ap, 6)) return false;
    // Linux rtl92cu_rx_query_desc: DW0 bit 27 set means software decryption
    // required. HW integrity and AES cipher status are required for plaintext.
    constexpr uint32_t sw_decrypt = uint32_t(1) << 27;
    if ((dw0 & (R92C_RXDW0_CRCERR | R92C_RXDW0_ICVERR | sw_decrypt)) ||
        ((dw0 >> 20) & 7) != 4) return false; // rtl8xxxu RX_DESC_ENC_AES
    bool group = (p[4] & 1) != 0;
    if (!group && std::memcmp(p+4, board_cal.macaddr, 6)) return false;
    size_t h = p[0] == 0x88 ? 26 : 24;
    if (n < h + 24 || (h == 26 && (p[24] & 0x80))) return false; // A-MSDU not negotiated
    unsigned tid = h == 26 ? (p[24] & 15) : 16;
    const uint8_t *iv = p + h;
    if (iv[2] || (iv[3] & 0x3f) != 0x20 || (iv[3] >> 6) != (group ? station.gtk_id : 0)) return false;
    uint64_t pn = read_le16(iv) | (uint64_t(read_le32(iv+4)) << 16);
    auto &last = group ? station.rx_group[tid] : station.rx_pair[tid];
    const uint8_t *llc = iv + 8;
    if (pn <= last || std::memcmp(llc, snap_prefix, 6)) return false;
    size_t eth_len = n - h - 8 - 8 - 6 + 12; // strip IV, MIC and LLC prefix
    if (eth_len > cap || eth_len > 1514) return false;
    std::memcpy(eth, p + 4, 6);
    std::memcpy(eth + 6, p + 16, 6);
    std::memcpy(eth + 12, llc + 6, eth_len - 12);
    last = pn;
    *written = eth_len;
    return true;
}
