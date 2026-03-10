#include <stdio.h>
#include "uart.h"

void uart_stdout_init(void)
{
    /* No-op: UART REPL disabled in embedded mode. */
}

int uart_stdout_tx_strn(const char *str, size_t len)
{
    if (!str || len == 0) return 0;
    return (int)fwrite(str, 1, len, stdout);
}
