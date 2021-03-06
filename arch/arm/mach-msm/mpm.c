/* Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/rpm-smd-regulator.h>
#include <linux/workqueue.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/clk/msm-clk.h>
#include <linux/irqchip/msm-gpio-irq.h>
#include <linux/irqchip/msm-mpm-irq.h>
#include <asm/arch_timer.h>

/******************************************************************************
 * Request and Status Definitions
 *****************************************************************************/

enum {
	MSM_MPM_REQUEST_REG_ENABLE,
	MSM_MPM_REQUEST_REG_CLEAR,
};

#define MSM_MPM_NR_APPS_IRQS  (NR_MSM_IRQS + NR_GPIO_IRQS)

#define MSM_MPM_REG_WIDTH  DIV_ROUND_UP(MSM_MPM_NR_MPM_IRQS, 32)

#define MSM_MPM_IRQ_INDEX(irq)  (irq / 32)
#define MSM_MPM_IRQ_MASK(irq)  BIT(irq % 32)

#define hashfn(val) (val % MSM_MPM_NR_MPM_IRQS)
#define SCLK_HZ (32768)
#define ARCH_TIMER_HZ (19200000)

static struct msm_mpm_device_data msm_mpm_dev_data;
static uint8_t msm_mpm_irqs_a2m[MSM_MPM_NR_APPS_IRQS];

static struct clk *xo_clk;
static bool xo_enabled;
static bool msm_mpm_in_suspend;

enum mpm_reg_offsets {
	MSM_MPM_REG_WAKEUP,
	MSM_MPM_REG_ENABLE,
	MSM_MPM_REG_FALLING_EDGE,
	MSM_MPM_REG_RISING_EDGE,
	MSM_MPM_REG_POLARITY,
	MSM_MPM_REG_STATUS,
};

static DEFINE_SPINLOCK(msm_mpm_lock);

/*
 * Note: the following two bitmaps only mark irqs that are _not_
 * mappable to MPM.
 */
static DECLARE_BITMAP(msm_mpm_enabled_apps_irqs, MSM_MPM_NR_APPS_IRQS);
static DECLARE_BITMAP(msm_mpm_wake_apps_irqs, MSM_MPM_NR_APPS_IRQS);

static DECLARE_BITMAP(msm_mpm_gpio_irqs_mask, MSM_MPM_NR_APPS_IRQS);

static uint32_t msm_mpm_enabled_irq[MSM_MPM_REG_WIDTH];
static uint32_t msm_mpm_wake_irq[MSM_MPM_REG_WIDTH];
static uint32_t msm_mpm_falling_edge[MSM_MPM_REG_WIDTH];
static uint32_t msm_mpm_rising_edge[MSM_MPM_REG_WIDTH];
static uint32_t msm_mpm_polarity[MSM_MPM_REG_WIDTH];

enum {
	MSM_MPM_DEBUG_NON_DETECTABLE_IRQ = BIT(0),
	MSM_MPM_DEBUG_PENDING_IRQ = BIT(1),
	MSM_MPM_DEBUG_WRITE = BIT(2),
	MSM_MPM_DEBUG_NON_DETECTABLE_IRQ_IDLE = BIT(3),
};

static int msm_mpm_debug_mask = 1;
module_param_named(
	debug_mask, msm_mpm_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP
);

enum mpm_state {
	MSM_MPM_IRQ_MAPPING_DONE = BIT(0),
	MSM_MPM_DEVICE_PROBED = BIT(1),
};

static enum mpm_state msm_mpm_initialized;

static inline bool msm_mpm_is_initialized(void)
{
	return msm_mpm_initialized &
		(MSM_MPM_IRQ_MAPPING_DONE | MSM_MPM_DEVICE_PROBED);

}

static inline uint32_t msm_mpm_read(
	unsigned int reg, unsigned int subreg_index)
{
	unsigned int offset = reg * MSM_MPM_REG_WIDTH + subreg_index;
	return __raw_readl(msm_mpm_dev_data.mpm_request_reg_base + offset * 4);
}

static inline void msm_mpm_write(
	unsigned int reg, unsigned int subreg_index, uint32_t value)
{
	unsigned int offset = reg * MSM_MPM_REG_WIDTH + subreg_index;

	__raw_writel(value, msm_mpm_dev_data.mpm_request_reg_base + offset * 4);
	if (MSM_MPM_DEBUG_WRITE & msm_mpm_debug_mask)
		pr_info("%s: reg %u.%u: 0x%08x\n",
			__func__, reg, subreg_index, value);
}

