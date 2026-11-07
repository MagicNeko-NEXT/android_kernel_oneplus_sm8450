// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot-stage colour markers and RAM-dump breadcrumb record drawn directly
 * into the bootloader's contiguous splash framebuffer.
 */

#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/splash_markers.h>
#include <linux/types.h>

#include <asm/barrier.h>
#include <asm/early_ioremap.h>
#include <asm/pgtable-prot.h>

#define SPLASH_PHYS_ADDR	((phys_addr_t)CONFIG_OPLUS_BRINGUP_SPLASH_PHYS_ADDR)
#define SPLASH_SIZE		((size_t)CONFIG_OPLUS_BRINGUP_SPLASH_SIZE)
#define SPLASH_WIDTH		((u32)CONFIG_OPLUS_BRINGUP_SPLASH_WIDTH)
#define SPLASH_HEIGHT		((u32)CONFIG_OPLUS_BRINGUP_SPLASH_HEIGHT)
#define SPLASH_STRIDE		((u32)CONFIG_OPLUS_BRINGUP_SPLASH_STRIDE)

#define SPLASH_BAND_ROWS	60U
/*
 * arm64 NR_FIX_BTMAPS is 64 pages (256 KiB). A full 60-row band at stride 4320
 * is 259,200 bytes (> 64 pages), which would fail __early_ioremap(). Map in
 * 20-row chunks (86,400 bytes, <= 23 pages) when using early_memremap_prot().
 */
#define SPLASH_EARLY_CHUNK_ROWS	20U
#define SPLASH_MAGIC		0x4f50534dU /* "OPSM" */

struct oplus_splash_record {
	u32 magic;
	u32 stage;
	u64 jiffies;
};

#define SPLASH_RECORD_TOTAL_BYTES \
	((OPLUS_SPLASH_STAGE_MAX + 1) * sizeof(struct oplus_splash_record))

/* Distinct a8r8g8b8 colours per boot stage (1..7). */
static const u32 splash_stage_colors[OPLUS_SPLASH_STAGE_MAX + 1] = {
	[OPLUS_SPLASH_STAGE_EARLY_IOREMAP]    = 0xffff0000U, /* Stage 1: Red */
	[OPLUS_SPLASH_STAGE_SETUP_ARCH_DONE]  = 0xffff8000U, /* Stage 2: Orange */
	[OPLUS_SPLASH_STAGE_MM_CORE_DONE]     = 0xffffff00U, /* Stage 3: Yellow */
	[OPLUS_SPLASH_STAGE_REST_INIT]        = 0xff00ff00U, /* Stage 4: Green */
	[OPLUS_SPLASH_STAGE_PRE_BASIC_SETUP]  = 0xff00ffffU, /* Stage 5: Cyan */
	[OPLUS_SPLASH_STAGE_POST_BASIC_SETUP] = 0xff0040ffU, /* Stage 6: Blue */
	[OPLUS_SPLASH_STAGE_PRE_INIT_PROC]    = 0xffff00ffU, /* Stage 7: Magenta */
};

static void splash_paint_rows(void __iomem *base, u32 rows, u32 color)
{
	u32 y, x;

	for (y = 0; y < rows; y++) {
		void __iomem *row = base + (size_t)y * SPLASH_STRIDE;

		for (x = 0; x < SPLASH_WIDTH; x++)
			writel_relaxed(color, row + x * sizeof(u32));
	}
	wmb();
	dsb(sy);
}

static void splash_write_one_record(void __iomem *slot, unsigned int stage, u64 now)
{
	writel_relaxed(SPLASH_MAGIC, slot + offsetof(struct oplus_splash_record, magic));
	writel_relaxed(stage, slot + offsetof(struct oplus_splash_record, stage));
	writeq_relaxed(now, slot + offsetof(struct oplus_splash_record, jiffies));
}

static void splash_write_record(void __iomem *rec_base, unsigned int stage)
{
	u64 now = READ_ONCE(jiffies_64);

	/*
	 * Slot 0 at the start of the last row always holds the latest stage
	 * reached; slot @stage preserves that individual stage's record.
	 */
	splash_write_one_record(rec_base, stage, now);
	splash_write_one_record(rec_base + stage * sizeof(struct oplus_splash_record),
				stage, now);
	wmb();
	dsb(sy);
}

void __ref oplus_splash_mark_stage(unsigned int stage)
{
	u32 start_row, color;
	phys_addr_t band_phys, rec_phys;
	size_t band_bytes;

	if (stage < 1 || stage > OPLUS_SPLASH_STAGE_MAX)
		return;

	if (SPLASH_WIDTH == 0 || SPLASH_HEIGHT <= SPLASH_BAND_ROWS ||
	    SPLASH_WIDTH * sizeof(u32) > SPLASH_STRIDE ||
	    SPLASH_RECORD_TOTAL_BYTES > SPLASH_STRIDE ||
	    (size_t)SPLASH_HEIGHT * SPLASH_STRIDE > SPLASH_SIZE)
		return;

	start_row = (stage - 1) * SPLASH_BAND_ROWS;
	if (start_row + SPLASH_BAND_ROWS >= SPLASH_HEIGHT)
		return;

	color = splash_stage_colors[stage];
	band_phys = SPLASH_PHYS_ADDR + (phys_addr_t)start_row * SPLASH_STRIDE;
	band_bytes = (size_t)SPLASH_BAND_ROWS * SPLASH_STRIDE;
	rec_phys = SPLASH_PHYS_ADDR + (phys_addr_t)(SPLASH_HEIGHT - 1) * SPLASH_STRIDE;

	if (!slab_is_available()) {
		u32 done, chunk_rows;
		void *raw;

		for (done = 0; done < SPLASH_BAND_ROWS; done += chunk_rows) {
			phys_addr_t chunk_phys;
			size_t chunk_bytes;

			chunk_rows = min(SPLASH_EARLY_CHUNK_ROWS,
					 SPLASH_BAND_ROWS - done);
			chunk_phys = band_phys + (phys_addr_t)done * SPLASH_STRIDE;
			chunk_bytes = (size_t)chunk_rows * SPLASH_STRIDE;

			raw = early_memremap_prot(chunk_phys, chunk_bytes,
						  PROT_NORMAL_NC);
			if (!raw)
				return;
			splash_paint_rows((void __iomem *)raw, chunk_rows, color);
			early_memunmap(raw, chunk_bytes);
		}

		raw = early_memremap_prot(rec_phys, SPLASH_RECORD_TOTAL_BYTES,
					  PROT_NORMAL_NC);
		if (!raw)
			return;
		splash_write_record((void __iomem *)raw, stage);
		early_memunmap(raw, SPLASH_RECORD_TOTAL_BYTES);
	} else {
		void __iomem *va;

		va = ioremap_wc(band_phys, band_bytes);
		if (!va)
			return;
		splash_paint_rows(va, SPLASH_BAND_ROWS, color);
		iounmap(va);

		va = ioremap_wc(rec_phys, SPLASH_RECORD_TOTAL_BYTES);
		if (!va)
			return;
		splash_write_record(va, stage);
		iounmap(va);
	}
}
