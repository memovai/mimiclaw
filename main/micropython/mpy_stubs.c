#include "py/obj.h"
#include "py/runtime.h"
#include "py/objtype.h"

// Minimal stubs to satisfy optional MicroPython modules that are disabled.

STATIC const mp_rom_map_elem_t mpy_stub_globals_table[] = {
};

STATIC MP_DEFINE_CONST_DICT(mpy_stub_globals, mpy_stub_globals_table);

const mp_obj_module_t mp_module_bluetooth = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mpy_stub_globals,
};

const mp_obj_module_t mp_module_network = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mpy_stub_globals,
};

const mp_obj_module_t mp_module_espnow = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mpy_stub_globals,
};

const mp_obj_type_t machine_timer_type = {
    { &mp_type_type },
    .name = MP_QSTR_Timer,
};