static inline void msm_mpm_send_interrupt(void)
{
	__raw_writel(msm_mpm_dev_data.mpm_apps_ipc_val,
			msm_mpm_dev_data.mpm_apps_ipc_reg);
	/* Ensure the write is complete before returning. */
	wmb();
}

static irqreturn_t msm_mpm_irq(int irq, void *dev_id)
{
	/*
	 * When the system resumes from deep sleep mode, the RPM hardware wakes
	 * up the Apps processor by triggering this interrupt. This interrupt
	 * has to be enabled and set as wake for the irq to get SPM out of
	 * sleep. Handle the interrupt here to make sure that it gets cleared.
	 */
	return IRQ_HANDLED;
}

static void msm_mpm_set(cycle_t wakeup, bool wakeset)
{
	uint32_t *irqs;
	unsigned int reg;
	int i;
	uint32_t *expiry_timer;

	expiry_timer = (uint32_t *)&wakeup;

	irqs = wakeset ? msm_mpm_wake_irq : msm_mpm_enabled_irq;
	for (i = 0; i < MSM_MPM_REG_WIDTH; i++) {
		reg = MSM_MPM_REG_WAKEUP;
		msm_mpm_write(reg, i, expiry_timer[i]);

		reg = MSM_MPM_REG_ENABLE;
		msm_mpm_write(reg, i, irqs[i]);

		reg = MSM_MPM_REG_FALLING_EDGE;
		msm_mpm_write(reg, i, msm_mpm_falling_edge[i]);

		reg = MSM_MPM_REG_RISING_EDGE;
		msm_mpm_write(reg, i, msm_mpm_rising_edge[i]);

		reg = MSM_MPM_REG_POLARITY;
		msm_mpm_write(reg, i, msm_mpm_polarity[i]);

		reg = MSM_MPM_REG_STATUS;
		msm_mpm_write(reg, i, 0);
	}

	/*
	 * Ensure that the set operation is complete before sending the
	 * interrupt
	 */
	wmb();
	msm_mpm_send_interrupt();
}

/******************************************************************************
 * Interrupt Mapping Functions
 *****************************************************************************/

static inline bool msm_mpm_is_valid_apps_irq(unsigned int irq)
{
	return irq < ARRAY_SIZE(msm_mpm_irqs_a2m);
}

static inline uint8_t msm_mpm_get_irq_a2m(unsigned int irq)
{
	return msm_mpm_irqs_a2m[irq];
}

static inline void msm_mpm_set_irq_a2m(unsigned int apps_irq,
	unsigned int mpm_irq)
{
	msm_mpm_irqs_a2m[apps_irq] = (uint8_t) mpm_irq;
}

static inline bool msm_mpm_is_valid_mpm_irq(unsigned int irq)
{
	return irq < msm_mpm_dev_data.irqs_m2a_size;
}

static inline uint16_t msm_mpm_get_irq_m2a(unsigned int irq)
{
	return msm_mpm_dev_data.irqs_m2a[irq];
}

static bool msm_mpm_bypass_apps_irq(unsigned int irq)
{
	int i;

	for (i = 0; i < msm_mpm_dev_data.bypassed_apps_irqs_size; i++)
		if (irq == msm_mpm_dev_data.bypassed_apps_irqs[i])
			return true;

	return false;
}

static int msm_mpm_enable_irq_exclusive(
	unsigned int irq, bool enable, bool wakeset)
{
	uint32_t mpm_irq;

	if (!msm_mpm_is_valid_apps_irq(irq))
		return -EINVAL;

	mpm_irq = msm_mpm_get_irq_a2m(irq);

	if (msm_mpm_bypass_apps_irq(irq))
		return 0;

	if (mpm_irq) {
		uint32_t *mpm_irq_masks = wakeset ?
				msm_mpm_wake_irq : msm_mpm_enabled_irq;
		uint32_t index = MSM_MPM_IRQ_INDEX(mpm_irq);
		uint32_t mask = MSM_MPM_IRQ_MASK(mpm_irq);

		if (enable)
			mpm_irq_masks[index] |= mask;
		else
			mpm_irq_masks[index] &= ~mask;
	} else {
		unsigned long *apps_irq_bitmap = wakeset ?
			msm_mpm_wake_apps_irqs : msm_mpm_enabled_apps_irqs;

		if (enable)
			__set_bit(irq, apps_irq_bitmap);
		else
			__clear_bit(irq, apps_irq_bitmap);
	}

	return 0;
}

