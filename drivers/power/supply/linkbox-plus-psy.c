#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

struct ina2xx_data {
	int power_uW;
	int current_uA;
	int voltage_uV;
};
typedef enum _startupCode {
	START_REBOOT = 0,
	START_ADAPTER = 1,
	START_KEY = 2,
	START_INVALID = 3
} startupCode;

static char *startup_code_text[4] = {
	"reboot",
	"dcin",
	"key",
	"unknown"
};

typedef enum _amp_state { AMP_OFF, AMP_ON, AMP_AUTO, AMP_INVALID } ampState;

static struct of_device_id linkbox_plus_psy_dt_ids[] = {
	{ .compatible = "datarespons,linkbox-plus-psy",  .data = 0, },
	{}
};
MODULE_DEVICE_TABLE(of, linkbox_plus_psy_dt_ids);

#define VALID_MASK_DCIN 0x1
#define VALID_MASK_BAT1 0x2
#define VALID_MASK_BAT2 0x4

struct lbp_priv {
	struct platform_device *pdev;
	struct i2c_adapter *i2c_adapter;
	struct power_supply *ps_dcin;
	struct power_supply *ps_manikin[2];
	bool manikin_12v_power_on;
	bool manikin_5v_power_on;
	struct gpio_desc *gpio_12v_manikin[2];
	bool gpio_12v_manikin_active_low[2];
	struct gpio_desc *gpio_5v_manikin;
	bool gpio_5v_manikin_active_low;
	struct gpio_desc *gpio_bat_det[2];
	int battery_detect_irqs[2];
	int current_bat_det_irq;
	struct gpio_desc *gpio_bat_disable[2];
	bool bat_disable[2];
	struct gpio_desc *gpio_bat_ce[2];
	bool gpio_bat_ce_active_low[2];
	bool bat_ce[2];;
	struct gpio_desc *gpio_valid[3];
	bool gpio_valid_active_low[3];
	struct gpio_desc *gpio_5w_sd;
	bool gpio_5w_sd_active_low;
	struct gpio_desc *gpio_spkr_sd;
	bool gpio_spkr_sd_active_low;
	struct gpio_desc *gpio_startup_code[2];
	struct gpio_desc *gpio_clr_status;
	bool gpio_clr_status_active_low;
	struct notifier_block ps_dcin_nb;
	ampState amp5w_state;
	ampState amp1w_state;
	struct delayed_work bat_work;

};

static startupCode get_startup(struct lbp_priv *priv)
{
	int val = gpiod_get_value(priv->gpio_startup_code[0]) ? 1 : 0;
	if (gpiod_get_value(priv->gpio_startup_code[1]))
		val |= 2;
	return (startupCode)(val & 3);
}

static int get_valids(struct lbp_priv *priv)
{
	int status=0;
	int n=0;
	for (n=0; n < 3; n++) {
		status |= gpiod_get_value(priv->gpio_valid[n]) << n;
	}
	return status;
}

static enum power_supply_property dcin_props_lb[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property manikin_12v_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};


static enum power_supply_property manikin_5v_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int lm_pmu_mains_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct lbp_priv *priv = power_supply_get_drvdata(psy);
	int valids = get_valids(priv);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = valids & VALID_MASK_DCIN ? 1 : 0;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int lm_pmu_manikin_12v_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct lbp_priv *priv = power_supply_get_drvdata(psy);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = priv->manikin_12v_power_on ? 1 : 0;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int lm_pmu_manikin_5v_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct lbp_priv *priv = power_supply_get_drvdata(psy);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = priv->manikin_5v_power_on ? 1 : 0;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int lm_pmu_manikin_prop_is_writable(struct power_supply *psy,
				     enum power_supply_property psp)
{
	switch (psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		return 1;
		break;

	default:
		return 0;
		break;
	}
}

