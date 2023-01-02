/*
 * Simple PWM buzzer
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/pinctrl/pinctrl.h>


#define MAX_HANDLES 2
struct fmux_data {
	struct device *dev;
	const char* names[MAX_HANDLES];
	int num_handles;
	struct pinctrl *pinc;
	struct pinctrl_state *pstate[MAX_HANDLES];
	int active;
};

static DEFINE_MUTEX(sysfs_lock);


static int fmux_reparse(struct fmux_data *priv)
{
	int ret;
	int n;
	ret = of_property_count_strings(priv->dev->of_node, "pinctrl-names");
	if ( ret < 0 || ret > MAX_HANDLES) {
		dev_err(priv->dev, "Wrong number of mux options or bad name string [%d]\n", ret);
		return -EINVAL;
	}

	priv->num_handles = ret;
	for (n=0; n < priv->num_handles; n++) {
		ret = of_property_read_string_index(priv->dev->of_node, "pinctrl-names", n, &priv->names[n]);
	}
	return ret;
}

static int fmux_match(struct fmux_data *priv, const char* s)
{
	int n;
	for (n=0; n < priv->num_handles; n++) {
		if (strcmp(priv->names[n], s) == 0)
			return n;
	}
	return -EINVAL;
}

static ssize_t fmux_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmux_data *priv = dev_get_drvdata(dev);
	ssize_t status=0;
	mutex_lock(&sysfs_lock);
	status = sprintf(buf, priv->names[priv->active]);
	mutex_unlock(&sysfs_lock);
	return status;
}

static ssize_t fmux_opt_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmux_data *priv = dev_get_drvdata(dev);
	ssize_t status=0;
	int n;
	mutex_lock(&sysfs_lock);
	for (n=0; n < priv->num_handles; n++)
		status += sprintf(buf+status, "%s ", priv->names[n]);
	status += sprintf(buf+status, "\n");
	mutex_unlock(&sysfs_lock);
	return status;
}

static ssize_t fmux_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t sz)
{
	struct fmux_data *priv = dev_get_drvdata(dev);
	size_t status = -EINVAL;
	char sbuf[40];
	int index;
	mutex_lock(&sysfs_lock);
	strncpy(sbuf, buf, min((int)sz, 39));
	sbuf[sz] = '\0';

	if ((index = fmux_match(priv, sbuf)) >= 0) {
		if (index == priv->active)
			status = sz;
		else {
			status = pinctrl_select_state(priv->pinc, priv->pstate[index]);
			if (status < 0) {
				dev_err(priv->dev, "Unable to select state %s\n", priv->names[index]);
			}
			else {
				priv->active = index;
				status = sz;
			}
		}
	}
	else {
		dev_err(priv->dev, "Invalid selection %s\n", sbuf);
	}
	mutex_unlock(&sysfs_lock);
	return status;
}

static DEVICE_ATTR(select, 0644, fmux_show, fmux_store);
static DEVICE_ATTR(possible, 0444, fmux_opt_show, NULL);


static int fmux_probe(struct platform_device *pdev)
{
	struct fmux_data *priv;
	int ret;
	struct device_node *np = pdev->dev.of_node;

	int status, n;

	if (!np) {
		dev_err(&pdev->dev, "No DT node found\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "no memory for state\n");
		return -ENOMEM;
	}

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	fmux_reparse(priv);
	if (priv->num_handles < 2) {
		dev_err(priv->dev, "Too few [%d] handles\n", priv->num_handles);
		status = -EINVAL;
		goto deref;
	}
	priv->pinc = devm_pinctrl_get(priv->dev);
	if ( IS_ERR(priv->pinc))
	{
		status = -ENODEV;
		goto deref;
	}
	for (n=0; n < priv->num_handles; n++) {
		priv->pstate[n] = pinctrl_lookup_state(priv->pinc, priv->names[n]);
	}
	mutex_lock(&sysfs_lock);
	ret = pinctrl_select_state(priv->pinc, priv->pstate[0]);
	if (ret < 0) {
		dev_err(priv->dev, "Unable to select state %s\n", priv->names[0]);
		status = ret;
		mutex_unlock(&sysfs_lock);
		goto deref;
	}
	priv->active = 0;
	mutex_unlock(&sysfs_lock);

	ret = sysfs_create_file(&pdev->dev.kobj, &dev_attr_select.attr);
	ret = sysfs_create_file(&pdev->dev.kobj, &dev_attr_possible.attr);

	return 0;
deref:
	return status;
}

static int fmux_remove(struct platform_device *pdev)
{
	sysfs_remove_file(&pdev->dev.kobj, &dev_attr_select.attr);
	sysfs_remove_file(&pdev->dev.kobj, &dev_attr_possible.attr);
	return 0;
}

static struct of_device_id fmux_of_match[] = {
	{
		.compatible = "datarespons,function-mux",
	},
	{}
};

MODULE_DEVICE_TABLE(of, fmux_of_match);

static struct platform_driver fmux_driver = {
	.driver =
	{
		.name = "function-mux",
		.owner = THIS_MODULE,
		.of_match_table = fmux_of_match,
	},
	.probe = fmux_probe,
	.remove = fmux_remove,
};

module_platform_driver(fmux_driver);

MODULE_DESCRIPTION("DT based function pinmux");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:function-mux");
MODULE_AUTHOR("Hans Christian Lonstad <hcl@datarespons.no>");
