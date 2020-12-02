/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <mali_kbase.h>
#include "mali_kbase_config_platform.h"
#include <mali_kbase_defs.h>

enum gpu_clk_idx {main, sub, mux, cg};
/* list of clocks required by GPU */
static const char * const gpu_clocks[] = {
	"clk_main_parent", "clk_sub_parent", "clk_mux", "subsys_mfg_cg",
};

static void pm_domain_term(struct kbase_device *kbdev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kbdev->pm_domain_devs); i++) {
		if (kbdev->pm_domain_devs[i])
			dev_pm_domain_detach(kbdev->pm_domain_devs[i], true);
	}
}

static int pm_domain_init(struct kbase_device *kbdev)
{
	int err;
	int i, num_domains, num_domain_names;
	const char *pd_names[GPU_CORE_NUM];

	num_domains = of_count_phandle_with_args(kbdev->dev->of_node,
						 "power-domains",
						 "#power-domain-cells");

	num_domain_names = of_property_count_strings(kbdev->dev->of_node,
					"power-domain-names");

	/*
	 * Single domain is handled by the core, and, if only a single power
	 * the power domain is requested, the property is optional.
	 */
	if (num_domains < 2 && kbdev->num_pm_domains < 2)
		return 0;

	if (num_domains != num_domain_names) {
		dev_err(kbdev->dev,
			"Device tree power domains are not match: PD %d, PD names %d\n",
			num_domains, num_domain_names);
		return -EINVAL;
	}

	if (num_domains != kbdev->num_pm_domains) {
		dev_err(kbdev->dev,
			"Incorrect number of power domains: %d provided, %d needed\n",
			num_domains, kbdev->num_pm_domains);
		return -EINVAL;
	}

	if (WARN(num_domains > ARRAY_SIZE(kbdev->pm_domain_devs),
			"Too many supplies in compatible structure.\n"))
		return -EINVAL;

	err = of_property_read_string_array(kbdev->dev->of_node,
					    "power-domain-names",
					    pd_names,
					    num_domain_names);

	if (err < 0) {
		dev_err(kbdev->dev, "Error reading supply-names: %d\n", err);
		return err;
	}

	for (i = 0; i < num_domains; i++) {
		kbdev->pm_domain_devs[i] =
			dev_pm_domain_attach_by_name(kbdev->dev,
					pd_names[i]);
		if (IS_ERR_OR_NULL(kbdev->pm_domain_devs[i])) {
			err = PTR_ERR(kbdev->pm_domain_devs[i]) ? : -ENODATA;
			kbdev->pm_domain_devs[i] = NULL;
			if (err == -EPROBE_DEFER) {
				dev_dbg(kbdev->dev,
					"Probe deferral for pm-domain %s(%d)\n",
					pd_names[i], i);
			} else {
				dev_err(kbdev->dev,
					"failed to get pm-domain %s(%d): %d\n",
					pd_names[i], i, err);
			}
			goto err;
		}
	}

	return 0;

err:
	pm_domain_term(kbdev);
	return err;
}

static void check_bus_idle(struct kbase_device *kbdev)
{
	struct mfg_base *mfg = kbdev->platform_context;
	u32 val;

	/* MFG_QCHANNEL_CON (0x13fb_f0b4) bit [1:0] = 0x1 */
	writel(0x00000001, mfg->g_mfg_base + MFG_QCHANNEL_CON);

	/* set register MFG_DEBUG_SEL (0x13fb_f170) bit [7:0] = 0x03 */
	writel(0x00000003, mfg->g_mfg_base + MFG_DEBUG_SEL);

	/* polling register MFG_DEBUG_TOP (0x13fb_f178) bit 2 = 0x1 */
	/* => 1 for bus idle, 0 for bus non-idle */
	do {
		val = readl(mfg->g_mfg_base + MFG_DEBUG_TOP);
	} while ((val & BUS_IDLE_BIT) != BUS_IDLE_BIT);
}

static void *get_mfg_base(const char *node_name)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, node_name);

	if (node)
		return of_iomap(node, 0);

	return NULL;
}

