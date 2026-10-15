// RTLRegs.h
// PLA and USB MCU register address constants for the RTL815x USB Ethernet family.
// Source: Linux r8152.h (Realtek open-source vendor driver).

#pragma once
#include <stdint.h>

// OCP USB control transfer bRequest values
#define RTL_REQ_GET_REGS    0x05   // bmRequestType=0xC0
#define RTL_REQ_SET_REGS    0x05   // bmRequestType=0x40

// MCU domain selectors (wIndex in USB control transfer)
#define MCU_TYPE_PLA        0x0100
#define MCU_TYPE_USB        0x0000

// ---------------------------------------------------------------------------
// PLA (Packet/MAC) registers

#define PLA_IDR             0xC000  // MAC address (6 bytes)
#define PLA_RCR             0xC010  // RX control
#define PLA_RMS             0xC016  // RX max packet size
#define PLA_CR              0xC016  // alias: MAC control register
#define PLA_TCR0            0xC018  // TX control 0
#define PLA_TCR1            0xC01A  // TX control 1
#define PLA_MTPS            0xC01E  // max TX packet size
#define PLA_MISC_1          0xC0A6  // misc control 1 (RXDY_EN bit)
#define PLA_OOB_CTRL        0xC08F  // OOB control

// PLA_CR bits
#define CR_RST              0x10
#define CR_RE               0x08    // RX enable
#define CR_TE               0x04    // TX enable

// PLA_RCR bits
#define RCR_AAP             0x0001  // accept all physical
#define RCR_APM             0x0002  // accept physical match
#define RCR_AM              0x0004  // accept multicast
#define RCR_AB              0x0008  // accept broadcast
#define RCR_ACPT_ALL        (RCR_AAP | RCR_APM | RCR_AM | RCR_AB)

// PLA_MISC_1 bits
#define RXDY_EN             0x0001

// ---------------------------------------------------------------------------
// USB MCU registers

#define USB_USB_CTRL        0xD406
#define USB_PM_CTRL_STATUS  0xD432
#define USB_RX_BUF_TH       0xD44C
#define USB_TX_AGG          0xD42A
#define USB_U2P3_CTRL       0xD580
#define USB_U1U2_TIMER      0xD472
#define USB_LPM_CONFIG      0xD41A
#define USB_UPS_FLAGS       0xD484
#define USB_WDT6_CTRL       0xE428
#define USB_BP_EN           0xD43E  // breakpoint enable

// USB_RX_BUF_TH — receive buffer threshold (default 0x7A120 = 500KB)
#define RX_BUF_THR          0x7A120

// USB_TX_AGG bits / values
#define TX_AGG_MAX_THRESHOLD    0x03

// ---------------------------------------------------------------------------
// PHY / OCP MII access

#define OCP_BASE_MII        0xA400  // OCP base address for MII register access
#define OCP_REG_PHYSTATUS   0xA420  // PHY status register (via PLA OCP)

// PHYSTATUS bits
#define PHYSTATUS_FDX       0x01
#define PHYSTATUS_SPEED_MASK    0x1C
#define PHYSTATUS_SPEED_1000    0x10
#define PHYSTATUS_SPEED_100     0x08
#define PHYSTATUS_SPEED_10      0x04
#define PHYSTATUS_LINK      0x02
