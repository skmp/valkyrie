/* 
 * Valkyrie
 * Copyright (C) 2011, Stefano Teso
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/*
 * IRQs
 * ====
 *
 * IRQ priorities are encoded in sh4.intc.irqs. Whenever an external IRQ
 * or on-chip IRQ should be raised or cleared, call sh4_set_irq_state.
 * It will update the sh4.intc.irqs table with the proper state.
 *
 * The interrupt priorities are either fixed (for external IRQs an
 * exceptions) or decided by the INTC settings; sh4_ireg_put will make
 * sure to update the sh4.intc.irqs priorities according to the INTC
 * configuration.
 *
 * Finally, 
 */

/* TODO: MMU */
/* TODO: propagate get/put and instruction errors to the main loop */
/* TODO: implement exceptions; this is really needed only with an MMU */
/* TODO: handle FP exceptions and rounding mode */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "vk/core.h"
#include "vk/cpu.h"

#include "sh4.h"
#include "sh4-ireg.h"

/* Helper Macros */

#define _RN	((inst >> 8) & 15)
#define _RM	((inst >> 4) & 15)
#define _UIMM8	((uint32_t)(uint8_t) inst)
#define _SIMM8	((int32_t)(int8_t) inst)
#define _SIMM12 ((int32_t)((inst & 0xFFF) | ((inst & 0x800) ? 0xFFFFF000 : 0)))

#define PC	ctx->pc
#define PR	ctx->pr
#define SR	ctx->sr
#define MAC	ctx->mac.full
#define MACH	ctx->mac.field.hi
#define MACL	ctx->mac.field.lo
#define GBR	ctx->gbr
#define VBR	ctx->vbr
#define SSR	ctx->ssr
#define SPC	ctx->spc
#define DBR	ctx->dbr
#define SGR	ctx->sgr

#define R(n_)	ctx->r[n_]
#define R0	R(0)
#define RN	R(_RN)
#define RM	R(_RM)
#define SP	R(15)

#define RBANK(n_) ctx->rbank[n_]

#define T	SR.bit.t
#define S	SR.bit.s
#define Q	SR.bit.q
#define M	SR.bit.m

#define FPSCR	ctx->fpscr
#define FPUL	ctx->fpul

#define FR(n_)	ctx->f.f[n_]
#define DR(n_)	ctx->f.d[(n_)/2]

#define XF(n_)	ctx->x.f[n_]
#define XD(n_)	ctx->x.d[(n_)/2]

#define FRN	FR(_RN)
#define FRM	FR(_RM)

#define DRN	DR(_RN)
#define DRM	DR(_RM)

#define XDN	XD(_RN)
#define XDM	XD(_RM)

#define IS_PRIVILEGED \
	(SR.bit.md == 1)

#define IS_FP_ENABLED \
	(SR.bit.fd == 0)

#define CHECK_PM \
	do { \
		VK_CPU_ASSERT (ctx, IS_PRIVILEGED); \
	} while (0);

#define CHECK_FP \
	do { \
		VK_CPU_ASSERT (ctx, IS_FP_ENABLED); \
	} while (0);

/* Taken from MAME */
#define SHRINK(addr_) \
	((((addr_) >> 8) & 0xFF00) | ((addr_) & 0xFF))

#define IREG_GET(size_, addr_) \
	vk_buffer_get (ctx->iregs, size_, SHRINK (addr_))

#define IREG_PUT(size_, addr_, val_) \
	vk_buffer_put (ctx->iregs, size_, SHRINK (addr_), val_)

/* Forward declarations */
static int	sh4_get (sh4_t *, unsigned, uint32_t, void *);
static int	sh4_put (sh4_t *, unsigned, uint32_t, uint64_t);
static void	sh4_update_irqs (sh4_t *ctx);
static int	sh4_set_irq_state (vk_cpu_t *cpu, unsigned num, vk_irq_state_t state);

/* Generic Helpers */

static void
swap_r_banks (sh4_t *ctx)
{
	unsigned i;
	for (i = 0; i < 8; i++) {
		uint32_t tmp = R(i);
		R(i) = RBANK(i);
		RBANK(i) = tmp;
	}
}

static void
swap_f_banks (sh4_t *ctx)
{
	unsigned i;
	for (i = 0; i < 16; i++) {
		uint32_t tmp = FR(i).u;
		FR(i).u = XF(i).u;
		XF(i).u = tmp;
	}
}

static unsigned
get_r_bank_num (sh4_sr_t sr)
{
	/* In user mode, bank 0 is always selected; in privileged mode, bank
	 * SR.bit.rb is selected; it follows that bank 1 is only selected if
	 * both MD and RB are set. */
	return sr.bit.md & sr.bit.rb;
}

static void
set_sr (sh4_t *ctx, uint32_t data)
{
	sh4_sr_t old = SR;

	SR.full = data & 0x700083F3;

	/* Swap R banks if required */
	if (get_r_bank_num (old) != get_r_bank_num (SR))
		swap_r_banks (ctx);

	/* If any IRQ-related bits changed, re-validate pending IRQs */
	if ((old.bit.i != SR.bit.i) ||
	    (old.bit.bl != SR.bit.bl))
		sh4_update_irqs (ctx);
}

static uint32_t
get_sr (sh4_t *ctx)
{
	return SR.full;
}

static void
set_fpscr (sh4_t *ctx, uint32_t val)
{
	sh4_fpscr_t old = FPSCR;

	FPSCR.full = val & 0x003FFFFF;
	/* Swap FR banks if required */
	if (old.bit.fr != FPSCR.bit.fr)
		swap_f_banks (ctx);

	/* SZ and PR can't be both set */
	if (FPSCR.bit.sz && FPSCR.bit.pr)
		VK_CPU_ABORT (ctx, "invalid FPSCR: SZ and PR bot set");
}

