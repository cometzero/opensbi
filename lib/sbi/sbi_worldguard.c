/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 WorldGuard Support for OpenSBI
 *
 * RISC-V WorldGuard initialization and management
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_worldguard.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_error.h>

/* Global WorldGuard state */
static struct sbi_worldguard_info wg_info = {
	.enabled = false,
	.nworlds = WORLDGUARD_DEFAULT_NWORLDS,
	.trustedwid = WORLDGUARD_DEFAULT_TRUSTEDWID,
	.smodewid = WORLDGUARD_DEFAULT_SMODEWID,
	.mwiddeleg = WORLDGUARD_DEFAULT_MWIDDELEG,
};

bool sbi_worldguard_enabled(void)
{
	return wg_info.enabled;
}

const struct sbi_worldguard_info *sbi_worldguard_get_info(void)
{
	return &wg_info;
}

/**
 * Try to read WorldGuard CSR to detect if extension is present
 * Returns 0 if WorldGuard is available, negative error otherwise
 */
static int worldguard_detect(void)
{
	unsigned long val;

	/*
	 * Try to read mlwid CSR. If WorldGuard extension is not present,
	 * this will cause an illegal instruction exception.
	 * For now, we assume WorldGuard is present if we reach this code.
	 * TODO: Add proper trap-based detection when DT parsing is implemented.
	 */

	/* Read current mlwid value */
	val = csr_read(CSR_MLWID);

	/* If we get here, WorldGuard CSRs are accessible */
	sbi_printf("WorldGuard: detected, current mlwid=%lu\n", val);

	return 0;
}

/**
 * Initialize WorldGuard CSRs with configured values
 * Note: slwid (0x190) is an S-mode CSR, it will be set by U-Boot/Linux
 * OpenSBI only sets M-mode CSRs: mlwid and mwiddeleg
 */
static void worldguard_init_csrs(void)
{
	/* Set M-mode WID to trusted WID */
	csr_write(CSR_MLWID, wg_info.trustedwid);

	/* Delegate WIDs to S-mode - slwid will be set by S-mode software */
	csr_write(CSR_MWIDDELEG, wg_info.mwiddeleg);
}

int sbi_worldguard_init(struct sbi_scratch *scratch, u32 cold_hartid)
{
	int rc;

	/* Only initialize on cold boot hart */
	if (scratch == NULL)
		return SBI_EINVAL;

	/* Detect WorldGuard presence */
	rc = worldguard_detect();
	if (rc) {
		/* WorldGuard not available - silent skip */
		wg_info.enabled = false;
		return 0;
	}

	/* Initialize CSRs with default/configured values */
	worldguard_init_csrs();

	wg_info.enabled = true;

	sbi_printf("WorldGuard: enabled, mlwid=%u, mwiddeleg=0x%x\n",
		   wg_info.trustedwid, wg_info.mwiddeleg);

	return 0;
}