static int pm_callback_power_on(struct kbase_device *kbdev)
{
	int error, i;
	struct mfg_base *mfg = kbdev->platform_context;

	if (mfg->is_powered) {
		dev_dbg(kbdev->dev, "mali_device is already powered\n");
		return 0;
	}

	for (i = 0; i < kbdev->nr_regulators; i++) {
		error = regulator_enable(kbdev->regulators[i]);
		if (error < 0) {
			dev_err(kbdev->dev,
				"Power on reg %d failed error = %d\n",
				i, error);
			return error;
		}
	}

	for (i = 0; i < kbdev->num_pm_domains; i++) {
		error = pm_runtime_get_sync(kbdev->pm_domain_devs[i]);
		if (error < 0) {
			dev_err(kbdev->dev,
				"Power on core %d failed (err: %d)\n",
				i+1, error);
			return error;
		}
	}

	error = clk_bulk_prepare_enable(mfg->num_clks, mfg->clks);
	if (error < 0) {
		dev_err(kbdev->dev,
			"gpu clock enable failed (err: %d)\n",
			error);
		return error;
	}

	mfg->is_powered = true;

	return 1;
}

static void pm_callback_power_off(struct kbase_device *kbdev)
{
	struct mfg_base *mfg = kbdev->platform_context;
	int error, i;

	if (!mfg->is_powered) {
		dev_dbg(kbdev->dev, "mali_device is already powered off\n");
		return;
	}

	mfg->is_powered = false;

	check_bus_idle(kbdev);

	clk_bulk_disable_unprepare(mfg->num_clks, mfg->clks);

	for (i = kbdev->num_pm_domains - 1; i >= 0; i--) {
		pm_runtime_mark_last_busy(kbdev->pm_domain_devs[i]);
		error = pm_runtime_put_autosuspend(kbdev->pm_domain_devs[i]);
		if (error < 0)
			dev_err(kbdev->dev,
				"Power off core %d failed (err: %d)\n",
				i+1, error);
	}

	for (i = kbdev->nr_regulators - 1; i >= 0; i--) {
		error = regulator_disable(kbdev->regulators[i]);
		if (error < 0)
			dev_err(kbdev->dev,
				"Power off reg %d failed error = %d\n",
				i, error);
	}
}

static int kbase_device_runtime_init(struct kbase_device *kbdev)
{
	dev_dbg(kbdev->dev, "%s\n", __func__);

	return 0;
}

static void kbase_device_runtime_disable(struct kbase_device *kbdev)
{
	dev_dbg(kbdev->dev, "%s\n", __func__);
}

static int pm_callback_runtime_on(struct kbase_device *kbdev)
{
	return 0;
}

static void pm_callback_runtime_off(struct kbase_device *kbdev)
{
}

static void pm_callback_resume(struct kbase_device *kbdev)
{
	pm_callback_power_on(kbdev);
}

static void pm_callback_suspend(struct kbase_device *kbdev)
{
	pm_callback_power_off(kbdev);
}

struct kbase_pm_callback_conf pm_callbacks = {
	.power_on_callback = pm_callback_power_on,
	.power_off_callback = pm_callback_power_off,
	.power_suspend_callback = pm_callback_suspend,
	.power_resume_callback = pm_callback_resume,
#ifdef KBASE_PM_RUNTIME
	.power_runtime_init_callback = kbase_device_runtime_init,
	.power_runtime_term_callback = kbase_device_runtime_disable,
	.power_runtime_on_callback = pm_callback_runtime_on,
	.power_runtime_off_callback = pm_callback_runtime_off,
#else				/* KBASE_PM_RUNTIME */
	.power_runtime_init_callback = NULL,
	.power_runtime_term_callback = NULL,
	.power_runtime_on_callback = NULL,
	.power_runtime_off_callback = NULL,
#endif				/* KBASE_PM_RUNTIME */
};


