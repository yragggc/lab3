/*
 * Copyright (C) 2005 - 2008 ServerEngines
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.  The full GNU General
 * Public License is included in this distribution in the file called COPYING.
 *
 * Contact Information:
 * linux-drivers@serverengines.com
 *
 * ServerEngines
 * 209 N. Fair Oaks Ave
 * Sunnyvale, CA 94085
 */
/*
 * Autogenerated by srcgen version: 0127
 */
#ifndef __ep_amap_h__
#define __ep_amap_h__

/* General Control and Status Register. */
struct BE_EP_CONTROL_CSR_AMAP {
	u8 m0_RxPbuf;	/* DWORD 0 */
	u8 m1_RxPbuf;	/* DWORD 0 */
	u8 m2_RxPbuf;	/* DWORD 0 */
	u8 ff_en;		/* DWORD 0 */
	u8 rsvd0[27];	/* DWORD 0 */
	u8 CPU_reset;	/* DWORD 0 */
} __packed;
struct EP_CONTROL_CSR_AMAP {
	u32 dw[1];
};

/* Semaphore Register. */
struct BE_EP_SEMAPHORE_CSR_AMAP {
	u8 value[32];	/* DWORD 0 */
} __packed;
struct EP_SEMAPHORE_CSR_AMAP {
	u32 dw[1];
};

/* Embedded Processor Specific Registers. */
struct BE_EP_CSRMAP_AMAP {
	struct BE_EP_CONTROL_CSR_AMAP ep_control;
	u8 rsvd0[32];	/* DWORD 1 */
	u8 rsvd1[32];	/* DWORD 2 */
	u8 rsvd2[32];	/* DWORD 3 */
	u8 rsvd3[32];	/* DWORD 4 */
	u8 rsvd4[32];	/* DWORD 5 */
	u8 rsvd5[8][128];	/* DWORD 6 */
	u8 rsvd6[32];	/* DWORD 38 */
	u8 rsvd7[32];	/* DWORD 39 */
	u8 rsvd8[32];	/* DWORD 40 */
	u8 rsvd9[32];	/* DWORD 41 */
	u8 rsvd10[32];	/* DWORD 42 */
	struct BE_EP_SEMAPHORE_CSR_AMAP ep_semaphore;
	u8 rsvd11[32];	/* DWORD 44 */
	u8 rsvd12[19][32];	/* DWORD 45 */
} __packed;
struct EP_CSRMAP_AMAP {
	u32 dw[64];
};

#endif /* __ep_amap_h__ */