static void msm_mpm_set_edge_ctl(int pin, unsigned int flow_type)
{
	uint32_t index;
	uint32_t mask;

	index = MSM_MPM_IRQ_INDEX(pin);
	mask = MSM_MPM_IRQ_MASK(pin);

	if (flow_type & IRQ_TYPE_EDGE_FALLING)
		msm_mpm_falling_edge[index] |= mask;
	else
		msm_mpm_falling_edge[index] &= ~mask;

	if (flow_type & IRQ_TYPE_EDGE_RISING)
		msm_mpm_rising_edge[index] |= mask;
	else
		msm_mpm_rising_edge[index] &= ~mask;

}

static int msm_mpm_set_irq_type_exclusive(
	unsigned int irq, unsigned int flow_type)
{
	uint32_t mpm_irq;

	mpm_irq = msm_mpm_get_irq_a2m(irq);

	if (!msm_mpm_is_valid_apps_irq(irq))
		return -EINVAL;

	if (msm_mpm_bypass_apps_irq(irq))
		return 0;

	if (mpm_irq) {
		uint32_t index = MSM_MPM_IRQ_INDEX(mpm_irq);
		uint32_t mask = MSM_MPM_IRQ_MASK(mpm_irq);

		if (index >= MSM_MPM_REG_WIDTH)
			return -EFAULT;

		msm_mpm_set_edge_ctl(mpm_irq, flow_type);

		if (flow_type &  IRQ_TYPE_LEVEL_HIGH)
			msm_mpm_polarity[index] |= mask;
		else
			msm_mpm_polarity[index] &= ~mask;
	}
	return 0;
}

static int __msm_mpm_enable_irq(unsigned int irq, unsigned int enable)
{
	unsigned long flags;
	int rc;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);
	rc = msm_mpm_enable_irq_exclusive(irq, (bool)enable, false);
	spin_unlock_irqrestore(&msm_mpm_lock, flags);

	return rc;
}

static void msm_mpm_enable_irq(struct irq_data *d)
{
	__msm_mpm_enable_irq(d->irq, 1);
}

static void msm_mpm_disable_irq(struct irq_data *d)
{
	__msm_mpm_enable_irq(d->irq, 0);
}

static int msm_mpm_set_irq_wake(struct irq_data *d, unsigned int on)
{
	unsigned long flags;
	int rc;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);
	rc = msm_mpm_enable_irq_exclusive(d->irq, (bool)on, true);
	spin_unlock_irqrestore(&msm_mpm_lock, flags);

	return rc;
}

static int msm_mpm_set_irq_type(struct irq_data *d, unsigned int flow_type)
{
	unsigned long flags;
	int rc;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);
	rc = msm_mpm_set_irq_type_exclusive(d->irq, flow_type);
	spin_unlock_irqrestore(&msm_mpm_lock, flags);

	return rc;
}

/******************************************************************************
 * Public functions
 *****************************************************************************/
int msm_mpm_enable_pin(unsigned int pin, unsigned int enable)
{
	uint32_t index = MSM_MPM_IRQ_INDEX(pin);
	uint32_t mask = MSM_MPM_IRQ_MASK(pin);
	unsigned long flags;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);

	if (enable)
		msm_mpm_enabled_irq[index] |= mask;
	else
		msm_mpm_enabled_irq[index] &= ~mask;

	spin_unlock_irqrestore(&msm_mpm_lock, flags);
	return 0;
}

int msm_mpm_set_pin_wake(unsigned int pin, unsigned int on)
{
	uint32_t index = MSM_MPM_IRQ_INDEX(pin);
	uint32_t mask = MSM_MPM_IRQ_MASK(pin);
	unsigned long flags;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);

	if (on)
		msm_mpm_wake_irq[index] |= mask;
	else
		msm_mpm_wake_irq[index] &= ~mask;

	spin_unlock_irqrestore(&msm_mpm_lock, flags);
	return 0;
}

int msm_mpm_set_pin_type(unsigned int pin, unsigned int flow_type)
{
	uint32_t index = MSM_MPM_IRQ_INDEX(pin);
	uint32_t mask = MSM_MPM_IRQ_MASK(pin);
	unsigned long flags;

	if (!msm_mpm_is_initialized())
		return -EINVAL;

	spin_lock_irqsave(&msm_mpm_lock, flags);

	msm_mpm_set_edge_ctl(pin, flow_type);

	if (flow_type & IRQ_TYPE_LEVEL_HIGH)
		msm_mpm_polarity[index] |= mask;
	else
		msm_mpm_polarity[index] &= ~mask;

	spin_unlock_irqrestore(&msm_mpm_lock, flags);
	return 0;
}