static uint32_t
get_fpscr (sh4_t *ctx)
{
	return FPSCR.full;
}

/* Port A */

/* XXX Port A emulation is still very rough; we probably want to notify the
 * external handlers about the directions of all bits; although they already
 * probably know what to do with them. */

static void
set_porta (sh4_t *ctx, uint16_t data)
{
	uint32_t pctra;
	uint16_t pdtra;
	unsigned i;

	VK_ASSERT (ctx->porta.put);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to output */
		if (direction != 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	if (ctx->porta.put (ctx, pdtra))
		VK_ASSERT (0);
}

static uint16_t
get_porta (sh4_t *ctx)
{
	uint32_t pctra;
	uint16_t pdtra, data;
	unsigned i;

	VK_ASSERT (ctx->porta.get);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	if (ctx->porta.get (ctx, &data))
		VK_ASSERT (0);
	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to input */
		if (direction == 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	return pdtra;
}

/* Interrupt Controller */

static void
set_irq_priority (sh4_t *ctx, unsigned num, unsigned priority)
{
	/* An IRQ priority can't be lowered while an IRQ is firing */
	VK_ASSERT ((ctx->intc.irqs[num].state != VK_IRQ_STATE_RAISED) ||
	           (ctx->intc.irqs[num].priority <= priority));
	ctx->intc.irqs[num].priority = priority;
}

static void
sh4_intc_update_priorities (sh4_t *ctx)
{
	uint16_t ipra = IREG_GET (2, INTC_IPRA);
	uint16_t iprb = IREG_GET (2, INTC_IPRB);
	uint16_t iprc = IREG_GET (2, INTC_IPRC);

	/* See Table 19.5, "Interrupt Exception Sources and Priority Order" */
	set_irq_priority (ctx, SH4_IESOURCE_UDI, iprc & 15);
	set_irq_priority (ctx, SH4_IESOURCE_GPIOI, (iprc >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE0, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE1, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE2, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE3, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMAE, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI0, (ipra >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI1, (ipra >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TICPI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ATI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_PRI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_CUI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TEI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_BRIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ITI, (iprb >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RCMI, (iprb >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ROVI, (iprb >> 8) & 15);

	/* Update the pending flag for the new priorities */
	sh4_update_irqs (ctx);
}

/* DMA Controller */

/* TODO: DTR mode */
/* TODO: DMAC Address error on:
 * - DAR is in Area 7
 * - non-existant on-chip address */
/* TODO: validate RS settings against SAR and DAR */

static const uint32_t ts_mask[8] = { 7, 0, 1, 3, 31, ~0, ~0, ~0 };
static const uint32_t ts_incr[8] = { 8, 1, 2, 4, 32, 0, 0, 0 };

static void
sh4_dmac_update_channel_state (sh4_t *ctx, unsigned ch, uint32_t request_type)
{
	uint32_t offs = ch * 0x10;
	uint32_t dmaor = IREG_GET (4, DMAC_DMAOR);
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);

	VK_ASSERT (ch < 4);
	ctx->dmac.is_running[ch] = false;

	/* Check that both DME and DE are set */
	if (dmaor & chcr & 1) {
		uint32_t sar = IREG_GET (4, DMAC_SAR0 + offs);
		uint32_t dar = IREG_GET (4, DMAC_DAR0 + offs);
		uint32_t ts = (chcr >> 4) & 7;
		uint32_t rs = (chcr >> 8) & 15;
		uint32_t sm = (chcr >> 12) & 3;
		uint32_t dm = (chcr >> 14) & 3;

		VK_ASSERT (ts < 5);
		VK_ASSERT (sm != 3);
		VK_ASSERT (dm != 3);
		VK_ASSERT (rs != 1 && rs != 7 && rs != 15);

		/* Check the addresses and update AE if needed; bail out and
		 * send an Address Error exception. We only do it here,
		 * because tick_channel () can't alter the addresses as to
		 * raise an AE if they are correct here (by induction.) */
		if ((sar | dar) & ts_mask[ts]) {
			VK_CPU_LOG (ctx, "DMAC: raising DMA address error");
			sh4_set_irq_state ((vk_cpu_t *) ctx,
			                   SH4_IESOURCE_DMAE,
			                   VK_IRQ_STATE_RAISED);
			return;
		}

		/* Check if NMIF, AE or TE have been set */
		if ((dmaor & 6) || (chcr & 2))
			return;

		/* All checks passed; this DMA channel may now run */
		if ((rs >> 2) == request_type) {
			VK_CPU_LOG (ctx, "DMAC: enabling channel %u", ch);
			ctx->dmac.is_running[ch] = true;
		}
	}
}

static void
sh4_dmac_update_state (sh4_t *ctx, uint32_t request_type)
{
	sh4_dmac_update_channel_state (ctx, 0, request_type);
	sh4_dmac_update_channel_state (ctx, 1, request_type);
	sh4_dmac_update_channel_state (ctx, 2, request_type);
	sh4_dmac_update_channel_state (ctx, 3, request_type);
}

static void
sh4_dmac_tick_channel (sh4_t *ctx, unsigned ch)
{
	if (ctx->dmac.is_running[ch]) {
		uint32_t offs = ch * 0x10;
		uint32_t dmaor = IREG_GET (4, DMAC_DMAOR);
		uint32_t sar  = IREG_GET (4, DMAC_SAR0 + offs);
		uint32_t dar  = IREG_GET (4, DMAC_DAR0 + offs);
		uint32_t tcr  = IREG_GET (4, DMAC_TCR0 + offs);
		uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);
		uint32_t ts = (chcr >> 4) & 7;
		uint32_t sm = (chcr >> 12) & 3;
		uint32_t dm = (chcr >> 14) & 3;
		uint64_t tmp;

		/* TODO: raise a DMA AE if any error occurs. */

		switch (ts) {
		case 0: /* 8 bytes */
			sh4_get (ctx, 8, sar, &tmp);
			sh4_put (ctx, 8, dar, tmp);
			break;
		case 1: /* 1 byte */
			sh4_get (ctx, 1, sar, &tmp);
			sh4_put (ctx, 1, dar, tmp);
			break;
		case 2: /* 2 bytes */
			sh4_get (ctx, 2, sar, &tmp);
			sh4_put (ctx, 2, dar, tmp);
			break;
		case 3: /* 4 bytes */
			sh4_get (ctx, 4, sar, &tmp);
			sh4_put (ctx, 4, dar, tmp);
			break;
		case 4: /* 32 bytes */
			/* TODO */
			VK_ASSERT (0);
			break;
		}

		switch (sm) {
		case 1:
			sar += ts_incr[ts];
			break;
		case 2:
			sar -= ts_incr[ts];
			break;
		}

		switch (dm) {
		case 1:
			dar += ts_incr[ts];
			break;
		case 2:
			dar -= ts_incr[ts];
			break;
		}

		tcr --;
		if (tcr == 0) {
			chcr |= 2; /* TE */
			if (chcr & 4) { /* IE */
				unsigned num;
				num = (ch == 0) ? SH4_IESOURCE_DMTE0 :
				      (ch == 1) ? SH4_IESOURCE_DMTE1 :
				      (ch == 2) ? SH4_IESOURCE_DMTE2 :
				      SH4_IESOURCE_DMTE3;
				sh4_set_irq_state ((vk_cpu_t *) ctx, num,
				                   VK_IRQ_STATE_RAISED);
			}
			ctx->dmac.is_running[ch] = false;
		}

		IREG_PUT (4, DMAC_SAR0 + offs, sar);
		IREG_PUT (4, DMAC_DAR0 + offs, dar);
		IREG_PUT (4, DMAC_TCR0 + offs, tcr);
		IREG_PUT (4, DMAC_CHCR0 + offs, chcr);
		IREG_PUT (4, DMAC_DMAOR, dmaor);
	}
}

static void
sh4_dmac_tick (sh4_t *ctx)
{
	/* TODO: priorities (DMAOR.PR). Are they really that important? */

	sh4_dmac_tick_channel (ctx, 0);
	sh4_dmac_tick_channel (ctx, 1);
	sh4_dmac_tick_channel (ctx, 2);
	sh4_dmac_tick_channel (ctx, 3);
}

/* On-chip Modules
 * See Table A.1, "Address List" */

static int
sh4_ireg_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	VK_CPU_LOG (ctx, "IREG R%u %08X", size * 8, addr);

	set_ptr (val, size, IREG_GET (size, addr));

	switch (addr & 0xFFFFFF) {
	case BSC_RFCR:
	case CPG_WTCSR:
	case BSC_PDTRA:
		VK_ASSERT (size == 2);
		set_ptr (val, size, get_porta (ctx));
		break;
	case CCN_CCR:
	case CCN_INTEVT:
	case BSC_PCTRA:
		VK_ASSERT (size == 4);
		break;
	/* INTC */
	case INTC_IPRA:
	case INTC_IPRB:
	case INTC_IPRC:
		VK_ASSERT (size == 2);
		break;
	/* DMAC */
	case DMAC_SAR0 ... DMAC_DMAOR:
		VK_ASSERT (size == 4);
		break;
	/* TMU */
	case TMU_TSTR:
		/* XXX pharrier */
		VK_ASSERT (size == 1);
		break;
	case TMU_TCNT0: {
		static uint32_t counter = 0;
		VK_ASSERT (size == 4);
		set_ptr (val, size, counter);
		counter += 4;
		}
		break;
	/* Invalid/Unhandled */
	default:
		return -1;
	}
	return 0;
}

/* TODO: mask writes to read-only bits */

static int
sh4_ireg_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	VK_CPU_LOG (ctx, "IREG W%u %08X = %llX", size * 8, addr, val);

	switch (addr & 0xFFFFFF) {
	case 0x900000 ... 0x90FFFF: /* BSC_SDRM2 */
	case 0x940000 ... 0x94FFFF: /* BSC_SDRM3 */
	case CPG_STBCR:
	case TMU_TOCR:
	case TMU_TSTR: /* XXX pharrier */
		VK_ASSERT (size == 1);
		break;
	case BSC_BCR2:
	case BSC_PCR:
	case BSC_RTCSR:
	case BSC_RTCNT:
	case BSC_RTCOR:
	case BSC_RFCR:
	case CPG_WTCSR:
	case TMU_TCR0:
	case TMU_TCR1:
	case TMU_TCR2:
		VK_ASSERT (size == 2);
		break;
	case BSC_PDTRA:
		VK_ASSERT (size == 2);
		set_porta (ctx, val);
		break;
	case CCN_MMUCR:
	case CCN_CCR:
	case CCN_QACR0:
	case CCN_QACR1:
	case BSC_BCR1:
	case BSC_WCR1:
	case BSC_WCR2:
	case BSC_WCR3:
	case BSC_MCR:
	case BSC_PCTRA:
	case TMU_TCOR0:
	case TMU_TCNT0:
		VK_ASSERT (size == 4);
		break;
	/* INTC */
	case INTC_ICR:
		{
			uint16_t old = IREG_GET (2, addr);
			VK_ASSERT (size == 2);
			VK_ASSERT (!(val & ~0xC380));
			/* ICR.NMIL is read only */
			IREG_PUT (2, addr, (old & 0x8000) | (val & 0x7FFF));
			sh4_update_irqs (ctx);
		}
		return 0;
	case INTC_IPRA:
		VK_ASSERT (size == 2);
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	case INTC_IPRB:
		VK_ASSERT (size == 2);
		VK_ASSERT (!(val & 0xF));
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	case INTC_IPRC:
		VK_ASSERT (size == 2);
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	/* DMAC */
	case DMAC_SAR0:
	case DMAC_SAR1:
	case DMAC_SAR2:
	case DMAC_SAR3:
	case DMAC_DAR0:
	case DMAC_DAR1:
	case DMAC_DAR2:
	case DMAC_DAR3:
		VK_ASSERT (size == 4);
		break;
	case DMAC_TCR0:
	case DMAC_TCR1:
	case DMAC_TCR2:
	case DMAC_TCR3:
		VK_ASSERT (size == 4);
		VK_ASSERT (!(val & 0xFF000000));
		break;
	case DMAC_CHCR0:
	case DMAC_CHCR1:
	case DMAC_CHCR2:
	case DMAC_CHCR3:
		{
			unsigned ch = (addr >> 4) & 3;
			uint32_t old = IREG_GET (size, addr);
			VK_ASSERT (size == 4);
			VK_ASSERT (!(val & 0x00F00008));
			VK_ASSERT ((ch < 2) || !(val & 0x00050000));
			/* Make sure that TE doesn't get set */
			IREG_PUT (size, addr, (val & ~2) | (old & val & 2));
			sh4_dmac_update_channel_state (ctx, ch, 1);
		}
		return 0;
	case DMAC_DMAOR:
		{
			uint32_t old = IREG_GET (size, addr);
			VK_ASSERT (size == 4);
			VK_ASSERT (!(val & 0xFFFF7CF8));
			/* Make sure that AE and NMIF don't get set */
			IREG_PUT (size, addr, (val & ~6) | (old & val & 6));
			sh4_dmac_update_state (ctx, 1);
		}
		return 0;
	/* Invalid/Unhandled */
	default:
		return -1;
	}
	IREG_PUT (size, addr, val);
	return 0;
}

/* Store Queues */

static uint32_t
get_sq_addr (sh4_t *ctx, uint32_t addr)
{
	uint32_t sq_num = (addr >> 5) & 1;
	uint32_t sq_base = (sq_num == 0) ?
	                   vk_buffer_get (ctx->iregs, 4, SHRINK (CCN_QACR0)) :
	                   vk_buffer_get (ctx->iregs, 4, SHRINK (CCN_QACR1));
	return ((sq_base & 0x1C) << 24) | (addr & 0x03FFFFE0);
}

static int
sh4_sq_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	uint32_t sq_addr = get_sq_addr (ctx, addr);
	return sh4_get (ctx, size, sq_addr | (addr & 0x1F), val);
}

static int
sh4_sq_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t sq_addr = get_sq_addr (ctx, addr) | (addr & 0x1F);
	return sh4_put (ctx, size, sq_addr | (addr & 0x1F), val);
}

/* Bus Access */

static int
sh4_fetch (sh4_t *ctx, uint32_t addr, uint16_t *inst)
{
	int ret;

	ret = vk_cpu_get ((vk_cpu_t *) ctx, 2, addr & ADDR_MASK, (void *) inst);
	if (ret)
		VK_CPU_ABORT (ctx, "unhandled fetch @%08X", addr);
	return 0;
}

static int
sh4_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	int ret;

	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_get (ctx, size, addr, val);
	else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_get (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF80000000) {
		VK_CPU_LOG (ctx, "ONCHIP R%d @%08X", 8*size, addr);
		set_ptr (val, size, 0);
		return 0;
	} else
		ret = vk_cpu_get ((vk_cpu_t *) ctx, size, addr & ADDR_MASK, val);
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret)
		VK_CPU_ABORT (ctx, "unhandled R%d @%08X", 8*size, addr);
	return ret;
}