static int lm_pmu_manikin_12v_set_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	struct lbp_priv *priv = power_supply_get_drvdata(psy);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		if (val->intval) {
			gpiod_set_value(priv->gpio_12v_manikin[0], 1);
			msleep(2);
			if (!IS_ERR(priv->gpio_12v_manikin[1]))
				gpiod_set_value(priv->gpio_12v_manikin[1], 1);
			if (priv->amp5w_state == AMP_AUTO) {
				msleep(10);
				gpiod_set_value(priv->gpio_5w_sd, 0);
			}
		} else  {
			if (priv->amp5w_state == AMP_AUTO)
				gpiod_set_value(priv->gpio_5w_sd, 1);
			if (!IS_ERR(priv->gpio_12v_manikin[1]))
				gpiod_set_value(priv->gpio_12v_manikin[1], 0);
			gpiod_set_value(priv->gpio_12v_manikin[0], 0);

		}
		priv->manikin_12v_power_on = val->intval ? true : false;
		power_supply_changed(psy);
		break;

	default:
		return -EPERM;
	}

	return 0;
}

static int lm_pmu_manikin_5v_set_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	struct lbp_priv *priv = power_supply_get_drvdata(psy);
	priv->manikin_5v_power_on = val->intval ? true : false;
	switch(psp)
	{
	case POWER_SUPPLY_PROP_ONLINE:
		if (val->intval) {
			gpiod_set_value(priv->gpio_5v_manikin, 1);
			if (priv->amp1w_state == AMP_AUTO) {
				msleep(5);
				gpiod_set_value(priv->gpio_spkr_sd, 0);
			}
		} else {
			if (priv->amp1w_state == AMP_AUTO)
				gpiod_set_value(priv->gpio_spkr_sd, 1);
			gpiod_set_value(priv->gpio_5v_manikin, 0);
			priv->manikin_5v_power_on = val->intval ? true : false;
		}
		power_supply_changed(psy);
		break;

	default:
		return -EPERM;
	}

	return 0;
}

static int lm_pmu_notifier_call(struct notifier_block *nb,
		unsigned long val, void *v)
{
	struct power_supply *psy = v;

	if (strncmp(psy->desc->name, "ds2781-battery", 14) == 0) {
		pr_info( "Found %s to %d\n", psy->desc->name, (int)val);

	}
	return 0;
}

static ssize_t lm_pmu_show_bat_det(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	int valids = get_valids(priv);

	if (strcmp(attr->attr.name, "bat_det1") == 0) {
		return sprintf(buf, "%d\n", gpiod_get_value(priv->gpio_bat_det[0]));
	}
	if (strcmp(attr->attr.name, "bat_det2") == 0) {
		return sprintf(buf, "%d\n", gpiod_get_value(priv->gpio_bat_det[1]));
	}
	if (strcmp(attr->attr.name, "bat_valid1") == 0) {
		return sprintf(buf, "%d\n", (valids & VALID_MASK_BAT1) ? 1 : 0);
	}

	if (strcmp(attr->attr.name, "bat_valid2") == 0) {
		return sprintf(buf, "%d\n", (valids & VALID_MASK_BAT2) ? 1 : 0);
	}
	return -EINVAL;
}

static ssize_t lm_pmu_show_bat_disable(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);

	if (strcmp(attr->attr.name, "bat_disable1") == 0) {
		return sprintf(buf, "%d\n", priv->bat_disable[0]);
	}
	if (strcmp(attr->attr.name, "bat_disable2") == 0) {
		return sprintf(buf, "%d\n", priv->bat_disable[1]);
	}
	return -EINVAL;
}

