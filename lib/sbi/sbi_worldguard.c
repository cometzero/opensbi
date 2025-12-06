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
#include <sbi_utils/fdt/fdt_helper.h>
#include <libfdt.h>

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
 * Parse WorldGuard configuration from Device Tree
 * Returns 0 if WorldGuard node found and parsed, negative otherwise
 */
static int worldguard_parse_fdt(void)
{
	const void *fdt = fdt_get_address();
	int nodeoff, len;
	const fdt32_t *val;

	if (!fdt)
		return SBI_ENODEV;

	/* Find worldguard node */
	nodeoff = fdt_node_offset_by_compatible(fdt, -1, "riscv,worldguard");
	if (nodeoff < 0) {
		/* No WorldGuard node - silent skip */
		return SBI_ENODEV;
	}

	/* Parse nworlds property */
	val = fdt_getprop(fdt, nodeoff, "nworlds", &len);
	if (val && len >= sizeof(fdt32_t)) {
		wg_info.nworlds = fdt32_to_cpu(*val);
	}

	/* Parse trustedwid property */
	val = fdt_getprop(fdt, nodeoff, "trustedwid", &len);
	if (val && len >= sizeof(fdt32_t)) {
		wg_info.trustedwid = fdt32_to_cpu(*val);
	}

	/* Parse mwiddeleg property */
	val = fdt_getprop(fdt, nodeoff, "mwiddeleg", &len);
	if (val && len >= sizeof(fdt32_t)) {
		wg_info.mwiddeleg = fdt32_to_cpu(*val);
	}

	/* Parse wid-assignment property (optional, 3 values: M, S, U) */
	val = fdt_getprop(fdt, nodeoff, "wid-assignment", &len);
	if (val && len >= 2 * sizeof(fdt32_t)) {
		/* Skip M-mode (index 0), get S-mode WID (index 1) */
		wg_info.smodewid = fdt32_to_cpu(val[1]);
	}

	sbi_printf("WorldGuard: FDT config - nworlds=%u, trustedwid=%u\n",
		   wg_info.nworlds, wg_info.trustedwid);

	return 0;
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
	 * TODO: Add proper trap-based detection.
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

	/* Try to parse FDT for WorldGuard configuration */
	rc = worldguard_parse_fdt();
	if (rc) {
		/* No WorldGuard DT node - use defaults or skip */
		sbi_printf("WorldGuard: No FDT node found, using defaults\n");
	}

	/* Detect WorldGuard presence by reading CSR */
	rc = worldguard_detect();
	if (rc) {
		/* WorldGuard not available - silent skip */
		wg_info.enabled = false;
		return 0;
	}

	/* Initialize CSRs with configured values */
	worldguard_init_csrs();

	wg_info.enabled = true;

	sbi_printf("WorldGuard: enabled, mlwid=%u, mwiddeleg=0x%x\n",
		   wg_info.trustedwid, wg_info.mwiddeleg);

	return 0;
}