static int
sh4_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	int ret;

	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_put (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF80000000) {
		VK_CPU_ABORT (ctx, "ONCHIP W%d @%08X = %llX", 8*size, addr, val);
		return 0;
	} else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_put (ctx, size, addr, val);
	else
		ret = vk_cpu_put ((vk_cpu_t *) ctx, size, addr & ADDR_MASK, val);
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret)
		VK_CPU_ABORT (ctx, "unhandled W%d @%08X = %llX", 8*size, addr, val);
	return ret;
}

static inline uint8_t
R8 (sh4_t *ctx, uint32_t addr)
{
	uint8_t tmp;
	sh4_get (ctx, 1, addr, &tmp);
	return tmp;
}

static inline uint16_t
R16 (sh4_t *ctx, uint32_t addr)
{
	uint16_t tmp;
	sh4_get (ctx, 2, addr, &tmp);
	return tmp;
}

static inline uint32_t
R32 (sh4_t *ctx, uint32_t addr)
{
	uint32_t tmp;
	sh4_get (ctx, 4, addr, &tmp);
	return tmp;
}

static inline uint64_t
R64 (sh4_t *ctx, uint32_t addr)
{
	uint64_t tmp;
	sh4_get (ctx, 8, addr, &tmp);
	return tmp;
}

