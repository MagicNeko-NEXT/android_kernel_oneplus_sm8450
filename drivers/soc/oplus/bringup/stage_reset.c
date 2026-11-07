// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot-stage warm-reset bisection for early bring-up on devices without UART
 * or working display/USB at early boot.
 */

#define pr_fmt(fmt) "bringup_stage_reset: " fmt

#include <linux/arm-smccc.h>
#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/smp.h>
#include <linux/stage_reset.h>
#include <linux/string.h>
#include <linux/types.h>

#include <uapi/linux/psci.h>

#include <asm/barrier.h>

static unsigned int bringup_reset_stage __ro_after_init;
static enum arm_smccc_conduit bringup_psci_conduit __ro_after_init =
	SMCCC_CONDUIT_SMC;

static const char * const bringup_psci_compat[] __initconst = {
	"arm,psci-1.0",
	"arm,psci-0.2",
	"arm,psci",
};

static const char * const bringup_stage_names[OPLUS_BRINGUP_STAGE_MAX + 1] = {
	[OPLUS_BRINGUP_STAGE_EARLY_PARAM]      = "early_param",
	[OPLUS_BRINGUP_STAGE_POST_PSCI]        = "post_psci",
	[OPLUS_BRINGUP_STAGE_SETUP_ARCH_DONE]  = "setup_arch_done",
	[OPLUS_BRINGUP_STAGE_MM_CORE_DONE]     = "mm_core_done",
	[OPLUS_BRINGUP_STAGE_IRQ_TIME_READY]   = "irq_time_ready",
	[OPLUS_BRINGUP_STAGE_REST_INIT]        = "rest_init",
	[OPLUS_BRINGUP_STAGE_PRE_BASIC_SETUP]  = "pre_basic_setup",
	[OPLUS_BRINGUP_STAGE_POST_BASIC_SETUP] = "post_basic_setup",
	[OPLUS_BRINGUP_STAGE_PRE_INIT_PROC]    = "pre_init_proc",
};

static enum arm_smccc_conduit __init oplus_bringup_fdt_psci_conduit(void)
{
	const void *fdt = initial_boot_params;
	const char *method;
	int node = -1;
	int len;
	size_t i;

	if (!fdt)
		return SMCCC_CONDUIT_SMC;

	for (i = 0; i < ARRAY_SIZE(bringup_psci_compat); i++) {
		node = fdt_node_offset_by_compatible(fdt, -1,
						     bringup_psci_compat[i]);
		if (node >= 0)
			break;
	}
	if (node < 0)
		return SMCCC_CONDUIT_SMC;

	method = fdt_getprop(fdt, node, "method", &len);
	if (method && len > 0 && method[len - 1] == '\0' &&
	    !strcmp(method, "hvc"))
		return SMCCC_CONDUIT_HVC;

	return SMCCC_CONDUIT_SMC;
}

static int __init bringup_reset_at_setup(char *str)
{
	unsigned int stage;
	int ret;

	if (!str)
		return -EINVAL;

	ret = kstrtouint(str, 10, &stage);
	if (ret)
		return ret;

	if (stage > OPLUS_BRINGUP_STAGE_MAX) {
		pr_warn("bringup.reset_at=%u out of range (0..%u), ignoring\n",
			stage, OPLUS_BRINGUP_STAGE_MAX);
		return -EINVAL;
	}

	bringup_reset_stage = stage;
	bringup_psci_conduit = oplus_bringup_fdt_psci_conduit();
	return 0;
}
early_param("bringup.reset_at", bringup_reset_at_setup);

static unsigned long oplus_bringup_psci_call(enum arm_smccc_conduit conduit,
					     unsigned long fn,
					     unsigned long arg0,
					     unsigned long arg1,
					     unsigned long arg2)
{
	struct arm_smccc_res res;

	if (conduit == SMCCC_CONDUIT_HVC)
		arm_smccc_hvc(fn, arg0, arg1, arg2, 0, 0, 0, 0, &res);
	else
		arm_smccc_smc(fn, arg0, arg1, arg2, 0, 0, 0, 0, &res);

	return res.a0;
}

static void __noreturn oplus_bringup_raw_psci_reset(void)
{
	enum arm_smccc_conduit conduit = arm_smccc_1_1_get_conduit();
	unsigned long ret;

	if (conduit != SMCCC_CONDUIT_HVC && conduit != SMCCC_CONDUIT_SMC)
		conduit = bringup_psci_conduit;

	dsb(sy);
	isb();

	/*
	 * Query PSCI_FEATURES (0x8400000a) for 64-bit SYSTEM_RESET2 (0xc4000012)
	 * over the DT-selected conduit ("hvc" or "smc"), matching
	 * psci_init_system_reset2() in drivers/firmware/psci/psci.c.
	 */
	ret = oplus_bringup_psci_call(conduit, PSCI_1_0_FN_PSCI_FEATURES,
				      PSCI_1_1_FN64_SYSTEM_RESET2, 0, 0);
	if ((s32)ret != PSCI_RET_NOT_SUPPORTED) {
		oplus_bringup_psci_call(conduit, PSCI_1_1_FN64_SYSTEM_RESET2,
					PSCI_1_1_RESET_TYPE_SYSTEM_WARM_RESET,
					0, 0);
	}

	/*
	 * Fall back to architectural PSCI 0.2 SYSTEM_RESET (0x84000009) if
	 * SYSTEM_RESET2 is unsupported or returned an error.
	 */
	oplus_bringup_psci_call(conduit, PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0);

	for (;;) {
		wfi();
		cpu_relax();
	}
}

static void __noreturn oplus_bringup_do_reset(enum oplus_bringup_stage stage)
{
	local_irq_disable();

	/*
	 * Once psci_dt_init() has run (stage >= 2), prefer the kernel's
	 * restart handler path with REBOOT_WARM so registered handlers
	 * (such as qcom-dload-mode clearing download mode and psci_sys_reset)
	 * run first. Call do_kernel_restart() directly rather than
	 * machine_restart()/emergency_restart() because machine_restart()
	 * ends in an infinite while(1) if psci_sys_reset's SYSTEM_RESET2 call
	 * returns without falling back to SYSTEM_RESET.
	 */
	if (stage >= OPLUS_BRINGUP_STAGE_POST_PSCI) {
		reboot_mode = REBOOT_WARM;
		kmsg_dump(KMSG_DUMP_EMERG);
		if (num_online_cpus() > 1)
			smp_send_stop();
		do_kernel_restart(NULL);
	}

	oplus_bringup_raw_psci_reset();
}

void oplus_bringup_stage_reset(enum oplus_bringup_stage stage)
{
	if (stage < OPLUS_BRINGUP_STAGE_EARLY_PARAM ||
	    stage > OPLUS_BRINGUP_STAGE_MAX)
		return;

	if (bringup_reset_stage == stage) {
		pr_emerg("stage %u (%s) reached, triggering warm reset (bringup.reset_at=%u)\n",
			 stage, bringup_stage_names[stage],
			 bringup_reset_stage);
		oplus_bringup_do_reset(stage);
	}

	pr_emerg("stage %u (%s) reached (bringup.reset_at=%u)\n",
		 stage, bringup_stage_names[stage], bringup_reset_stage);
}