static ssize_t lm_pmu_set_bat_disable(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	int valids = get_valids(priv);
	bool disable = buf[0] == '1' ? 1 : 0;
	int n = -1;
	int mask;
	if (strcmp(attr->attr.name, "bat_disable1") == 0) {
		mask = VALID_MASK_DCIN | VALID_MASK_BAT2;
		n = 0;
	}
	else if (strcmp(attr->attr.name, "bat_disable2") == 0) {
		mask = VALID_MASK_DCIN | VALID_MASK_BAT1;
		n = 1;
	}
	if ( n < 0 )
		return -EINVAL;

	if (disable) {
		if (valids & mask) {
			gpiod_set_value(priv->gpio_bat_disable[n], 1);
			priv->bat_disable[n] = true;
			return count;
		}
		else {
			dev_warn(dev, "Can not turn off all power sources\n");
			return -EINVAL;
		}
	}
	else {
		priv->bat_disable[n] = false;
		gpiod_set_value(priv->gpio_bat_disable[n], 0);
		return count;
	}
	return -EINVAL;
}



static ssize_t lm_pmu_show_startup(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	startupCode c = get_startup(priv);
	if (strcmp(attr->attr.name, "startup_code") == 0) {
		return sprintf(buf, "%s\n", startup_code_text[c]);
	}
	return -EINVAL;
}

static ssize_t lm_pmu_show_bat_ce(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);

	if (strcmp(attr->attr.name, "bat_ce1") == 0) {
		return sprintf(buf, "%d\n", priv->bat_ce[0]);
	}
	if (strcmp(attr->attr.name, "bat_ce2") == 0) {
		return sprintf(buf, "%d\n", priv->bat_ce[1]);
	}
	return -EINVAL;
}

static ssize_t lm_pmu_set_bat_ce(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	bool enable = buf[0] == '1' ? 1 : 0;
	if (strcmp(attr->attr.name, "bat_ce1") == 0) {
		gpiod_set_value(priv->gpio_bat_ce[0], enable);
		priv->bat_ce[0] = enable;
		return count;
	}
	else if (strcmp(attr->attr.name, "bat_ce2") == 0) {
		gpiod_set_value(priv->gpio_bat_ce[1], enable);
		priv->bat_ce[1] = enable;
		return count;
	}

	return -EINVAL;
}

static ampState lm_pmu_get_ampstate(const char *buf, size_t count)
{
	if (strncmp(buf, "auto", 4) == 0)
		return AMP_AUTO;
	if (strncmp(buf, "on", 2) == 0)
			return AMP_ON;
	if (strncmp(buf, "off", 3) == 0)
			return AMP_OFF;
	return AMP_INVALID;
}

static ssize_t lm_pmu_show_amps(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	ampState s;
	if (strcmp(attr->attr.name, "amp5w_enable") == 0) {
		s = priv->amp5w_state;
	}
	else if (strcmp(attr->attr.name, "amp1w_enable") == 0) {
		s = priv->amp1w_state;
	}
	else
		return -EINVAL;

	switch (s)
	{
	case AMP_OFF:
		return sprintf(buf, "off\n");
		break;

	case AMP_ON:
		return sprintf(buf, "on\n");
		break;

	case AMP_AUTO:
		return sprintf(buf, "auto\n");
		break;

	default:
		return sprintf(buf, "invalid\n");
		break;
	}

}

static ssize_t lm_pmu_set_amps(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct lbp_priv *priv = dev_get_drvdata(dev->parent);
	ampState s = lm_pmu_get_ampstate(buf, count);
	int enable;
	if (s == AMP_INVALID)
		return -EINVAL;

	if (strcmp(attr->attr.name, "amp5w_enable") == 0) {
		enable = (s == AMP_ON || (s == AMP_AUTO && priv->manikin_12v_power_on)) ? 0 : 1;
		gpiod_set_value(priv->gpio_5w_sd, enable);
		priv->amp5w_state = s;
		return count;
	}
	else if (strcmp(attr->attr.name, "amp1w_enable") == 0) {
		enable = (s == AMP_ON || (s == AMP_AUTO && priv->manikin_5v_power_on)) ? 0 : 1;
		gpiod_set_value(priv->gpio_spkr_sd, enable);
		priv->amp1w_state = s;
		return count;
	}

	return -EINVAL;
}