static inline void
W8 (sh4_t *ctx, uint32_t addr, uint8_t val)
{
	sh4_put (ctx, 1, addr, val);
}

static inline void
W16 (sh4_t *ctx, uint32_t addr, uint16_t val)
{
	sh4_put (ctx, 2, addr, val);
}

static inline void
W32 (sh4_t *ctx, uint32_t addr, uint32_t val)
{
	sh4_put (ctx, 4, addr, val);
}

static inline void
W64 (sh4_t *ctx, uint32_t addr, uint64_t val)
{
	sh4_put (ctx, 8, addr, val);
}

/* Interrupt Controller */

/**
 * Updates the intc.pending flag depending on whether an IRQ is pending or
 * not.
 */
static void
sh4_update_irqs (sh4_t *ctx)
{
	unsigned priority, i;
	int maxi;
	uint16_t icr = IREG_GET (2, INTC_ICR);

	/* Default state: no IRQ pending */
	ctx->intc.pending = false;
	ctx->intc.index = -1;

	/* Handle NMI first. NMI is always accepted when the CPU is in SLEEP
	 * or STANDBY state, and when ICR.NMIB is set. It is blocked by BL
	 * only if ICR.NMIB is clear. */
	if (ctx->intc.irqs[SH4_IESOURCE_NMI].state == VK_IRQ_STATE_RAISED) {
		ctx->intc.pending = (SR.bit.bl == 0) || ((icr & 0x200) != 0);
		if (ctx->intc.pending) {
			ctx->intc.index = SH4_IESOURCE_NMI;
			return;
		}
	}

	/* TODO: ICR.MIE */

	/* All interrupts are blocked when SR.BL is set */
	if (SR.bit.bl)
		return;

	/* Find the highest priority raised IRQ; note that interrupt source
	 * numbers are sorted from highest to lowest priority. */
	priority = 0;
	for (i = 0; i < SH4_NUM_IESOURCES; i++) {
		/* TODO: handle ties like the hardware does */
		if (ctx->intc.irqs[i].state == VK_IRQ_STATE_RAISED &&
		    ctx->intc.irqs[i].priority > priority) {
			priority = ctx->intc.irqs[i].priority;
			maxi = i;
		}
	}
	/* Set the pending flag */
	if (priority > 0) {
		ctx->intc.pending = true;
		ctx->intc.index = maxi;
	}
}

