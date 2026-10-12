// SPDX-License-Identifier: GPL-2.0-only
/*
 * Relmon MS35774 STEP/DIR stepper motor driver
 *
 * Minimal driver for a MS35774 that rotates a part over a limited range,
 * e.g. the camera of the Rabbit R1. The position is exposed in degrees
 * through the "orientation" sysfs attribute. The part is homed at probe
 * time by driving it past the full range into its mechanical end stop.
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#define MS35774_MAX_DEGREES		180
#define MS35774_PARK_DEGREES		90
/* Full steps per degree in the microstep mode selected below. */
#define MS35774_STEPS_PER_DEGREE	6
#define MS35774_STEP_HIGH_US		50
#define MS35774_STEP_LOW_US		360
/* MS1 high, MS2 low */
#define MS35774_MODE			0x1

struct ms35774 {
	struct device *dev;
	struct regulator *vm;
	struct gpio_desc *enable;
	struct gpio_desc *dir;
	struct gpio_desc *step;
	struct gpio_desc *powerdown;
	struct gpio_descs *mode;
	struct work_struct work;
	struct mutex lock;		/* protects position, target, homed */
	unsigned int position;
	unsigned int target;
	bool homed;
};

static int ms35774_move(struct ms35774 *priv, unsigned int from,
			unsigned int to)
{
	unsigned int steps;
	int ret;

	if (from == to)
		return 0;

	steps = MS35774_STEPS_PER_DEGREE * abs((int)to - (int)from);

	ret = regulator_enable(priv->vm);
	if (ret) {
		dev_err(priv->dev, "failed to enable motor supply: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(priv->dir, to > from);
	gpiod_set_value_cansleep(priv->enable, 1);

	while (steps--) {
		gpiod_set_value(priv->step, 1);
		udelay(MS35774_STEP_HIGH_US);
		gpiod_set_value(priv->step, 0);
		usleep_range(MS35774_STEP_LOW_US, MS35774_STEP_LOW_US + 40);
	}

	udelay(MS35774_STEP_HIGH_US);
	gpiod_set_value_cansleep(priv->enable, 0);
	regulator_disable(priv->vm);

	return 0;
}

static void ms35774_work(struct work_struct *work)
{
	struct ms35774 *priv = container_of(work, struct ms35774, work);
	unsigned int from, to;

	mutex_lock(&priv->lock);

	if (!priv->homed) {
		/* Assume the far end, so the move ends up in the end stop. */
		mutex_unlock(&priv->lock);
		if (ms35774_move(priv, MS35774_MAX_DEGREES, 0))
			return;
		mutex_lock(&priv->lock);
		priv->position = 0;
		priv->homed = true;
	}

	while (priv->position != priv->target) {
		from = priv->position;
		to = priv->target;
		mutex_unlock(&priv->lock);

		if (ms35774_move(priv, from, to))
			return;

		mutex_lock(&priv->lock);
		priv->position = to;
	}

	mutex_unlock(&priv->lock);
}

static ssize_t orientation_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ms35774 *priv = dev_get_drvdata(dev);
	unsigned int position;

	mutex_lock(&priv->lock);
	position = priv->position;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", position);
}

static ssize_t orientation_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ms35774 *priv = dev_get_drvdata(dev);
	unsigned int target;
	int ret;

	ret = kstrtouint(buf, 10, &target);
	if (ret)
		return ret;

	if (target > MS35774_MAX_DEGREES)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->target = target;
	mutex_unlock(&priv->lock);

	queue_work(system_long_wq, &priv->work);

	return count;
}
static DEVICE_ATTR_RW(orientation);

static struct attribute *ms35774_attrs[] = {
	&dev_attr_orientation.attr,
	NULL
};
ATTRIBUTE_GROUPS(ms35774);

static void ms35774_cancel_work(void *data)
{
	struct ms35774 *priv = data;

	cancel_work_sync(&priv->work);
}

static int ms35774_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	DECLARE_BITMAP(mode, 2) = { MS35774_MODE };
	struct ms35774 *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	priv->vm = devm_regulator_get(dev, "vm");
	if (IS_ERR(priv->vm))
		return dev_err_probe(dev, PTR_ERR(priv->vm),
				     "failed to get motor supply\n");

	priv->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable))
		return dev_err_probe(dev, PTR_ERR(priv->enable),
				     "failed to get enable GPIO\n");

	priv->dir = devm_gpiod_get(dev, "dir", GPIOD_OUT_LOW);
	if (IS_ERR(priv->dir))
		return dev_err_probe(dev, PTR_ERR(priv->dir),
				     "failed to get dir GPIO\n");

	priv->step = devm_gpiod_get(dev, "step", GPIOD_OUT_LOW);
	if (IS_ERR(priv->step))
		return dev_err_probe(dev, PTR_ERR(priv->step),
				     "failed to get step GPIO\n");
	if (gpiod_cansleep(priv->step))
		return dev_err_probe(dev, -EINVAL,
				     "step GPIO must not sleep\n");

	priv->powerdown = devm_gpiod_get_optional(dev, "powerdown",
						  GPIOD_OUT_LOW);
	if (IS_ERR(priv->powerdown))
		return dev_err_probe(dev, PTR_ERR(priv->powerdown),
				     "failed to get powerdown GPIO\n");

	priv->mode = devm_gpiod_get_array_optional(dev, "mode", GPIOD_OUT_LOW);
	if (IS_ERR(priv->mode))
		return dev_err_probe(dev, PTR_ERR(priv->mode),
				     "failed to get mode GPIOs\n");

	if (priv->mode) {
		if (priv->mode->ndescs != 2)
			return dev_err_probe(dev, -EINVAL,
					     "expected 2 mode GPIOs\n");

		ret = gpiod_set_array_value_cansleep(priv->mode->ndescs,
						     priv->mode->desc,
						     priv->mode->info, mode);
		if (ret)
			return ret;
	}

	INIT_WORK(&priv->work, ms35774_work);
	ret = devm_add_action_or_reset(dev, ms35774_cancel_work, priv);
	if (ret)
		return ret;

	priv->target = MS35774_PARK_DEGREES;
	queue_work(system_long_wq, &priv->work);

	return 0;
}

static void ms35774_shutdown(struct platform_device *pdev)
{
	struct ms35774 *priv = platform_get_drvdata(pdev);

	/* Park in the middle of the range, like the stock firmware. */
	mutex_lock(&priv->lock);
	priv->target = MS35774_PARK_DEGREES;
	mutex_unlock(&priv->lock);

	queue_work(system_long_wq, &priv->work);
	flush_work(&priv->work);
}

static int ms35774_suspend(struct device *dev)
{
	struct ms35774 *priv = dev_get_drvdata(dev);

	/*
	 * A move sleeps between steps, and its timers stop while the system
	 * sleeps: finish it rather than sleep with the coils energised.
	 */
	flush_work(&priv->work);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ms35774_pm_ops, ms35774_suspend, NULL);

static const struct of_device_id ms35774_of_match[] = {
	{ .compatible = "relmon,ms35774" },
	{ }
};
MODULE_DEVICE_TABLE(of, ms35774_of_match);

static struct platform_driver ms35774_driver = {
	.probe = ms35774_probe,
	.shutdown = ms35774_shutdown,
	.driver = {
		.name = "ms35774",
		.of_match_table = ms35774_of_match,
		.dev_groups = ms35774_groups,
		.pm = pm_sleep_ptr(&ms35774_pm_ops),
	},
};
module_platform_driver(ms35774_driver);

MODULE_DESCRIPTION("Relmon MS35774 stepper motor driver");
MODULE_LICENSE("GPL");
