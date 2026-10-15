// RTLDesc.h
// RX and TX descriptor structs for RTL815x USB Ethernet (Config 1 vendor mode).
// Source: Linux r8152.h rx_desc / tx_desc.
//
// RX: Hardware delivers aggregated frames in each USB bulk IN transfer.
//     Each frame is preceded by a 16-byte rx_desc. Parse from the start of
//     the transfer buffer, advancing by (sizeof(rx_desc) + frame_length)
//     rounded up to 8-byte alignment for each frame.
//
// TX: Each USB bulk OUT transfer carries one or more frames, each preceded
//     by an 8-byte tx_desc.

#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// RX descriptor (16 bytes, little-endian)

struct rtl_rx_desc {
    uint32_t opts1;   // bits[17:0] = frame length (including CRC)
                      // bit 21 = RX error
                      // bit 23 = checksum ok (if RX checksum offload enabled)
    uint32_t opts2;   // bits[15:0] = VLAN tag (if VLAN offload enabled)
    uint32_t opts3;
    uint32_t opts4;
};

// opts1 field accessors
#define RX_LEN_MASK         0x0003FFFF  // bits[17:0] = frame length
#define RX_ERR              (1u << 21)  // RX error — drop frame
#define RX_CSUM_OK          (1u << 23)  // IP/TCP/UDP checksum valid

static inline uint32_t rtl_rx_frame_len(const struct rtl_rx_desc *d)
{
    return d->opts1 & RX_LEN_MASK;
}

static inline bool rtl_rx_error(const struct rtl_rx_desc *d)
{
    return (d->opts1 & RX_ERR) != 0;
}

// ---------------------------------------------------------------------------
// TX descriptor (8 bytes, little-endian)

struct rtl_tx_desc {
    uint32_t opts1;   // bits[15:0] = frame length
                      // bit 31 = first segment (set for single-segment frames)
    uint32_t opts2;   // bits[15:0] = VLAN tag (if VLAN offload enabled)
                      // bit 17 = TX checksum offload request
};

// opts1 field helpers
#define TX_FS               (1u << 31)  // first segment
#define TX_LS               (1u << 30)  // last segment
#define TX_LEN_MASK         0x0000FFFF  // bits[15:0] = frame length

static inline uint32_t rtl_tx_opts1(uint32_t frameLen)
{
    // Single-segment frame: set FS+LS and encode length
    return TX_FS | TX_LS | (frameLen & TX_LEN_MASK);
}

// RX buffer stride: each frame in the aggregate is aligned to 8 bytes
static inline uint32_t rtl_rx_frame_stride(uint32_t frameLen)
{
    return (frameLen + 7u) & ~7u;
}
