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
#include <sbi/sbi_string.h>
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

/* WorldGuard Linux S-mode test scratch addresses (must be DRAM) */
#define WGTEST_W2_PA	0xBFFF0000ULL
#define WGTEST_W3_PA	0xAFFF0000ULL
#define WGTEST_W2_VAL	0x1122334455667788ULL
#define WGTEST_W3_VAL	0x8877665544332211ULL

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
static bool worldguard_cpu_has_ext(const void *fdt, int cpuoff, const char *ext)
{
	const char *list;
	const char *isa;
	int len;
	size_t ext_len;
	size_t isa_len;
	int i;

	list = fdt_getprop(fdt, cpuoff, "riscv,isa-extensions", &len);
	if (list && fdt_stringlist_contains(list, len, ext))
		return true;

	isa = fdt_getprop(fdt, cpuoff, "riscv,isa", &len);
	if (!isa || len <= 0)
		return false;

	ext_len = sbi_strlen(ext);
	isa_len = sbi_strnlen(isa, len);
	if (ext_len == 0 || isa_len < ext_len)
		return false;

	for (i = 0; i + ext_len <= isa_len; i++) {
		if (!sbi_strncmp(isa + i, ext, ext_len))
			return true;
	}

	return false;
}

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

	/* Check if SPL already initialized WorldGuard (T036-T038) */
	val = fdt_getprop(fdt, nodeoff, "spl-initialized", &len);
	if (val && len >= sizeof(fdt32_t) && fdt32_to_cpu(*val) == 1) {
		sbi_printf("WorldGuard: Already initialized by SPL\n");

		/* Read SPL configuration */
		val = fdt_getprop(fdt, nodeoff, "mlwid", &len);
		if (val && len >= sizeof(fdt32_t)) {
			wg_info.trustedwid = fdt32_to_cpu(*val);
		}

		val = fdt_getprop(fdt, nodeoff, "mwiddeleg", &len);
		if (val && len >= sizeof(fdt32_t)) {
			wg_info.mwiddeleg = fdt32_to_cpu(*val);
		}

		sbi_printf("WorldGuard: SPL config - mlwid=%u, mwiddeleg=0x%x\n",
			   wg_info.trustedwid, wg_info.mwiddeleg);

		wg_info.enabled = true;
		return SBI_EALREADY;
	}

	val = fdt_getprop(fdt, nodeoff, "status", &len);
	if (val && len > 0 && !fdt_stringlist_contains((const char *)val, len, "okay"))
		return SBI_ENODEV;

	if (fdt_node_offset_by_compatible(fdt, -1, "riscv") < 0)
		return SBI_ENODEV;

	if (!worldguard_cpu_has_ext(fdt, fdt_node_offset_by_compatible(fdt, -1, "riscv"), "smwg"))
		return SBI_ENODEV;

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
static void wgchecker_program_slot(int slot_num, u64 end_addr, u32 perm, u32 cfg)
{
	unsigned long slot_addr = WGCHECKER_SLOT_ADDR(slot_num);
	unsigned long slot_perm = WGCHECKER_SLOT_PERM(slot_num);
	unsigned long slot_cfg = WGCHECKER_SLOT_CFG(slot_num);
	u64 slot_end = WGCHECKER_SLOT_ADDR_FMT(end_addr);

	wgchecker_write64(slot_addr, slot_end);
	wgchecker_write64(slot_perm, (u64)perm);
	wgchecker_write32(slot_cfg, cfg);

	sbi_printf("  slot[%d]: end=0x%lx slot_end=0x%lx perm=0x%x cfg=0x%x\n",
		   slot_num, (unsigned long)end_addr, (unsigned long)slot_end, perm, cfg);
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
	int ret;

	/* Only initialize on cold boot hart */
	if (scratch == NULL)
		return SBI_EINVAL;

	/* Parse Device Tree configuration */
	ret = worldguard_parse_fdt();
	if (ret == SBI_ENODEV) {
		/* No WorldGuard - silent skip */
		return 0;
	}
	
	if (ret == SBI_EALREADY) {
		sbi_printf("WorldGuard: Skipping CSR programming (SPL initialized)\n");
		return 0;
	}
	
	if (ret != 0) {
		sbi_printf("WorldGuard: FDT parsing failed: %d\n", ret);
		return ret;
	}

	sbi_printf("WorldGuard: detected, nworlds=%u trustedwid=%u mwiddeleg=0x%x\n",
		   wg_info.nworlds, wg_info.trustedwid, wg_info.mwiddeleg);

	/* Initialize WorldGuard CSRs */
	csr_write(CSR_MLWID, wg_info.smodewid);
	csr_write(CSR_MWIDDELEG, wg_info.mwiddeleg);
	sbi_printf("WorldGuard: CSRs - mlwid(s-mode)=%u mwiddeleg=0x%x\n",
		   wg_info.smodewid, wg_info.mwiddeleg);

	/* Parse and program wgChecker slots */
	ret = wgchecker_parse_and_program_fdt();
	if (ret == SBI_ENODEV) {
		/* No wgChecker - not an error */
		sbi_printf("WorldGuard: No wgChecker configuration\n");
	} else if (ret != 0) {
		sbi_printf("WorldGuard: wgChecker programming failed: %d\n", ret);
		return ret;
	}

	/*
	 * Test scratch: write patterns as trusted WID (M-mode).
	 * Linux S-mode (WID=mlwid) should observe WG enforcement when accessing
	 * these addresses.
	 */
	*(volatile u64 *)WGTEST_W2_PA = WGTEST_W2_VAL;
	*(volatile u64 *)WGTEST_W3_PA = WGTEST_W3_VAL;
	sbi_printf("WorldGuard: test scratch written w2=0x%llx @0x%llx w3=0x%llx @0x%llx\n",
		   (unsigned long long)WGTEST_W2_VAL, (unsigned long long)WGTEST_W2_PA,
		   (unsigned long long)WGTEST_W3_VAL, (unsigned long long)WGTEST_W3_PA);

	/* Mark as enabled */
	wg_info.enabled = true;
	sbi_printf("WorldGuard: initialization complete\n");

	return 0;
}
