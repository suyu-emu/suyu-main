// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The standalone recompiler only needs mbedTLS's dependency-free SHA-256 implementation.
#define MBEDTLS_SHA256_C
#ifdef _WIN32
// This mbedTLS branch requires its platform switch on Windows even when the only selected
// primitive merely uses platform_util.c for secure context clearing.
#define MBEDTLS_PLATFORM_C
#endif

#include <mbedtls/check_config.h>