int mali_mfgsys_init(struct kbase_device *kbdev, struct mfg_base *mfg)
{
	int err, i;
	unsigned long volt;

	kbdev->num_pm_domains = GPU_CORE_NUM;

	err = pm_domain_init(kbdev);
	if (err < 0)
		return err;

	for (i = 0; i < kbdev->nr_regulators; i++)
		if (kbdev->regulators[i] == NULL)
			return -EINVAL;

	mfg->num_clks = ARRAY_SIZE(gpu_clocks);
	mfg->clks = devm_kcalloc(kbdev->dev, mfg->num_clks,
				     sizeof(*mfg->clks), GFP_KERNEL);

	if (!mfg->clks)
		return -ENOMEM;

	for (i = 0; i < mfg->num_clks; ++i)
		mfg->clks[i].id = gpu_clocks[i];

	err = devm_clk_bulk_get(kbdev->dev, mfg->num_clks, mfg->clks);
	if (err != 0) {
		dev_err(kbdev->dev,
			"clk_bulk_get error: %d\n",
			err);
		return err;
	}

	for (i = 0; i < kbdev->nr_regulators; i++) {
		volt = (i == 0) ? VGPU_MAX_VOLT : VSRAM_GPU_MAX_VOLT;
		err = regulator_set_voltage(kbdev->regulators[i],
			volt, volt + VOLT_TOL);
		if (err < 0) {
			dev_err(kbdev->dev,
				"Regulator %d set voltage failed: %d\n",
				i, err);
			return err;
		}
		kbdev->current_voltages[i] = volt;
	}

	mfg->g_mfg_base = get_mfg_base("mediatek,mt8192-mfgcfg");
	if (!mfg->g_mfg_base) {
		dev_err(kbdev->dev, "Cannot find mfgcfg node\n");
		return -ENODEV;
	}

	mfg->is_powered = false;

	return 0;
}

static void voltage_range_check(struct kbase_device *kbdev,
				unsigned long *voltages)
{
	if (voltages[1] - voltages[0] < MIN_VOLT_BIAS ||
	    voltages[1] - voltages[0] > MAX_VOLT_BIAS)
		voltages[1] = voltages[0] + MIN_VOLT_BIAS;
	voltages[1] = clamp_t(unsigned long, voltages[1], VSRAM_GPU_MIN_VOLT,
			      VSRAM_GPU_MAX_VOLT);
}

static int set_frequency(struct kbase_device *kbdev, unsigned long freq)
{
	int err;
	struct mfg_base *mfg = kbdev->platform_context;

	if (kbdev->current_freqs[0] != freq) {
		err = clk_set_parent(mfg->clks[mux].clk, mfg->clks[sub].clk);
		if (err) {
			dev_err(kbdev->dev, "Failed to select sub clock src\n");
			return err;
		}

		err = clk_set_rate(mfg->clks[main].clk, freq);
		if (err) {
			dev_err(kbdev->dev,
				"Failed to set clock rate: %lu (err: %d)\n",
				freq, err);
			return err;
		}
		kbdev->current_freqs[0] = freq;

		err = clk_set_parent(mfg->clks[mux].clk, mfg->clks[main].clk);
		if (err) {
			dev_err(kbdev->dev, "Failed to select main clock src\n");
			return err;
		}
	}

	return 0;
}

static int platform_init(struct kbase_device *kbdev)
{
	int err, i;
	struct mfg_base *mfg;

	mfg = devm_kzalloc(kbdev->dev, sizeof(*mfg), GFP_KERNEL);
	if (!mfg)
		return -ENOMEM;

	err = mali_mfgsys_init(kbdev, mfg);
	if (err)
		return err;

	kbdev->platform_context = mfg;
	for (i = 0; i < kbdev->num_pm_domains; i++) {
		pm_runtime_set_autosuspend_delay(kbdev->pm_domain_devs[i], 50);
		pm_runtime_use_autosuspend(kbdev->pm_domain_devs[i]);
	}

	err = clk_set_parent(mfg->clks[mux].clk, mfg->clks[sub].clk);
	if (err) {
		dev_err(kbdev->dev, "Failed to select sub clock src\n");
		return err;
	}

	err = clk_set_rate(mfg->clks[main].clk, GPU_FREQ_KHZ_MAX * 1000);
	if (err) {
		dev_err(kbdev->dev, "Failed to set clock %d kHz\n",
			GPU_FREQ_KHZ_MAX);
		return err;
	}

	err = clk_set_parent(mfg->clks[mux].clk, mfg->clks[main].clk);
	if (err) {
		dev_err(kbdev->dev, "Failed to select main clock src\n");
		return err;
	}

	kbdev->devfreq_ops.set_frequency = set_frequency;
	kbdev->devfreq_ops.voltage_range_check = voltage_range_check;

	return 0;
}

static void platform_term(struct kbase_device *kbdev)
{
	kbdev->platform_context = NULL;
	pm_domain_term(kbdev);
}

struct kbase_platform_funcs_conf platform_funcs = {
	.platform_init_func = platform_init,
	.platform_term_func = platform_term
};
