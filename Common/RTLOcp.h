// RTLOcp.h
// OCP (On-Chip Programming) register access helpers for RTL815x.
//
// All RTL815x chip state is accessed via USB vendor control transfers:
//   bRequest = RTL_REQ_GET_REGS (0x05) for reads  — bmRequestType=0xC0
//   bRequest = RTL_REQ_SET_REGS (0x05) for writes — bmRequestType=0x40
//   wValue   = register address
//   wIndex   = MCU domain: MCU_TYPE_PLA (0x0100) or MCU_TYPE_USB (0x0000)
//   wLength  = byte count (must be 4 for dword, 2 for word, 1 for byte —
//              note: byte access in Linux reads a dword and extracts; dword
//              transfers are more reliable)
//
// Source: Linux r8152.c set_registers / get_registers / ocp_read_* / ocp_write_*.

#pragma once
#include "RTLRegs.h"

// Note: these helpers are declared here for documentation / future shared-library
// use. In DriverKit, the actual DeviceRequest calls live in the NIC .cpp because
// they require an IOUSBHostInterface* which is not available at header inclusion
// time. Each chip's NIC implementation should include this header and implement
// its own thin wrappers or inline the pattern directly.
//
// Pattern:
//
//   // Read 4-byte dword from PLA register 0xC000
//   uint32_t val = 0;
//   DeviceRequest(0xC0, RTL_REQ_GET_REGS, 0xC000, MCU_TYPE_PLA, 4, buf, &xfr, 500);
//
//   // Write 2-byte word to PLA register 0xC010
//   uint16_t w = 0x0C00;
//   DeviceRequest(0x40, RTL_REQ_SET_REGS, 0xC010, MCU_TYPE_PLA, 2, buf, &xfr, 500);
//
// Alignment: PLA dword registers are 4-byte aligned. Word/byte accesses to
// unaligned addresses require reading the containing dword, modifying, and
// writing back (as Linux ocp_read_byte / ocp_write_byte do).
