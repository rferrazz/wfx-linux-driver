// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common hwbus abstraction layer interface for WFX wireless driver
 *
 * Copyright (c) 2017-2018, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#ifndef WFX_HWBUS_H
#define WFX_HWBUS_H

#define WFX_REG_CONFIG        0x0
#define WFX_REG_CONTROL       0x1
#define WFX_REG_IN_OUT_QUEUE  0x2
#define WFX_REG_AHB_DPORT     0x3
#define WFX_REG_BASE_ADDR     0x4
#define WFX_REG_SRAM_DPORT    0x5
#define WFX_REG_SET_GEN_R_W   0x6
#define WFX_REG_FRAME_OUT     0x7

struct hwbus_ops {
	int (*copy_from_io)(void *bus_priv, unsigned int addr, void *dst, size_t count);
	int (*copy_to_io)(void *bus_priv, unsigned int addr, const void *src, size_t count);
	void (*lock)(void *bus_priv);
	void (*unlock)(void *bus_priv);
	size_t (*align_size)(void *bus_priv, size_t size);
};

#endif /* WFX_HWBUS_H */
