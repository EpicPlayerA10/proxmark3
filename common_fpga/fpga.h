//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------

#ifndef __FPGA_H
#define __FPGA_H

#include <stdbool.h>
#include <inttypes.h>

#define FPGA_BITSTREAM_FIXED_HEADER_SIZE    sizeof(bitparse_fixed_header)
#define FPGA_INTERLEAVE_SIZE                288
#if defined XC3
#define FPGA_TYPE "3s100evq100"
#define FPGA_CONFIG_SIZE                    72864L  // FPGA .bit file rounded up to next multiple of FPGA_INTERLEAVE_SIZE
#elif defined XC2S50
#define FPGA_TYPE "2s50vq144"
#define FPGA_CONFIG_SIZE                    69984L  // FPGA .bit file rounded up to next multiple of FPGA_INTERLEAVE_SIZE
#else
#define FPGA_TYPE "2s30vq100"
#define FPGA_CONFIG_SIZE                    42336L  // FPGA .bit file rounded up to next multiple of FPGA_INTERLEAVE_SIZE
#endif
#define FPGA_RING_BUFFER_BYTES              (1024 * 30)
#define FPGA_TRACE_SIZE                     3072

// definitions for multiple FPGA config files support
typedef enum {
    FPGA_BITSTREAM_UNKNOWN = 0,
    FPGA_BITSTREAM_LF = 1,
    FPGA_BITSTREAM_MIN = FPGA_BITSTREAM_LF,
    FPGA_BITSTREAM_HF,
    FPGA_BITSTREAM_HF_FELICA,
    FPGA_BITSTREAM_HF_15,
    FPGA_BITSTREAM_MAX = FPGA_BITSTREAM_HF_15,
    FPGA_CONFIG_COUNT
} FPGA_config;

typedef struct {
    const char *const versionString;
    const FPGA_config target_config;
} FPGA_VERSION_INFORMATION;

static const uint8_t bitparse_fixed_header[] = {0x00, 0x09, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x00, 0x00, 0x01};
extern const int g_fpga_bitstream_num;
extern const FPGA_VERSION_INFORMATION g_fpga_version_information[];

#endif