static DEVICE_ATTR(bat_det1, S_IRUGO, lm_pmu_show_bat_det, NULL);
static DEVICE_ATTR(bat_det2, S_IRUGO, lm_pmu_show_bat_det, NULL);
static DEVICE_ATTR(bat_disable1, 0644, lm_pmu_show_bat_disable, lm_pmu_set_bat_disable);
static DEVICE_ATTR(bat_disable2, 0644, lm_pmu_show_bat_disable, lm_pmu_set_bat_disable);
static DEVICE_ATTR(bat_valid1, S_IRUGO, lm_pmu_show_bat_det, NULL);
static DEVICE_ATTR(bat_valid2, S_IRUGO, lm_pmu_show_bat_det, NULL);
static DEVICE_ATTR(bat_ce1, 0644, lm_pmu_show_bat_ce, lm_pmu_set_bat_ce);
static DEVICE_ATTR(bat_ce2, 0644, lm_pmu_show_bat_ce, lm_pmu_set_bat_ce);
static DEVICE_ATTR(startup_code, S_IRUGO, lm_pmu_show_startup, NULL);


static struct attribute *bat_sysfs_attr[] = {
	&dev_attr_bat_det1.attr,
	&dev_attr_bat_det2.attr,
	&dev_attr_bat_disable1.attr,
	&dev_attr_bat_disable2.attr,
	&dev_attr_bat_valid1.attr,
	&dev_attr_bat_valid2.attr,
	&dev_attr_bat_ce1.attr,
	&dev_attr_bat_ce2.attr,
	&dev_attr_startup_code.attr,
	NULL,
};

static const struct attribute_group bat_sysfs_attr_group = {
	.attrs = bat_sysfs_attr,
};

static DEVICE_ATTR(amp5w_enable, 0644, lm_pmu_show_amps, lm_pmu_set_amps);
static DEVICE_ATTR(amp1w_enable, 0644, lm_pmu_show_amps, lm_pmu_set_amps);


static irqreturn_t bat_det_handler(int irq, void *_ptr)
{
	struct lbp_priv *priv = _ptr;
	priv->current_bat_det_irq = irq;
	if (!delayed_work_pending(&priv->bat_work))
		schedule_delayed_work(&priv->bat_work, msecs_to_jiffies(100));
	return IRQ_HANDLED;
}

static int lm_pmu_setup_irq(struct lbp_priv *priv)
{
	int n;
	for(n=0; n < 2; n++)
		if (!IS_ERR(priv->gpio_bat_det[n])) {
			priv->battery_detect_irqs[n] = gpiod_to_irq(priv->gpio_bat_det[n]);
			if ( devm_request_irq(&priv->pdev->dev, priv->battery_detect_irqs[n],
					bat_det_handler, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
					"linkbox-plus-psy", priv ) < 0) {
				dev_err(&priv->pdev->dev, "Failed to get irq for gpio_bat_det[%d]\n", n);
			}
		}
	return 0;
}

static void bat_worker(struct work_struct *ws)
{
	struct lbp_priv *priv = container_of(ws, struct lbp_priv, bat_work.work);
	char *envp[4];
	int bat_changed = priv->current_bat_det_irq == priv->battery_detect_irqs[0] ? 0 : 1;
	char *buf = kzalloc(128, GFP_ATOMIC);
	envp[0] = "NAME=LB_BAT_DETECT";
	envp[1] = buf;
	envp[2] = &buf[64];
	envp[3] = NULL;

	sprintf(envp[1], "NUMBER=%d", bat_changed);
	sprintf(envp[2], "STATE=%s", gpiod_get_value(priv->gpio_bat_det[bat_changed]) ?
			"CONNECTED" : "DISCONNECTED");
	kobject_uevent_env(&priv->pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);
	priv->current_bat_det_irq = 0;
}