static int
sh4_set_irq_state (vk_cpu_t *cpu, unsigned num, vk_irq_state_t state)
{
	sh4_t *ctx = (sh4_t *) cpu;
	if (num >= SH4_NUM_IESOURCES)
		return -1;

	ctx->intc.irqs[num].state = state;

	/* Handle NMI */
	if (num == SH4_IESOURCE_NMI) {
		/* Set ICR.NMIL and DMAOR.NMIF */
		IREG_PUT (2, INTC_ICR, IREG_GET (2, INTC_ICR) | 0x8000);
		IREG_PUT (4, DMAC_DMAOR, IREG_GET (4, DMAC_DMAOR) | 2);
		/* Notify the DMAC that an NMI occurred */
		sh4_dmac_update_state (ctx, 0);
	}

	/* Update the pending flag */
	sh4_update_irqs (ctx);
	return 0;
}

void
sh4_process_irqs (vk_cpu_t *cpu)
{
	sh4_t *ctx = (sh4_t *) cpu;
	int index;

	/* Check if there's something to do */
	if (!ctx->intc.pending)
		return;

	index = ctx->intc.index;
	if (ctx->intc.irqs[index].priority > SR.bit.i) {
		sh4_sr_t tmp;

		/* Standard interrupt context switch */
		SPC = PC;
		SSR = SR;
		SGR = R(15);

		PC = VBR + ctx->intc.irqs[index].offset;

		VK_CPU_LOG (ctx, "IRQ taken: SR.i=%X PRI=%X VBR=%08X offs=%X code=%X; jumping at %08X",
		            SR.bit.i, ctx->intc.irqs[index].priority,
		            VBR, ctx->intc.irqs[index].offset,
		            ctx->intc.irqs[index].code, PC);

		tmp.full = SR.full;
		tmp.bit.bl = 1;
		tmp.bit.md = 1;
		tmp.bit.rb = 1;
		set_sr (ctx, tmp.full);

		IREG_PUT (4, CCN_INTEVT, ctx->intc.irqs[index].code);

		/* Clear the interrupt source; TODO this is not correct, the
		 * source should be cleared externally! */
		ctx->intc.irqs[index].state = VK_IRQ_STATE_CLEAR;

		/* Update the pending flag */
		sh4_update_irqs (ctx);
	}
}

/* Instructions */

static void delay_slot (sh4_t *ctx, uint32_t pc);

typedef void (* itype) (sh4_t *ctx, uint16_t inst);

#define I(name_) \
	static void sh4_interp_##name_ (sh4_t *ctx, uint16_t inst)

#define IDEF(mask_, match_, name_) \
	{ \
		mask_, \
		match_, \
		sh4_interp_##name_, \
	}

#define IS_SH4

#include "sh-insns-interp.h"
#include "sh-insns-desc.h"

#undef I
#undef IDEF
#undef IS_SH4

#define CHECK_COLLISION \
	do { \
		if (insns[inst] != sh4_interp_invalid) { \
			VK_LOG ("inst=%04X insns[inst]=%p", inst, insns[inst]); \
			VK_ABORT ("insns table collision"); \
		} \
	} while (0);

