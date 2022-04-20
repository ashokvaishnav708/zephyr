/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Seppo Ingalsuo <seppo.ingalsuo@linux.intel.com>
 */

#include <stdint.h>

const int32_t src_int32_3_1_2268_5000_fir[60] = {
	-166536,
	2306339,
	-6050784,
	2678997,
	20170183,
	-56648456,
	63227591,
	26267865,
	-254983011,
	677549850,
	1351061190,
	234544530,
	-235734562,
	109710788,
	-4081183,
	-34183778,
	25146783,
	-6857929,
	-1408097,
	1421800,
	346619,
	1663348,
	-9179696,
	16157737,
	-2290482,
	-49228991,
	115231627,
	-111856179,
	-100474502,
	1096579163,
	1096579163,
	-100474502,
	-111856179,
	115231627,
	-49228991,
	-2290482,
	16157737,
	-9179696,
	1663348,
	346619,
	1421800,
	-1408097,
	-6857929,
	25146783,
	-34183778,
	-4081183,
	109710788,
	-235734562,
	234544530,
	1351061190,
	677549850,
	-254983011,
	26267865,
	63227591,
	-56648456,
	20170183,
	2678997,
	-6050784,
	2306339,
	-166536

};

struct src_stage src_int32_3_1_2268_5000 = {
	0, 1, 3, 20, 60, 1, 3, 0, 0,
	src_int32_3_1_2268_5000_fir};