/**********************************************************************
 *  Parse DT
 */

static int lm_pmu_dt(struct lbp_priv *priv)
{
	struct device *dev = &priv->pdev->dev;
	int n, num_gpio;

	num_gpio = gpiod_count(dev, "manikin-12v");
	if (num_gpio > 2) {
		dev_err(dev, "%s: More than 2 gpios for manikin-12v-gpio [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->gpio_12v_manikin[n] = devm_gpiod_get_index(dev, "manikin-12v", n, GPIOD_OUT_LOW);
		if (IS_ERR(priv->gpio_12v_manikin[n])) {
			dev_err(dev, "%s: GPIO manikin-12v-gpio [%d] invalid\n", __func__, n);
			return -EINVAL;
		}
	}

	priv->gpio_5v_manikin = devm_gpiod_get(dev, "manikin-5v", GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpio_5v_manikin)) {
		dev_err(dev, "%s: Invalid GPIO manikin-5v-gpio\n", __func__);
		return -EINVAL;
	}

	/* Battery detects */
	num_gpio = gpiod_count(dev, "bat-detect");
	if (num_gpio > 2) {
		dev_err(dev, "%s: More than gpios for bat-detect-gpios [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->gpio_bat_det[n] = devm_gpiod_get_index(dev, "bat-detect", n, GPIOD_IN);
		if (IS_ERR(priv->gpio_bat_det[n])) {
			dev_err(dev, "%s: Unable to request gpio_bat_det[n] pin %d\n", __func__, n);
			return -EINVAL;
		}
	}

	/* Bat Disables */
	num_gpio = gpiod_count(dev, "bat-disable");
	if (num_gpio > 2) {
		dev_err(dev, "%s: More than gpios for bat-disable-gpios [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->bat_disable[n] = false;
		priv->gpio_bat_disable[n] = devm_gpiod_get_index(dev, "bat-disable", n, GPIOD_OUT_LOW);
		if (IS_ERR(priv->gpio_bat_disable[n])) {
			dev_err(dev, "%s: Unable to get gpio_bat_disable[%d]\n", __func__, n);
			return -EINVAL;
		}
	}

	/* Charge enables */
	priv->bat_ce[0] = priv->bat_ce[1] = false;
	num_gpio = gpiod_count(dev, "bat-ce");
	if (num_gpio > 2) {
		dev_err(dev, "%s: More than gpios for bat-ce-gpios [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->gpio_bat_ce[n] = devm_gpiod_get_index(dev, "bat-ce", n, GPIOD_OUT_LOW);
		if (IS_ERR(priv->gpio_bat_ce[n])) {
			dev_err(dev, "%s: Unable to request gpio_bat_ce[%d]\n", __func__, n);
			return -EINVAL;
		}

	}

	/* Valids */
	num_gpio = gpiod_count(dev, "valid");
	if (num_gpio > 3) {
		dev_err(dev, "%s: More than gpios for valid-gpios [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->gpio_valid[n] = devm_gpiod_get_index(dev, "valid", n, GPIOD_IN);
		if(IS_ERR(priv->gpio_valid[n])) {
			dev_err(dev, "%s: Unable to request gpio_valid[%d]\n", __func__, n);
			return -EINVAL;
		}
	}


	priv->gpio_5w_sd = devm_gpiod_get(dev, "spksd", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpio_5w_sd)) {
		dev_err(dev, "Unable to request gpio_5w_sd\n");
		return -EINVAL;
	}

	priv->gpio_spkr_sd = devm_gpiod_get(dev, "amp-shutdown", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpio_spkr_sd)) {
		dev_err(dev, "Unable to request amp-shutdown-gpio, gpio_spkr_sd\n");
		return -EINVAL;
	}

	/* Startup codes */
	num_gpio = gpiod_count(dev, "startup");
	if (num_gpio > 2) {
		dev_err(dev, "%s: to many startup gpios [%d]\n", __func__, num_gpio);
		return -EINVAL;
	}
	for (n=0; n < num_gpio; n++) {
		priv->gpio_startup_code[n] = devm_gpiod_get_index(dev, "startup", n, GPIOD_IN);
		if (IS_ERR(priv->gpio_startup_code[n])) {
			dev_err(dev, "%s: Unable to request gpio_startup-gpios[%d]\n", __func__, n);
			return -EINVAL;
		}
	}

	/* CLR_STATUS */

	priv->gpio_clr_status = devm_gpiod_get_optional(dev, "clr-status", GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpio_clr_status)) {
			dev_err(dev, "%s: Unable to request gpio_clr_status pin\n", __func__);
			return -EINVAL;
	}

	return 0;
}

static int linkbox_plus_psy_probe(struct platform_device *pdev)
{
	struct lbp_priv *priv;
	int ret=0;
	struct power_supply_desc *psy_desc_dcin;
	struct power_supply_desc *psy_desc_manikin[2];
	struct power_supply_config psy_cfg = {};

	psy_desc_dcin = devm_kzalloc(&pdev->dev, sizeof(*psy_desc_dcin), GFP_KERNEL);
	if (!psy_desc_dcin)
		return -ENOMEM;

	psy_desc_manikin[0] = devm_kzalloc(&pdev->dev, sizeof(struct power_supply_desc), GFP_KERNEL);
	psy_desc_manikin[1] = devm_kzalloc(&pdev->dev, sizeof(struct power_supply_desc), GFP_KERNEL);
	if (!psy_desc_manikin[0] || !psy_desc_manikin[1])
		return -ENOMEM;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;


	psy_cfg.drv_data = priv;

	priv->pdev = pdev;
	platform_set_drvdata(pdev, priv);
	ret = lm_pmu_dt(priv);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: Failed to obtain platform data\n", __func__);
		goto cleanup;
	}

	/* Power supplies */
	priv->manikin_12v_power_on = false;
	priv->manikin_5v_power_on = false;
	priv->amp1w_state = AMP_AUTO;
	priv->amp5w_state = AMP_AUTO;

	psy_desc_dcin->name = "DCIN";
	psy_desc_dcin->type = POWER_SUPPLY_TYPE_MAINS;
	psy_desc_dcin->get_property = lm_pmu_mains_get_property;
	psy_desc_dcin->num_properties = ARRAY_SIZE(dcin_props_lb);
	psy_desc_dcin->properties = dcin_props_lb;

	platform_set_drvdata(pdev, priv);

	priv->ps_dcin = power_supply_register_no_ws(&pdev->dev, psy_desc_dcin, &psy_cfg);
	if (IS_ERR(priv->ps_dcin)) {
		dev_err(&pdev->dev, "Unable to register MAINS PS\n");
		ret = PTR_ERR(priv->ps_dcin);
		goto cleanup;
	}
	else {
		priv->ps_dcin_nb.notifier_call = lm_pmu_notifier_call;
		power_supply_reg_notifier(&priv->ps_dcin_nb);
		ret = sysfs_create_group(&priv->ps_dcin->dev.kobj, &bat_sysfs_attr_group);
	}

	psy_desc_manikin[0]->name = "MANIKIN_12V";
	psy_desc_manikin[0]->type = POWER_SUPPLY_TYPE_UNKNOWN;
	psy_desc_manikin[0]->get_property = lm_pmu_manikin_12v_get_property;
	psy_desc_manikin[0]->set_property = lm_pmu_manikin_12v_set_property;
	psy_desc_manikin[0]->num_properties = ARRAY_SIZE(manikin_12v_props);
	psy_desc_manikin[0]->properties = manikin_12v_props;
	psy_desc_manikin[0]->property_is_writeable = lm_pmu_manikin_prop_is_writable;

	priv->ps_manikin[0]= power_supply_register_no_ws(&pdev->dev, psy_desc_manikin[0], &psy_cfg);
	if (IS_ERR(priv->ps_manikin[0])) {
		dev_err(&pdev->dev, "Unable to register MANIKIN 12V PS\n");
		ret = PTR_ERR(priv->ps_manikin[0]);
		goto cleanup;
	}
	if (sysfs_create_file(&priv->ps_manikin[0]->dev.kobj, &dev_attr_amp5w_enable.attr) < 0)
		dev_err(&pdev->dev, "Unable to create attribute file for amp5w\n");
	psy_desc_manikin[1]->name = "MANIKIN_5V";
	psy_desc_manikin[1]->type = POWER_SUPPLY_TYPE_UNKNOWN;
	psy_desc_manikin[1]->get_property = lm_pmu_manikin_5v_get_property;
	psy_desc_manikin[1]->set_property = lm_pmu_manikin_5v_set_property;
	psy_desc_manikin[1]->num_properties = ARRAY_SIZE(manikin_5v_props);
	psy_desc_manikin[1]->properties = manikin_5v_props;
	if (!IS_ERR(priv->gpio_5v_manikin)) {
		psy_desc_manikin[1]->property_is_writeable = lm_pmu_manikin_prop_is_writable;
	}
	priv->ps_manikin[1]= power_supply_register_no_ws(&pdev->dev, psy_desc_manikin[1], &psy_cfg);
	if (IS_ERR(priv->ps_manikin[1])) {
		dev_err(&pdev->dev, "Unable to register MANIKIN 5V PS\n");
		ret = PTR_ERR(priv->ps_manikin[1]);
		goto cleanup;
	}
	if (sysfs_create_file(&priv->ps_manikin[1]->dev.kobj, &dev_attr_amp1w_enable.attr) < 0)
		dev_err(&pdev->dev, "Unable to create attribute file for amp1w\n");

	INIT_DELAYED_WORK(&priv->bat_work, bat_worker);
	return lm_pmu_setup_irq(priv);

cleanup:
	if (!IS_ERR_OR_NULL(priv->ps_dcin))
		power_supply_unregister(priv->ps_dcin);
	if (!IS_ERR_OR_NULL(priv->ps_manikin[0]))
		power_supply_unregister(priv->ps_manikin[0]);
	if (!IS_ERR_OR_NULL(priv->ps_manikin[1]))
		power_supply_unregister(priv->ps_manikin[1]);
	return ret;
}