static void
setup_insns_handlers_from_table (const idesctype *desc,
                                 unsigned size)
{
	unsigned i, j;
	uint16_t inst;

	for (i = 0; i < size; i++) {
		switch (desc[i].mask) {
		case 0xF000:
			for (j = 0; j < 4096; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF00F:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | (j << 4);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xFF00:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF08F:
			for (j = 0; j < 128; j++) {
				inst = desc[i].match |
				       ((j & 7) << 4) |
				       ((j >> 3) << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF0FF:
			for (j = 0; j < 16; j++) {
				inst = desc[i].match | (j << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF1FF:
			for (j = 0; j < 8; j++) {
				inst = desc[i].match | (j << 9);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF3FF:
			for (j = 0; j < 4; j++) {
				inst = desc[i].match | (j << 10);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xFFFF:
			inst = desc[i].match;
			CHECK_COLLISION;
			insns[inst] = desc[i].handler;
			break;
		default:
			VK_ABORT ("unhandled mask %04X", desc[i].mask);
		}
	}
}

#undef CHECK_COLLISION

static void
setup_insns_handlers (void)
{
	unsigned i;

	for (i = 0; i < 65536; i++) {
		insns[i] = sh4_interp_invalid;
	}

	setup_insns_handlers_from_table (insns_desc_sh2, NUMELEM (insns_desc_sh2));
	setup_insns_handlers_from_table (insns_desc_sh4, NUMELEM (insns_desc_sh4));
}

/* Execution */

static void
sh4_step (sh4_t *ctx, uint32_t pc)
{
	uint16_t inst;
	uint32_t ppc = pc & 0x1FFFFFFF;

	sh4_fetch (ctx, pc, &inst);

	switch (ppc) {

	/* BOOTROM 0.92 */
	case 0x0C0010A4:
		VK_CPU_LOG (ctx, " ### IRQ: about to jump to handler @%08X", R(5))
		break;
	case 0x0C00B90A:
		VK_CPU_LOG (ctx, " ### JUMPING TO ROM CODE! (%X)", R(11));
		break;
#if 0
	case 0x0C00BD18:
		VK_CPU_LOG (ctx, " ### set_errno_and_init_machine_extended (%X)", R(4));
		break;
	case 0x0C00BC5C:
		VK_CPU_LOG (ctx, " ### authenticate_rom (%X, %X, %X)", R(6), R(7), R(8));
		break;
	case 0x0C00BC9E:
		VK_CPU_LOG (ctx, " ### authenticate_rom () : values read from EPROM %X: %X %X",
		            R(5), R(3), R(1));
		break;
	case 0x0C004E46:
		VK_CPU_LOG (ctx, " ### rombd_do_crc (%X, %X, %X)", R(4), R(5), R(6));
		break;
#endif

	/* All games */
	case 0x0C00BFB2:
		/* Makes the EEPROM check pass */
		R(0) = 0xF;
		break;
#if 0
	/* AIRTRIX */
	case 0x0C010E78:
		VK_CPU_LOG (ctx, " ### AIRTRIX: main ()");
		break;
	case 0x0C697A40:
		VK_CPU_LOG (ctx, " ### AIRTRIX: sync (%X)", R(4));
		break;
	case 0x0C697CDC:
		VK_CPU_LOG (ctx, " ### AIRTRIX: flush_list_to_15000010_idma ()");
		break;
	case 0x0C697D48:
		VK_CPU_LOG (ctx, " ### AIRTRIX: flush_list_to_1A040000_fifo ()");
		break;
	case 0x0C697C3C:
		VK_CPU_LOG (ctx, " ### AIRTRIX: update_controls ()");
		break;
	case 0x0C6996A0:
		VK_CPU_LOG (ctx, " ### AIRTRIX: print_warning (%X)", R(4));
		break;
	case 0x0C0310CC:
		VK_CPU_LOG (ctx, " ### AIRTRIX: upload_textures_from_table_with_dma_and_idma (%X)", R(4));
		break;
	case 0x0C699A60:
		VK_CPU_LOG (ctx, " ### AIRTRIX: do_memctl_dma_and_idma (%X,%X,%X,%X)", R(4),R(5),R(6),R(7));
		break;
	case 0x0C699580:
	case 0x0C699760:
		VK_CPU_LOG (ctx, " ### AIRTRIX: upload_texture (%X,%X,%X)", R(4),R(5),R(6));
		break;
	case 0x0C699200:
		VK_CPU_LOG (ctx, " ### AIRTRIX: texture_foo (%X,%X,%X,%X,SP,SP+4)", R(4),R(5),R(6),R(7));
		break;
	case 0x0C699140:
		VK_CPU_LOG (ctx, " ### AIRTRIX: clear_layer (%X,%X)", R(4),R(5));
		break;
	case 0x0C010F9A:
		/* Make the 'WARNING' screen faster (well, 656 frames faster) */
		R(2) = 0x290;
		break;
	case 0x0C014D72:
		VK_CPU_LOG (ctx, " ### AIRTRIX: huge_shit ()");
		break;
	case 0x0C0263EA:
		VK_CPU_LOG (ctx, " ### AIRTRIX: game_logic_update_A ()");
		break;
#endif

#if 1
	/* PHARRIER */
	case 0x0C0125C0:
		VK_CPU_LOG (ctx, " ### PHARRIER: sync (%X)", R(4));
		break;
	case 0x0C0F4280:
		VK_CPU_LOG (ctx, " ### PHARRIER: upload_textures_in_a_double_loop (%X, %X, %X)", R(4), R(5), R(6));
		break;
	case 0x0C0149CA:
		VK_CPU_LOG (ctx, " ### PHARRIER: aica_upload (%X, %X, %X)", R(4), R(5), R(6));
		break;
	case 0x0C0144FE:
		VK_CPU_LOG (ctx, " ### PHARRIER: do_dmac (%X, %X, %X, %X)", R(4), R(5), R(6), R(7));
		break;
	case 0x0C011AF0:
		VK_CPU_LOG (ctx, " ### PHARRIER: memcpy_sq (%X,%X,%X)", R(4), R(5), R(6));
		break;
	case 0x0C094560:
		VK_CPU_LOG (ctx, " ### PHARRIER: model_init_main ()");
		break;
	case 0x0C012990:
		VK_CPU_LOG (ctx, " ### PHARRIER: slMemCpySlaveCPU (%X, %X, %X, %X)", R(4), R(5), R(6), R(7));
		break;
	case 0x0C0C89C0:
		VK_CPU_LOG (ctx, " ### PHARRIER: print_str (%X,%X)", R(4), R(5));
		break;
	case 0x0C095240:
		VK_CPU_LOG (ctx, " ### PHARRIER: memctl_dma_and_load_obj (%X)", R(4));
		break;
	case 0x0C01C0A0:
		VK_CPU_LOG (ctx, " ### PHARRIER: gpu_1a_fifo_periodic ()");
		break;
#if 1
	case 0x0C01C322:
		/* Patches an AICA-related while (1) into a NOP */
		inst = 0x0009;
		break;
	case 0x0C011B14:
		/* Patches a software BUG */
		R(0) |= 0xFFFFFF;
		break;
#endif
#endif
	}

	insns[inst] (ctx, inst);
	//sh4_bsc_tick (ctx);
	//sh4_tmu_rtc_tick (ctx);
	//sh4_sci_tick (ctx);
	sh4_dmac_tick (ctx);
	ctx->base.remaining --;
}

static void
delay_slot (sh4_t *ctx, uint32_t pc)
{
	ctx->in_slot = true;
	sh4_step (ctx, pc);
	ctx->in_slot = false;
}

static int
sh4_run (vk_cpu_t *cpu, int cycles)
{
	sh4_t *ctx = (sh4_t *) cpu;

	cpu->remaining = cycles;
	while (cpu->remaining > 0) {
		if (cpu->state != VK_CPU_STATE_RUN)
			return 0;
		sh4_process_irqs (cpu);
		sh4_step (ctx, PC);
		PC += 2;
	}
	return -cpu->remaining;
}

void
sh4_set_state (vk_cpu_t *cpu, vk_cpu_state_t state)
{
	/* TODO: standby, deep sleep, etc. */
	cpu->state = state;
}

/* See Table 19.5, "Interrupt Exception Handling Sources and
 * Priority Orders".
 *
 * Note that the indices of this array are ordered from highest priority
 * to lowest. */

static const sh4_irq_state_t default_irq_state[SH4_NUM_IESOURCES] = {
	[SH4_IESOURCE_NMI] = { VK_IRQ_STATE_CLEAR, 16, 0x600, 0x1C0 },
	/* IRQs */
	[SH4_IESOURCE_IRQ0] = { VK_IRQ_STATE_CLEAR, 15, 0x600, 0x200 },
	[SH4_IESOURCE_IRQ1] = { VK_IRQ_STATE_CLEAR, 14, 0x600, 0x220 },
	[SH4_IESOURCE_IRQ2] = { VK_IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	[SH4_IESOURCE_IRQ3] = { VK_IRQ_STATE_CLEAR, 12, 0x600, 0x260 },
	[SH4_IESOURCE_IRQ4] = { VK_IRQ_STATE_CLEAR, 11, 0x600, 0x280 },
	[SH4_IESOURCE_IRQ5] = { VK_IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	[SH4_IESOURCE_IRQ6] = { VK_IRQ_STATE_CLEAR,  9, 0x600, 0x2C0 },
	[SH4_IESOURCE_IRQ7] = { VK_IRQ_STATE_CLEAR,  8, 0x600, 0x2E0 },
	[SH4_IESOURCE_IRQ8] = { VK_IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	[SH4_IESOURCE_IRQ9] = { VK_IRQ_STATE_CLEAR,  6, 0x600, 0x320 },
	[SH4_IESOURCE_IRQ10] = { VK_IRQ_STATE_CLEAR,  5, 0x600, 0x360 },
	[SH4_IESOURCE_IRQ11] = { VK_IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	[SH4_IESOURCE_IRQ12] = { VK_IRQ_STATE_CLEAR,  3, 0x600, 0x380 },
	[SH4_IESOURCE_IRQ13] = { VK_IRQ_STATE_CLEAR,  2, 0x600, 0x3A0 },
	[SH4_IESOURCE_IRQ14] = { VK_IRQ_STATE_CLEAR,  1, 0x600, 0x3C0 },
	/* IRLs */
	[SH4_IESOURCE_IRL0] = { VK_IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	[SH4_IESOURCE_IRL1] = { VK_IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	[SH4_IESOURCE_IRL2] = { VK_IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	[SH4_IESOURCE_IRL3] = { VK_IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	/* UDI */
	[SH4_IESOURCE_UDI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x600 },
	/* GPIO */
	[SH4_IESOURCE_GPIOI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x620 },
	/* DMAC */
	[SH4_IESOURCE_DMTE0] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x640 },
	[SH4_IESOURCE_DMTE1] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x660 },
	[SH4_IESOURCE_DMTE2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x680 },
	[SH4_IESOURCE_DMTE3] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x6A0 },
	[SH4_IESOURCE_DMAE] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x6C0 },
	/* TMU */
	[SH4_IESOURCE_TUNI0] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x400 },
	[SH4_IESOURCE_TUNI1] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x420 },
	[SH4_IESOURCE_TUNI2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x440 },
	[SH4_IESOURCE_TICPI2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x460 },
	/* RTC */
	[SH4_IESOURCE_ATI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x480 },
	[SH4_IESOURCE_PRI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4A0 },
	[SH4_IESOURCE_CUI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4C0 },
	/* SCI1 */
	[SH4_IESOURCE_ERI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4E0 },
	[SH4_IESOURCE_RXI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x500 },
	[SH4_IESOURCE_TXI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x520 },
	[SH4_IESOURCE_TEI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x540 },
	/* SCIF */
	[SH4_IESOURCE_ERIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x700 },
	[SH4_IESOURCE_RXIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x720 },
	[SH4_IESOURCE_BRIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x740 },
	[SH4_IESOURCE_TXIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x760 },
	/* WDT */
	[SH4_IESOURCE_ITI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x560 },
	/* REF */
	[SH4_IESOURCE_RCMI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x580 },
	[SH4_IESOURCE_ROVI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x5A0 },
};

static void
sh4_reset (vk_cpu_t *cpu, vk_reset_type_t type)
{
	sh4_t *ctx = (sh4_t *) cpu;
	uint32_t tmp;

	cpu->state = (ctx->config.master) ?
	              VK_CPU_STATE_RUN :
	              VK_CPU_STATE_STOP;

	memset (ctx->r, 0, sizeof (ctx->r));
	memset (ctx->f.f, 0, sizeof (ctx->f.f));
	memset (ctx->x.f, 0, sizeof (ctx->x.f));
	memset (ctx->rbank, 0, sizeof (ctx->rbank));

	vk_buffer_clear (ctx->iregs);

	PC = 0xA0000000;
	PR = 0;
	SPC = 0;
	SGR = 0;
	VBR = 0;
	GBR = 0;
	DBR = 0;
	MAC = 0;

	SR.full = 0;
	SR.bit.i = 0xF;
	SR.bit.bl = 1;
	SR.bit.rb = 1;
	SR.bit.md = 1;

	SSR.full = 0;

	FPSCR.full = 0;
	FPSCR.bit.rm = 1;
	FPSCR.bit.dn = 1;
	FPUL.u    = 0;

	/* See Table A.1, "Address List" */
	tmp = (ctx->config.master ? 0x40000000 : 0) |
	      (ctx->config.little_endian) ? 0x80000000 : 0;

	IREG_PUT (4, CCN_EXPEVT, (type == VK_RESET_TYPE_HARD) ? 0 : 0x20);
	IREG_PUT (4, BSC_BCR1, tmp);
	IREG_PUT (2, BSC_BCR2, 0x3FFC);
	IREG_PUT (4, BSC_WCR1, 0x77777777);
	IREG_PUT (4, BSC_WCR2, 0xFFFEEFFF);
	IREG_PUT (4, BSC_WCR3, 0x07777777);
	IREG_PUT (4, TMU_TCOR0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR2, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT2, 0xFFFFFFFF);
	IREG_PUT (1, SCI_SCBRR1, 0xFF);
	IREG_PUT (1, SCI_SCTDR1, 0xFF);
	IREG_PUT (1, SCI_SCSSR1, 0x84);
	IREG_PUT (1, SCIF_SCBRR2, 0xFF);
	IREG_PUT (2, SCIF_SCFSR2, 0x0060);
	IREG_PUT (2, UDI_SDIR, 0xFFFF);

	ctx->dmac.is_running[0] = false;
	ctx->dmac.is_running[1] = false;
	ctx->dmac.is_running[2] = false;
	ctx->dmac.is_running[3] = false;

	/* TODO: master/slave in BSC */

	ctx->in_slot = false;

	ctx->intc.pending = false;
	ctx->intc.index = -1;
	VK_ASSERT (sizeof (ctx->intc.irqs) == sizeof (default_irq_state));
	memcpy (ctx->intc.irqs, default_irq_state, sizeof (default_irq_state));
}

static const char *
sh4_get_debug_string (vk_cpu_t *cpu)
{
	sh4_t *ctx = (sh4_t *) cpu;
	static char str[256];
	sprintf (str, "%c @%08X @%08X %08X",
	         ctx->config.master ? 'M' : 'S', PC, PR, SR.full);
	return str;
}

void
sh4_set_porta_handlers (vk_cpu_t *cpu,
                        int (* get)(sh4_t *ctx, uint16_t *val),
                        int (* put)(sh4_t *ctx, uint16_t val))
{
	sh4_t *ctx = (sh4_t *) cpu;

	ctx->porta.get = get;
	ctx->porta.put = put;
}

static void
sh4_delete (vk_cpu_t **cpu_)
{
	if (cpu_) {
		sh4_t *ctx = (sh4_t *) *cpu_;
		vk_buffer_delete (&ctx->iregs);
		free (ctx);
		*cpu_ = NULL;
	}
}

vk_cpu_t *
sh4_new (vk_machine_t *mach, vk_mmap_t *mmap, bool master, bool le)
{
	sh4_t *ctx = ALLOC (sh4_t);
	if (!ctx)
		goto fail;

	ctx->base.mach = mach;
	ctx->base.mmap = mmap;

	ctx->base.set_state		= sh4_set_state;
	ctx->base.run			= sh4_run;
	ctx->base.reset			= sh4_reset;
	ctx->base.set_irq_state		= sh4_set_irq_state;
	ctx->base.get_debug_string	= sh4_get_debug_string;
	ctx->base.delete		= sh4_delete;

	ctx->config.master = master;
	ctx->config.little_endian = le;

	ctx->iregs = vk_buffer_le32_new (0x10000, 0);
	if (!ctx->iregs)
		goto fail;

	setup_insns_handlers ();

	return (vk_cpu_t *) ctx;
fail:
	vk_cpu_delete ((vk_cpu_t **) &ctx);
	return NULL;
}
