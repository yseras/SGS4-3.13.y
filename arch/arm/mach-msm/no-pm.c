/*
 * Copyright (c) 2010-2011, 2013-2014, The Linux Foundation.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <asm/proc-fns.h>
#include <mach/cpuidle.h>
#include "pm.h"

void arch_idle(void)
{
	cpu_do_idle();
}

void msm_cpu_pm_enter_sleep(enum msm_pm_sleep_mode mode, bool from_idle) {}

void msm_pm_enable_retention(bool enable) {}

