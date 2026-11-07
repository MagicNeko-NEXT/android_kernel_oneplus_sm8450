/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SPLASH_MARKERS_H
#define _LINUX_SPLASH_MARKERS_H

enum oplus_splash_stage {
	OPLUS_SPLASH_STAGE_EARLY_IOREMAP = 1,
	OPLUS_SPLASH_STAGE_SETUP_ARCH_DONE = 2,
	OPLUS_SPLASH_STAGE_MM_CORE_DONE = 3,
	OPLUS_SPLASH_STAGE_REST_INIT = 4,
	OPLUS_SPLASH_STAGE_PRE_BASIC_SETUP = 5,
	OPLUS_SPLASH_STAGE_POST_BASIC_SETUP = 6,
	OPLUS_SPLASH_STAGE_PRE_INIT_PROC = 7,
	OPLUS_SPLASH_STAGE_MAX = 7,
};

#ifdef CONFIG_OPLUS_BRINGUP_SPLASH_MARKERS
void oplus_splash_mark_stage(unsigned int stage);
#else
static inline void oplus_splash_mark_stage(unsigned int stage)
{
}
#endif

#endif /* _LINUX_SPLASH_MARKERS_H */