bool msm_mpm_irqs_detectable(bool from_idle)
{
	unsigned long *apps_irq_bitmap;
	bool ret = false;
	int debug_mask;

	if (from_idle) {
		apps_irq_bitmap = msm_mpm_enabled_apps_irqs;
		debug_mask = msm_mpm_debug_mask &
				MSM_MPM_DEBUG_NON_DETECTABLE_IRQ_IDLE;
	} else {
		apps_irq_bitmap = msm_mpm_wake_apps_irqs;
		debug_mask = msm_mpm_debug_mask &
				MSM_MPM_DEBUG_NON_DETECTABLE_IRQ;
	}

	ret = (bool)__bitmap_empty(apps_irq_bitmap, MSM_MPM_NR_APPS_IRQS);

	if (debug_mask && !ret) {
		static char buf[DIV_ROUND_UP(MSM_MPM_NR_APPS_IRQS, 32)*9+1];

		bitmap_scnprintf(buf, sizeof(buf), apps_irq_bitmap,
				MSM_MPM_NR_APPS_IRQS);
		buf[sizeof(buf) - 1] = '\0';

		pr_info("%s: cannot monitor %s", __func__, buf);
	}

	return ret;
}

bool msm_mpm_gpio_irqs_detectable(bool from_idle)
{
	unsigned long *apps_irq_bitmap = from_idle ?
			msm_mpm_enabled_apps_irqs : msm_mpm_wake_apps_irqs;

	return !__bitmap_intersects(msm_mpm_gpio_irqs_mask, apps_irq_bitmap,
			MSM_MPM_NR_APPS_IRQS);
}

void msm_mpm_enter_sleep(uint32_t sclk_count, bool from_idle,
		const struct cpumask *cpumask)
{
	cycle_t wakeup = (u64)sclk_count * ARCH_TIMER_HZ;

	if (!msm_mpm_is_initialized()) {
		pr_err("%s(): MPM not initialized\n", __func__);
		return;
	}

	if (sclk_count) {
		do_div(wakeup, SCLK_HZ);
		wakeup += arch_counter_get_cntpct();
	} else {
		wakeup = (~0ULL);
	}

	msm_mpm_set(wakeup, !from_idle);
	if (cpumask)
		irq_set_affinity(msm_mpm_dev_data.mpm_ipc_irq, cpumask);
}

void msm_mpm_exit_sleep(bool from_idle)
{
	unsigned long pending;
	int i;
	int k;

	if (!msm_mpm_is_initialized()) {
		pr_err("%s(): MPM not initialized\n", __func__);
		return;
	}

	for (i = 0; i < MSM_MPM_REG_WIDTH; i++) {
		pending = msm_mpm_read(MSM_MPM_REG_STATUS, i);

		if (MSM_MPM_DEBUG_PENDING_IRQ & msm_mpm_debug_mask)
			pr_info("%s: pending.%d: 0x%08lx", __func__,
					i, pending);

		k = find_first_bit(&pending, 32);
		while (k < 32) {
			unsigned int mpm_irq = 32 * i + k;
			unsigned int apps_irq = msm_mpm_get_irq_m2a(mpm_irq);
			struct irq_desc *desc = apps_irq ?
				irq_to_desc(apps_irq) : NULL;

			if (desc && !irqd_is_level_type(&desc->irq_data)) {
				irq_set_pending(apps_irq);
				if (from_idle) {
					raw_spin_lock(&desc->lock);
					check_irq_resend(desc, apps_irq);
					raw_spin_unlock(&desc->lock);
				}
			}

			k = find_next_bit(&pending, 32, k + 1);
		}
	}
}
static void msm_mpm_sys_low_power_modes(bool allow)
{
	if (allow) {
		if (xo_enabled) {
			clk_disable_unprepare(xo_clk);
			xo_enabled = false;
		}
	} else {
		if (!xo_enabled) {
			/* If we cannot enable XO clock then we want to flag it,
			 * than having to deal with not being able to wakeup
			 * from a non-monitorable interrupt
			 */
			BUG_ON(clk_prepare_enable(xo_clk));
			xo_enabled = true;
		}
	}
}

void msm_mpm_suspend_prepare(void)
{
	bool allow;
	unsigned long flags;

	spin_lock_irqsave(&msm_mpm_lock, flags);

	allow = msm_mpm_irqs_detectable(false) &&
		msm_mpm_gpio_irqs_detectable(false);
	msm_mpm_in_suspend = true;

	spin_unlock_irqrestore(&msm_mpm_lock, flags);
	msm_mpm_sys_low_power_modes(allow);
}
EXPORT_SYMBOL(msm_mpm_suspend_prepare);

