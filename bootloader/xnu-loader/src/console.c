#include "console.h"
#include "serial.h"
#include <stdarg.h>

/* Format once, then fan the result out to both the EFI console (while boot
 * services are alive) and the COM3 UART (works pre- and post-ExitBootServices,
 * so a serial cable captures the full boot on real hardware). */
static BOOLEAN log_screen_off;

// a graphical boot keeps the log on serial so only the logo reaches the screen
VOID log_screen_quiet(VOID) {
#if !defined(__aarch64__) && !defined(__riscv)
  log_screen_off = TRUE;
  if (ST != NULL && ST->ConOut != NULL)
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
#endif
}

static VOID log_emit(CONST CHAR16 *fmt, va_list args) {
  CHAR16 buf[512];

  VSPrint(buf, sizeof(buf), (CHAR16 *)fmt, args);

  if (!log_screen_off && ST != NULL && ST->ConOut != NULL)
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, buf);

#if !defined(__aarch64__) && !defined(__riscv)
  serial_put16(buf);
#endif
  /* AAVMF's ARM64-virt ConOut is the PL011 UART, so writing directly to the
   * UART here too duplicates every pre-ExitBootServices line. The explicit
   * serial trace path remains available after boot services are gone. */
}

VOID log_info(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_emit(fmt, args);
  va_end(args);
}

VOID log_error(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_emit(fmt, args);
  va_end(args);
}
