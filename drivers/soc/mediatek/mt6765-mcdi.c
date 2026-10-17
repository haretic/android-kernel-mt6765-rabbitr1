// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6765 MCDI: CPU core power-down on behalf of PSCI.
 *
 * The stock ATF does not power a core off itself. For CPU_SUSPEND and
 * CPU_OFF it parks the core in "wfi; b ." and leaves the power-down to the
 * SSPM's MCDI task, which only manages the CPUs listed in its mailbox
 * (AVAIL_CPU_MASK, zero after boot), and to the MTK POWER_DOWN_CORE SiP call
 * for CPUs that went offline. Without them a core that idles in cpu-off never
 * returns and an offlined CPU can never be brought back.
 *
 * This follows the stock kernel (mcdi_v1, SSPM interface): list the online
 * CPUs and power down dead CPUs. Around a hotplug the SSPM task keeps running
 * and only the deep states are held off, as stock does: a CPU that executes
 * CPU_OFF while the task is paused is never powered down, and that reset the
 * board.
 * Until "enable" is set, deep idle states stay off: a CPU latency request
 * from early boot, which the governor honours, and the states' driver-disable
 * bit, which suspend-to-idle honours too.
 */

#include <linux/arm-smccc.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/cpuhotplug.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>

/* SSPM mailbox 3 slots, 4 bytes each (stock mt6765/mtk_mcdi_reg.h). */
#define MCDI_PAUSE_ACTION	6
#define MCDI_AVAIL_CPU_MASK	7
#define MCDI_CPU_ISO_MASK	12
#define MCDI_PAUSE_ACK		13

#define MTK_SIP_POWER_DOWN_CORE	0xc2000212

static void __iomem *mcdi_mbox;
static DEFINE_MUTEX(mcdi_lock);
static bool mcdi_enabled;
static struct pm_qos_request mcdi_gate;
/* CPUs between cpu_pm_enter() and cpu_pm_exit(), i.e. in a power-down state. */
static atomic_t mcdi_deep = ATOMIC_INIT(0);

static void mcdi_write(unsigned int slot, u32 value)
{
	writel(value, mcdi_mbox + slot * 4);
}

static int mcdi_pause(bool pause)
{
	u32 ack;
	int ret;

	mcdi_write(MCDI_PAUSE_ACTION, pause);
	ret = readl_poll_timeout(mcdi_mbox + MCDI_PAUSE_ACK * 4, ack,
				 ack == pause, 10, 10000);
	if (ret)
		pr_warn_ratelimited("mt6765-mcdi: SSPM did not %s (ack %u)\n",
				    pause ? "pause" : "resume", ack);
	return ret;
}

/* Every state past WFI powers the core down. */
static void mcdi_gate_states(bool gate)
{
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		drv = dev ? cpuidle_get_cpu_driver(dev) : NULL;
		if (!drv)
			continue;
		for (i = 1; i < drv->state_count; i++) {
			if (gate)
				dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_DRIVER;
			else
				dev->states_usage[i].disable &= ~CPUIDLE_STATE_DISABLED_BY_DRIVER;
		}
	}
}

static int mcdi_cpu_pm_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	if (action == CPU_PM_ENTER)
		atomic_inc(&mcdi_deep);
	else if (action == CPU_PM_EXIT || action == CPU_PM_ENTER_FAILED)
		atomic_dec(&mcdi_deep);
	return NOTIFY_OK;
}

static struct notifier_block mcdi_cpu_pm_nb = {
	.notifier_call = mcdi_cpu_pm_notify,
};

/* WFI only from here on, and every CPU back up from a power-down state. */
static void mcdi_quiesce(void)
{
	int deep;

	cpu_latency_qos_update_request(&mcdi_gate, 0);
	wake_up_all_idle_cpus();
	if (read_poll_timeout(atomic_read, deep, !deep, 10, 10000, false, &mcdi_deep))
		pr_warn_ratelimited("mt6765-mcdi: %d CPUs still powered down\n", deep);
}

/* Callers hold mcdi_lock; the online mask is stable in hotplug callbacks. */
static void mcdi_write_mask(void)
{
	u32 mask = 0;
	unsigned int cpu;

	for_each_online_cpu(cpu)
		mask |= BIT(cpu);
	mcdi_write(MCDI_AVAIL_CPU_MASK, mask);
}

/* Hold off deep idle before a CPU goes up or down, update the mask once it has. */
static int mcdi_hotplug_enter(unsigned int cpu)
{
	mutex_lock(&mcdi_lock);
	if (mcdi_enabled)
		mcdi_quiesce();
	mutex_unlock(&mcdi_lock);
	return 0;
}