static int lm_pmu_lb_remove(struct platform_device *pdev)
{
	struct lbp_priv *priv = platform_get_drvdata(pdev);
	sysfs_remove_group(&priv->ps_dcin->dev.kobj, &bat_sysfs_attr_group);
	sysfs_remove_file(&priv->ps_manikin[0]->dev.kobj, &dev_attr_amp5w_enable.attr);
	sysfs_remove_file(&priv->ps_manikin[1]->dev.kobj, &dev_attr_amp1w_enable.attr);
	put_device(&priv->i2c_adapter->dev);
	return 0;
}

static void lm_pmu_lb_shutdown(struct platform_device *pdev)
{
	struct lbp_priv *priv = platform_get_drvdata(pdev);
	gpiod_set_value(priv->gpio_bat_disable[0], 0);
	gpiod_set_value(priv->gpio_bat_disable[1], 0);

}

static struct platform_driver linkbox_plus_psy_driver = {
	.driver = {
		.name = "linkbox-plus-psy",
		.owner = THIS_MODULE,
		.of_match_table = linkbox_plus_psy_dt_ids,
	},
	.probe = linkbox_plus_psy_probe,
	.remove = lm_pmu_lb_remove,
	.shutdown = lm_pmu_lb_shutdown,
};

module_platform_driver(linkbox_plus_psy_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hans Christian Lonstad <hcl@datarespons.no>");
MODULE_DESCRIPTION("Laerdal Plus Linkbox Power Supply");
