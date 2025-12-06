/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 WorldGuard Support for OpenSBI
 *
 * RISC-V WorldGuard CSR and MMIO definitions
 */

#ifndef __RISCV_WORLDGUARD_H__
#define __RISCV_WORLDGUARD_H__

#include <sbi/sbi_types.h>

/*
 * WorldGuard CSR addresses (RISC-V WorldGuard spec v0.4)
 */
#define CSR_MLWID       0x390   /* Machine Local World ID */
#define CSR_SLWID       0x190   /* Supervisor Local World ID */
#define CSR_MWIDDELEG   0x748   /* Machine WID Delegation */

/*
 * WorldGuard default configuration
 * These values can be overridden by Device Tree
 */
#define WORLDGUARD_DEFAULT_NWORLDS      4
#define WORLDGUARD_DEFAULT_TRUSTEDWID   3
#define WORLDGUARD_DEFAULT_SMODEWID     2
#define WORLDGUARD_DEFAULT_MWIDDELEG    0x6  /* Delegate WID 1,2 to S-mode */

/*
 * wgChecker MMIO base address (QEMU virt machine)
 */
#define WGCHECKER_BASE_ADDR     0x6000000UL
#define WGCHECKER_SIZE          0x1000

/*
 * wgChecker slot register offsets
 * Each slot: addr(8) + perm(4) + cfg(4) = 16 bytes
 */
#define WGCHECKER_SLOT_ADDR(n)  (WGCHECKER_BASE_ADDR + 0x100 + (n) * 16)
#define WGCHECKER_SLOT_PERM(n)  (WGCHECKER_BASE_ADDR + 0x100 + (n) * 16 + 8)
#define WGCHECKER_SLOT_CFG(n)   (WGCHECKER_BASE_ADDR + 0x100 + (n) * 16 + 12)

/*
 * wgChecker slot configuration bits
 */
#define WGCHECKER_CFG_A_OFF     0x0
#define WGCHECKER_CFG_A_TOR     0x1
#define WGCHECKER_CFG_A_NA4     0x2
#define WGCHECKER_CFG_A_NAPOT   0x3
#define WGCHECKER_CFG_A_MASK    0x3
#define WGCHECKER_CFG_L         (1 << 7)  /* Lock bit */

/*
 * wgChecker permission bits per World ID
 * Each world has 2 bits: [1:0]=Read/Write
 */
#define WGCHECKER_PERM_R        0x1
#define WGCHECKER_PERM_W        0x2
#define WGCHECKER_PERM_RW       0x3
#define WGCHECKER_PERM_WID(wid, perm)   ((perm) << ((wid) * 2))

struct sbi_scratch;

/**
 * WorldGuard state structure
 */
struct sbi_worldguard_info {
	bool enabled;
	u32 nworlds;
	u32 trustedwid;
	u32 smodewid;
	u32 mwiddeleg;
};

/**
 * Initialize WorldGuard support
 * @param scratch: pointer to sbi_scratch
 * @param cold_hartid: cold boot hart ID
 * @return: 0 on success, negative error code on failure
 */
int sbi_worldguard_init(struct sbi_scratch *scratch, u32 cold_hartid);

/**
 * Check if WorldGuard is enabled
 * @return: true if enabled, false otherwise
 */
bool sbi_worldguard_enabled(void);

/**
 * Get WorldGuard info
 * @return: pointer to worldguard info structure
 */
const struct sbi_worldguard_info *sbi_worldguard_get_info(void);

#endif /* __RISCV_WORLDGUARD_H__ */