static int mcdi_hotplug_leave(unsigned int cpu)
{
	mutex_lock(&mcdi_lock);
	if (mcdi_enabled) {
		mcdi_write_mask();
		cpu_latency_qos_update_request(&mcdi_gate, PM_QOS_DEFAULT_VALUE);
	}
	mutex_unlock(&mcdi_lock);
	return 0;
}

/* Runs on a control CPU once @cpu is dead and has left the SSPM's mask. */
static int mcdi_cpu_dead(unsigned int cpu)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CORE, cpu, 0, 0, 0, 0, 0, 0, &res);
	return 0;
}

static int mcdi_set_enable(bool enable)
{
	cpus_read_lock();
	mutex_lock(&mcdi_lock);
	if (enable == mcdi_enabled)
		goto out;
	if (enable) {
		if (mcdi_pause(true))
			goto out;
		mcdi_write(MCDI_CPU_ISO_MASK, 0);
		mcdi_write_mask();
		mcdi_pause(false);
		mcdi_enabled = true;
		mcdi_gate_states(false);
	} else {
		mcdi_gate_states(true);
		/* Every CPU out of cpu-off before the SSPM lets go. */
		mcdi_quiesce();
		mcdi_pause(true);
		mcdi_write(MCDI_AVAIL_CPU_MASK, 0);
		mcdi_pause(false);
		mcdi_enabled = false;
	}
out:
	mutex_unlock(&mcdi_lock);
	cpus_read_unlock();
	if (enable && mcdi_enabled)
		cpu_latency_qos_update_request(&mcdi_gate, PM_QOS_DEFAULT_VALUE);
	return enable == mcdi_enabled ? 0 : -EIO;
}

static int mcdi_enable_set(const char *val, const struct kernel_param *kp)
{
	bool enable;
	int ret;

	ret = kstrtobool(val, &enable);
	if (ret)
		return ret;
	if (!mcdi_mbox)
		return -ENODEV;
	return mcdi_set_enable(enable);
}

static int mcdi_enable_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%c\n", mcdi_enabled ? 'Y' : 'N');
}

static const struct kernel_param_ops mcdi_enable_ops = {
	.set = mcdi_enable_set,
	.get = mcdi_enable_get,
};
module_param_cb(enable, &mcdi_enable_ops, NULL, 0644);
MODULE_PARM_DESC(enable, "Let the SSPM power idle CPU cores off");

static int mt6765_mcdi_probe(struct platform_device *pdev)
{
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	mcdi_mbox = base;

	ret = cpu_pm_register_notifier(&mcdi_cpu_pm_nb);
	if (ret)
		return ret;

	/*
	 * Dynamic states tear down from the last allocated: a dying CPU leaves
	 * the mask and the MCDI task resumes before its core is powered down.
	 */
	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "soc/mt6765-mcdi:dead",
					NULL, mcdi_cpu_dead);
	if (ret < 0)
		return ret;
	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "soc/mt6765-mcdi:prepare",
					mcdi_hotplug_enter, mcdi_hotplug_leave);
	if (ret < 0)
		return ret;
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "soc/mt6765-mcdi:online",
					mcdi_hotplug_leave, mcdi_hotplug_enter);
	return ret < 0 ? ret : 0;
}

static const struct of_device_id mt6765_mcdi_of_match[] = {
	{ .compatible = "mediatek,mt6765-mcdi" },
	{ }
};

static struct platform_driver mt6765_mcdi_driver = {
	.probe = mt6765_mcdi_probe,
	.driver = {
		.name = "mt6765-mcdi",
		.of_match_table = mt6765_mcdi_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6765_mcdi_driver);

/* Before cpuidle registers: only WFI until the SSPM manages the cores. */
static int __init mt6765_mcdi_gate(void)
{
	if (of_machine_is_compatible("mediatek,mt6765"))
		cpu_latency_qos_add_request(&mcdi_gate, 0);
	return 0;
}
early_initcall(mt6765_mcdi_gate);

/* cpuidle has registered by now; suspend-to-idle ignores latency requests. */
static int __init mt6765_mcdi_gate_states(void)
{
	mutex_lock(&mcdi_lock);
	if (cpu_latency_qos_request_active(&mcdi_gate) && !mcdi_enabled)
		mcdi_gate_states(true);
	mutex_unlock(&mcdi_lock);
	return 0;
}
late_initcall(mt6765_mcdi_gate_states);
