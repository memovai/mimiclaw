/*
 * MicroPython port configuration for MimiClaw (ESP32-S3 embed).
 *
 * This file is used BOTH when building the embed port (to generate
 * micropython_embed.c/h) AND when compiling the generated source
 * as part of the ESP-IDF project.
 *
 * Safety: network, filesystem, hardware, and threading modules are
 * disabled to sandbox executed code.
 */
#pragma once

#include <stdint.h>

/* Feature level — includes basic standard library modules */
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_BASIC_FEATURES)

/* Type definitions for ESP32-S3 (32-bit Xtensa) */
typedef intptr_t  mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long      mp_off_t;

/* Compiler & GC (required for mp_embed_exec_str) */
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_GC               (1)

/* Scheduler — needed for KeyboardInterrupt on timeout */
#define MICROPY_ENABLE_SCHEDULER        (1)
#define MICROPY_KBD_EXCEPTION           (1)

/* Stack overflow checking */
#define MICROPY_STACKCHECK              (1)

/* ---- Enabled modules ---- */
#define MICROPY_PY_MATH                 (1)
#define MICROPY_PY_CMATH                (0)
#define MICROPY_PY_JSON                 (1)
#define MICROPY_PY_RE                   (1)
#define MICROPY_PY_COLLECTIONS          (1)
#define MICROPY_PY_COLLECTIONS_DEQUE    (1)
#define MICROPY_PY_COLLECTIONS_ORDEREDDICT (1)
#define MICROPY_PY_IO                   (1)
#define MICROPY_PY_STRUCT               (1)
#define MICROPY_PY_BINASCII             (1)
#define MICROPY_PY_RANDOM               (1)
#define MICROPY_PY_HEAPQ                (1)
#define MICROPY_PY_HASHLIB              (0)

/* Useful builtins */
#define MICROPY_PY_BUILTINS_HELP        (0)
#define MICROPY_PY_BUILTINS_INPUT       (0)

/* ---- Disabled modules (sandbox) ---- */
#define MICROPY_PY_SYS                  (0)
#define MICROPY_PY_OS                   (0)
#define MICROPY_PY_NETWORK              (0)
#define MICROPY_PY_SOCKET               (0)
#define MICROPY_PY_MACHINE              (0)
#define MICROPY_PY_SELECT               (0)
#define MICROPY_PY_THREAD               (0)

/* No filesystem access */
#define MICROPY_VFS                     (0)
#define MICROPY_READER_VFS              (0)
#define MICROPY_PY_BUILTINS_OPEN        (0)

/* ---- VM hook for timeout enforcement ---- */
extern volatile int micropython_vm_timeout_flag;

#define MICROPY_VM_HOOK_LOOP \
    do { \
        if (micropython_vm_timeout_flag) { \
            micropython_vm_timeout_flag = 0; \
            mp_sched_keyboard_interrupt(); \
        } \
    } while (0);

/* Root pointers (none needed for embed) */
#define MICROPY_PORT_ROOT_POINTERS
