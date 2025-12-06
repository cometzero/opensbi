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

/**
 * Write to wgChecker MMIO register
 */
static inline void wgchecker_write64(unsigned long addr, u64 val)
{
	*(volatile u64 *)addr = val;
}

static inline void wgchecker_write32(unsigned long addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

/**
 * Program a single wgChecker slot
 * @param slot_num: Slot number (1-based, slot 0 is implicit)
 * @param addr: End address for TOR mode
 * @param perm: Permission bits (2 bits per WID)
 * @param cfg: Configuration (TOR/NAPOT + Lock bit)
 */
static void wgchecker_program_slot(int slot_num, u64 addr, u32 perm, u32 cfg)
{
	unsigned long slot_addr = WGCHECKER_SLOT_ADDR(slot_num);
	unsigned long slot_perm = WGCHECKER_SLOT_PERM(slot_num);
	unsigned long slot_cfg = WGCHECKER_SLOT_CFG(slot_num);

	wgchecker_write64(slot_addr, addr);
	wgchecker_write32(slot_perm, perm);
	wgchecker_write32(slot_cfg, cfg);

	sbi_printf("  slot[%d]: addr=0x%lx perm=0x%x cfg=0x%x\n",
		   slot_num, (unsigned long)addr, perm, cfg);
}

/**
 * Parse and program wgChecker slots from Device Tree
 */
static int wgchecker_parse_and_program_fdt(void)
{
	const void *fdt = fdt_get_address();
	int nodeoff, len, i;
	const fdt32_t *slots;
	int num_slots;

	if (!fdt)
		return SBI_ENODEV;

	/* Find wgchecker node */
	nodeoff = fdt_node_offset_by_compatible(fdt, -1, "riscv,wgchecker");
	if (nodeoff < 0) {
		/* No wgChecker node - skip slot programming */
		return SBI_ENODEV;
	}

	/* Parse slots property: <addr_hi addr_lo size_hi size_lo perm cfg> */
	slots = fdt_getprop(fdt, nodeoff, "slots", &len);
	if (!slots || len < 6 * sizeof(fdt32_t)) {
		sbi_printf("WorldGuard: wgChecker has no valid slots\n");
		return SBI_ENODEV;
	}

	/* Each slot is 6 u32 values */
	num_slots = len / (6 * sizeof(fdt32_t));

	sbi_printf("WorldGuard: Programming %d wgChecker slots\n", num_slots);

	for (i = 0; i < num_slots; i++) {
		u64 addr, size, end_addr;
		u32 perm, cfg;
		int base = i * 6;

		/* Parse addr (64-bit) */
		addr = ((u64)fdt32_to_cpu(slots[base]) << 32) |
		       fdt32_to_cpu(slots[base + 1]);

		/* Parse size (64-bit) */
		size = ((u64)fdt32_to_cpu(slots[base + 2]) << 32) |
		       fdt32_to_cpu(slots[base + 3]);

		/* For TOR mode, end_addr = addr + size */
		end_addr = addr + size;

		/* Parse perm and cfg */
		perm = fdt32_to_cpu(slots[base + 4]);
		cfg = fdt32_to_cpu(slots[base + 5]);

		/* Program slot (1-based index) */
		wgchecker_program_slot(i + 1, end_addr, perm, cfg);
	}

	return 0;
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

	/* Program wgChecker slots if defined in FDT */
	wgchecker_parse_and_program_fdt();

	wg_info.enabled = true;

	sbi_printf("WorldGuard: enabled, mlwid=%u, mwiddeleg=0x%x\n",
		   wg_info.trustedwid, wg_info.mwiddeleg);

	return 0;
}