void msm_mpm_suspend_wake(void)
{
	bool allow;
	unsigned long flags;

	spin_lock_irqsave(&msm_mpm_lock, flags);

	allow = msm_mpm_irqs_detectable(true) &&
		msm_mpm_gpio_irqs_detectable(true);

	spin_unlock_irqrestore(&msm_mpm_lock, flags);
	msm_mpm_sys_low_power_modes(allow);
	msm_mpm_in_suspend = false;
}
EXPORT_SYMBOL(msm_mpm_suspend_wake);

static int __init msm_mpm_early_init(void)
{
	uint8_t mpm_irq;
	uint16_t apps_irq;

	for (mpm_irq = 0; msm_mpm_is_valid_mpm_irq(mpm_irq); mpm_irq++) {
		apps_irq = msm_mpm_get_irq_m2a(mpm_irq);
		if (apps_irq && msm_mpm_is_valid_apps_irq(apps_irq))
			msm_mpm_set_irq_a2m(apps_irq, mpm_irq);
	}

	msm_mpm_initialized |= MSM_MPM_DEVICE_PROBED;
	return 0;
}

early_initcall(msm_mpm_early_init);

void __init msm_mpm_irq_extn_init(struct msm_mpm_device_data *mpm_data)
{
	gic_arch_extn.irq_mask = msm_mpm_disable_irq;
	gic_arch_extn.irq_unmask = msm_mpm_enable_irq;
	gic_arch_extn.irq_disable = msm_mpm_disable_irq;
	gic_arch_extn.irq_set_type = msm_mpm_set_irq_type;
	gic_arch_extn.irq_set_wake = msm_mpm_set_irq_wake;

	msm_gpio_irq_extn.irq_mask = msm_mpm_disable_irq;
	msm_gpio_irq_extn.irq_unmask = msm_mpm_enable_irq;
	msm_gpio_irq_extn.irq_disable = msm_mpm_disable_irq;
	msm_gpio_irq_extn.irq_set_type = msm_mpm_set_irq_type;
	msm_gpio_irq_extn.irq_set_wake = msm_mpm_set_irq_wake;

	bitmap_set(msm_mpm_gpio_irqs_mask, NR_MSM_IRQS, NR_GPIO_IRQS);

	if (!mpm_data) {
#ifdef CONFIG_MSM_MPM
		BUG();
#endif
		return;
	}

	memcpy(&msm_mpm_dev_data, mpm_data, sizeof(struct msm_mpm_device_data));

	msm_mpm_dev_data.irqs_m2a =
		kzalloc(msm_mpm_dev_data.irqs_m2a_size * sizeof(uint16_t),
			GFP_KERNEL);
	BUG_ON(!msm_mpm_dev_data.irqs_m2a);
	memcpy(msm_mpm_dev_data.irqs_m2a, mpm_data->irqs_m2a,
		msm_mpm_dev_data.irqs_m2a_size * sizeof(uint16_t));
	msm_mpm_dev_data.bypassed_apps_irqs =
		kzalloc(msm_mpm_dev_data.bypassed_apps_irqs_size *
			sizeof(uint16_t), GFP_KERNEL);
	BUG_ON(!msm_mpm_dev_data.bypassed_apps_irqs);
	memcpy(msm_mpm_dev_data.bypassed_apps_irqs,
		mpm_data->bypassed_apps_irqs,
		msm_mpm_dev_data.bypassed_apps_irqs_size * sizeof(uint16_t));
}

static int __init msm_mpm_init(void)
{
	unsigned int irq = msm_mpm_dev_data.mpm_ipc_irq;
	int rc;

	rc = request_irq(irq, msm_mpm_irq,
			IRQF_TRIGGER_RISING, "mpm_drv", msm_mpm_irq);

	if (rc) {
		pr_err("%s: failed to request irq %u: %d\n",
			__func__, irq, rc);
		goto init_bail;
	}

	rc = irq_set_irq_wake(irq, 1);
	if (rc) {
		pr_err("%s: failed to set wakeup irq %u: %d\n",
			__func__, irq, rc);
		goto init_free_bail;
	}

	msm_mpm_initialized |= MSM_MPM_IRQ_MAPPING_DONE;
	return 0;

init_free_bail:
	free_irq(irq, msm_mpm_irq);

init_bail:
	return rc;
}
arch_initcall(msm_mpm_init);
