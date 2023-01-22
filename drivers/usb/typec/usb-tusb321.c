/* SPDX-License-Identifier: GPL-2.0
 *
 * USB CTYPE driver for TI TUSB321 circuit
 *
 * Author: Hans Christian Lønstad <hcl@datarespons.com>
 *
 * Some code borrowed from drivers/usb/common/usb-conn-gpio.c
 */
#define DEBUG
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>

#define USB_GPIO_DEB_MS		20	/* ms */
#define USB_GPIO_DEB_US		((USB_GPIO_DEB_MS) * 1000)	/* us */

#define USB_CONN_IRQF	\
	(IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT)

struct usb_tusb321_info {
	struct device *dev;
	struct usb_role_switch *role_sw;
	enum usb_role last_role;
	struct regulator *vbus;
	struct delayed_work dw_det;
	unsigned long debounce_jiffies;

	struct gpio_desc *id_gpiod;
	struct gpio_descs *cmode_gpiod;
	int id_irq;
	int attach_irq[2];
};

/*
 *  Role          |   ID  |  CMODE0 | CMODE1
 * ------------------------------------------
 *  [1] DEVICE    |   L   |   L	    |   L
 *  [1] DEVICE    |   L   |   L	    |   H
 *  [1] DEVICE    |   L   |   H	    |   L
 *  [2] NONE      |   L   |   H     |   H
 *  [3] HOST      |   H   |   X     |   X
 *
 */
static void usb_tusb321_detect_cable(struct work_struct *work)
{
	struct usb_tusb321_info *info;
	enum usb_role role;
	int id, ret, cmode[2];

	info = container_of(to_delayed_work(work),
			    struct usb_tusb321_info, dw_det);


	/* check ID and VBUS */
	id = gpiod_get_value_cansleep(info->id_gpiod);
	cmode[0] = gpiod_get_value_cansleep(info->cmode_gpiod->desc[0]);
	cmode[1] = gpiod_get_value_cansleep(info->cmode_gpiod->desc[1]);

	if (id) {
		role = USB_ROLE_HOST;
		if (info->vbus && !regulator_is_enabled(info->vbus)) {
			ret = regulator_enable(info->vbus);
			if (ret < 0) {
				dev_warn(info->dev, "Failed to enable VBUS (%d)\n", ret);
			}
		}
	}
	else {
		if (cmode[0] == 1 && cmode[1] == 1)
			role = USB_ROLE_NONE;
		else
			role = USB_ROLE_DEVICE;
		if (info->vbus && regulator_is_enabled(info->vbus))
			regulator_disable(info->vbus);
	}

	dev_dbg(info->dev, "role %s -> %s, gpios: id %d, out1 %d, out2 %d\n",
		usb_role_string(info->last_role), usb_role_string(role), id, cmode[0], cmode[1]);

	if (info->last_role == role) {
		dev_warn(info->dev, "repeated role: %s\n", usb_role_string(role));
		return;
	}

	ret = usb_role_switch_set_role(info->role_sw, role);
	if (ret)
		dev_err(info->dev, "failed to set role: %d\n", ret);

	info->last_role = role;

	if (info->vbus)
		dev_dbg(info->dev, "vbus regulator is %s\n",
			regulator_is_enabled(info->vbus) ? "enabled" : "disabled");

}

static void usb_tusb321_queue_dwork(struct usb_tusb321_info *info,
				 unsigned long delay)
{
	queue_delayed_work(system_power_efficient_wq, &info->dw_det, delay);
}

static irqreturn_t usb_tusb321_isr(int irq, void *dev_id)
{
	struct usb_tusb321_info *info = dev_id;

	usb_tusb321_queue_dwork(info, info->debounce_jiffies);

	return IRQ_HANDLED;
}

