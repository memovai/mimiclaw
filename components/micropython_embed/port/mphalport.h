/*
 * Minimal HAL port header for MicroPython embed on ESP32-S3.
 * Actual HAL implementations are in main/micropython/micropython_vm.c.
 */
#pragma once

static inline void mp_hal_set_interrupt_char(int c) {
    (void)c;
}
