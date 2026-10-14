#ifndef SERIAL_H
#define SERIAL_H

#include "common.h"

/*
 * Minimal 16550 UART driver for early/bare-metal debug output.
 *
 * Targets COM3 (I/O base 0x3E8) at 115200 8N1.  Uses raw x86 port I/O, so it
 * works both before and after ExitBootServices (unlike ST->ConOut, which is
 * gone once boot services exit).  log_info/log_error mirror their formatted
 * output here so a serial cable can capture the whole boot on real hardware.
 */

/* Program the UART: 115200 baud, 8 data bits, no parity, 1 stop bit, FIFOs on. */
VOID serial_init(VOID);

/* Re-apply that programming without the banner. Call after ExitBootServices,
 * whose teardown can leave the port in a non-transmitting state. */
VOID serial_reinit(VOID);

/* Emit a NUL-terminated narrow string (used for raw byte output). */
VOID serial_puts8(CONST CHAR8 *s);

/* Emit a NUL-terminated wide string, narrowing each unit to a byte. */
VOID serial_put16(CONST CHAR16 *s);

/* Emit an unpadded uppercase hex value (no "0x" prefix). */
VOID serial_puthex(UINT64 v);

/*
 * Trace points for the post-ExitBootServices handoff, where ConOut is gone and
 * log_info() is silent. Both write directly to the UART.
 */
VOID serial_trace(CONST CHAR8 *tag, UINT64 v);
VOID serial_mark(CONST CHAR8 *tag);

#endif