static int usb_tusb321_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_tusb321_info *info;
	bool need_vbus = true;
	int ret = 0;
	int i;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->id_gpiod = devm_gpiod_get(dev, "id", GPIOD_IN);
	if (IS_ERR(info->id_gpiod))
		return PTR_ERR(info->id_gpiod);

	info->cmode_gpiod = devm_gpiod_get_array(dev, "cmode", GPIOD_IN);
	if (IS_ERR(info->cmode_gpiod) || info->cmode_gpiod->ndescs < 2) {
		dev_err(dev, "Failed to get cmode GPIOS\n");
		return -EINVAL;
	}


	gpiod_set_debounce(info->id_gpiod, USB_GPIO_DEB_US);
	info->debounce_jiffies = msecs_to_jiffies(USB_GPIO_DEB_MS);

	INIT_DELAYED_WORK(&info->dw_det, usb_tusb321_detect_cable);

	/*
	 * If the USB connector is a child of a USB port and that port already provides the VBUS
	 * supply, there's no need for the USB connector to provide it again.
	 */
	if (dev->parent && dev->parent->of_node) {
		if (of_find_property(dev->parent->of_node, "vbus-supply", NULL))
			need_vbus = false;
	}

	if (!need_vbus) {
		info->vbus = devm_regulator_get_optional(dev, "vbus");
		if (PTR_ERR(info->vbus) == -ENODEV)
			info->vbus = NULL;
	} else {
		info->vbus = devm_regulator_get(dev, "vbus");
	}

	if (IS_ERR(info->vbus)) {
		ret = PTR_ERR(info->vbus);
		return dev_err_probe(dev, ret, "failed to get vbus :%d\n", ret);
	}

	info->role_sw = usb_role_switch_get(dev);
	if (IS_ERR(info->role_sw))
		return dev_err_probe(dev, PTR_ERR(info->role_sw),
				     "failed to get role switch\n");

	info->id_irq = gpiod_to_irq(info->id_gpiod);
	if (info->id_irq < 0) {
		dev_err(dev, "failed to get ID IRQ\n");
		ret = info->id_irq;
		goto put_role_sw;
	}

	info->attach_irq[0] = gpiod_to_irq(info->cmode_gpiod->desc[0]);
	info->attach_irq[1] = gpiod_to_irq(info->cmode_gpiod->desc[1]);
	if (info->attach_irq[0] < 0 || info->attach_irq[1] < 0) {
		dev_err(dev, "failed to get Attach IRQ\n");
		ret = -EINVAL;
		goto put_role_sw;
	}

	ret = devm_request_threaded_irq(dev, info->id_irq, NULL,
					usb_tusb321_isr, USB_CONN_IRQF,
					pdev->name, info);
	if (ret < 0) {
		dev_err(dev, "failed to request ID IRQ\n");
		goto put_role_sw;
	}

	for (i=0; i < 2; i++) {
		ret = devm_request_threaded_irq(dev, info->attach_irq[i], NULL,
						usb_tusb321_isr, USB_CONN_IRQF,
						pdev->name, info);
		if (ret < 0) {
			dev_err(dev, "failed to request Attach IRQ %d\n", i);
			goto put_role_sw;
		}
	}

	platform_set_drvdata(pdev, info);

	/* Perform initial detection */
	usb_tusb321_queue_dwork(info, 0);

	return 0;

put_role_sw:
	usb_role_switch_put(info->role_sw);
	return ret;
}

static int usb_tusb321_remove(struct platform_device *pdev)
{
	struct usb_tusb321_info *info = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&info->dw_det);

	if (info->last_role == USB_ROLE_HOST && info->vbus)
		regulator_disable(info->vbus);

	usb_role_switch_put(info->role_sw);

	return 0;
}

static int __maybe_unused usb_tusb321_suspend(struct device *dev)
{
	struct usb_tusb321_info *info = dev_get_drvdata(dev);

	disable_irq(info->id_irq);
	disable_irq(info->attach_irq[0]);
	disable_irq(info->attach_irq[1]);

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int __maybe_unused usb_tusb321_resume(struct device *dev)
{
	struct usb_tusb321_info *info = dev_get_drvdata(dev);

	pinctrl_pm_select_default_state(dev);

	enable_irq(info->id_irq);
	enable_irq(info->attach_irq[0]);
	enable_irq(info->attach_irq[1]);

	usb_tusb321_queue_dwork(info, 0);

	return 0;
}

static SIMPLE_DEV_PM_OPS(usb_tusb321_pm_ops,
			 usb_tusb321_suspend, usb_tusb321_resume);

static const struct of_device_id usb_tusb321_dt_match[] = {
	{ .compatible = "ti,tusb321", },
	{ }
};
MODULE_DEVICE_TABLE(of, usb_tusb321_dt_match);

static struct platform_driver usb_tusb321_driver = {
	.probe		= usb_tusb321_probe,
	.remove		= usb_tusb321_remove,
	.driver		= {
		.name	= "usb-tusb321",
		.pm	= &usb_tusb321_pm_ops,
		.of_match_table = usb_tusb321_dt_match,
	},
};

module_platform_driver(usb_tusb321_driver);

MODULE_AUTHOR("Hans Christian Lønstad <hcl@datarespons.com>");
MODULE_DESCRIPTION("USB detector TI TUSB321 support");
MODULE_LICENSE("GPL v2");
