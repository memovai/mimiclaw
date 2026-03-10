#pragma once

#include "sdkconfig.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#define MIMI_CRT_BUNDLE_ATTACH esp_crt_bundle_attach
#else
#define MIMI_CRT_BUNDLE_ATTACH NULL
#endif
