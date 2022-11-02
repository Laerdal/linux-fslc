/*
 * lm-imx-wm8960.c
 *
 *  Created on: Sep 1, 2015
 *      Author: hcl
 */


/*
 * Copyright (C) 2015 DATA RESPONS AS
 *
 * Based on imx-wm8962.c
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 *
 * Based on imx-sgtl5000.c
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 * Copyright (C) 2012 Linaro Ltd.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <sound/soc-component.h>
#include <linux/pinctrl/consumer.h>

#include "../codecs/wm8960.h"
#include "imx-audmux.h"

#define DAI_NAME_SIZE	32

enum cpu_type_id {CPUTYPE_IMX8, CPUTYPE_IMX6};

struct board_variant {
	bool codec_master;
	enum cpu_type_id cpu_type;
};

struct card_options {
	int hp_gpio;
	int hp_active_level;
	int mic_gpio;
	int mic_active_level;
	int iphone_jack;
	int spk_amp_gpio;
	int spk_amp_active_level;
	bool auto_switch_speaker;
	bool broken_mic_detect;
	int hsjack_last_status;
	struct snd_soc_jack headphone_jack;
	struct snd_soc_jack mic_jack;
	bool mic_attr;
	bool hp_attr;
};

struct imx_wm8960_data {
	struct platform_device *pdev;
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	char codec_dai_name[DAI_NAME_SIZE];
	char platform_name[DAI_NAME_SIZE];
	unsigned int clk_frequency;
	struct clk *codec_clk;
	bool stream_active[2];
	struct snd_soc_dai_link_component comp[3];
	struct card_options options;
	const struct board_variant *board_info;
};

const struct board_variant imx6_wm8960 = { .codec_master = true,
					    .cpu_type = CPUTYPE_IMX6 };
const struct board_variant imx8_wm8962 = { .codec_master = false,
					    .cpu_type = CPUTYPE_IMX8 };
static struct snd_soc_jack_pin imx_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};


static struct snd_soc_jack_gpio imx_hp_jack_gpio= {
		.name = "headphone detect",
		.report = SND_JACK_HEADPHONE,
		.debounce_time = 200,
};

static struct snd_soc_jack_pin imx_mic_jack_pins[] = {
	{
		.pin = "AMIC",
		.mask = SND_JACK_MICROPHONE,
	},
};
static struct snd_soc_jack_gpio imx_mic_jack_gpio = {
	.name = "microphone detect",
	.report = SND_JACK_MICROPHONE,
	.debounce_time = 200,
};

static ssize_t show_headphone(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);
	int hp_status;

	if (!gpio_is_valid(data->options.hp_gpio)) {
		strcpy(buf, "no detect gpio connected\n");
		return strlen(buf);
	}

	/* Check if headphone is plugged in */
	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;

	if (hp_status == data->options.hp_active_level)
		strcpy(buf, "present\n");
	else
		strcpy(buf, "none\n");

	return strlen(buf);
}

static DEVICE_ATTR(headphone, S_IRUGO, show_headphone, NULL);

static ssize_t show_mic(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);
	int mic_status, hp_status;

	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;
	if (!gpio_is_valid(data->options.mic_gpio)) {
		if (hp_status == data->options.hp_active_level)
			strcpy(buf, "present\n");
		else
			strcpy(buf, "none\n");
		return strlen(buf);
	}

	/* Check if analog microphone is plugged in */
	mic_status = gpio_get_value(data->options.mic_gpio) ? 1 : 0;
	strcpy(buf, "none\n");
	if (data->options.iphone_jack) {
		if (mic_status == data->options.mic_active_level && hp_status == data->options.hp_active_level)
			strcpy(buf, "present\n");
	}
	else {
		if (mic_status == data->options.mic_active_level)
			strcpy(buf, "present\n");
	}
	return strlen(buf);
}

static DEVICE_ATTR(microphone, S_IRUGO, show_mic, NULL);
static int hpjack_status_check(void *priv)
{
	struct imx_wm8960_data *data = priv;
	char *envp[3], *buf;
	int hp_status, ret;
	if (!gpio_is_valid(data->options.hp_gpio))
		return 0;
	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;

	dev_info(&data->pdev->dev, "%s: hpdet = %d (%d)\n", __func__, hp_status, data->options.hp_active_level);
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}

	envp[0] = "NAME=headphone";
	ret = SND_JACK_HEADPHONE;

	if (hp_status == data->options.hp_active_level) {
		snprintf(buf, 32, "STATE=%d", 2);
		if (data->options.auto_switch_speaker) {
			snd_soc_dapm_enable_pin(&data->card.dapm, "Headphone Jack");
			snd_soc_dapm_disable_pin(&data->card.dapm, "Ext Spk");
		}

		if (!gpio_is_valid(data->options.mic_gpio)) {
			ret = SND_JACK_HEADSET;
			envp[0] = "NAME=headset";
		}

		if (data->options.iphone_jack || !gpio_is_valid(data->options.mic_gpio)) {
			snd_soc_dapm_force_enable_pin(&data->card.dapm, "MICB");
		}
		BUG_ON(data->card.snd_card == NULL);
		snd_jack_report(data->options.headphone_jack.jack, 1);
	} else {
		snprintf(buf, 32, "STATE=%d", 0);
		if (data->options.auto_switch_speaker) {
			snd_soc_dapm_enable_pin(&data->card.dapm, "Ext Spk");
			snd_soc_dapm_disable_pin(&data->card.dapm, "Headphone Jack");
		}
		if (data->options.iphone_jack || !gpio_is_valid(data->options.mic_gpio))
			snd_soc_dapm_disable_pin(&data->card.dapm, "MICB");

		ret = 0;
		snd_jack_report(data->options.headphone_jack.jack, 0);
	}

	snd_soc_dapm_sync(&data->card.dapm);
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	dev_info(&data->pdev->dev, "%s: Send event %s %s, status %d\n", __func__, envp[0], envp[1], ret);
	kfree(buf);

	return ret;
}

static int micjack_status_check(void *priv)
{
	struct imx_wm8960_data *data = priv;
	char *envp[3], *buf;
	int mic_status, ret=0, hp_status;
	if (!gpio_is_valid(data->options.mic_gpio))
		return 0;

	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;
	mic_status = gpio_get_value(data->options.mic_gpio) ? 1 : 0;

	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}

	if (data->options.iphone_jack) {
		if (hp_status != data->options.hp_active_level) {
			dev_dbg(&data->pdev->dev, "%s: no iphone jack present\n", __func__);
			snprintf(buf, 32, "STATE=%d", 0);
			ret = 0;
		}
		else {
			if (mic_status == data->options.mic_active_level) {
				snprintf(buf, 32, "STATE=%d", 2);
				ret = imx_mic_jack_gpio.report;
				snd_soc_dapm_force_enable_pin(&data->card.dapm, "AMIC");
			} else {
				snprintf(buf, 32, "STATE=%d", 0);
				ret = 0;
				snd_soc_dapm_disable_pin(&data->card.dapm, "AMIC");
			}
		}
	}
	else {
		if (mic_status == data->options.mic_active_level) {
			snprintf(buf, 32, "STATE=%d", 2);
			dev_dbg(&data->pdev->dev, "%s: Enable AMIC\n", __func__);
			snd_soc_dapm_force_enable_pin(&data->card.dapm, "MICB");
			snd_soc_dapm_force_enable_pin(&data->card.dapm, "AMIC");
			ret = imx_mic_jack_gpio.report;
		} else {
			snprintf(buf, 32, "STATE=%d", 0);
			dev_dbg(&data->pdev->dev, "%s: Disable AMIC\n", __func__);
			snd_soc_dapm_disable_pin(&data->card.dapm, "AMIC");
			snd_soc_dapm_disable_pin(&data->card.dapm, "MICB");
			ret = 0;
		}
	}
	snd_soc_dapm_sync(&data->card.dapm);

	envp[0] = "NAME=microphone";
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	dev_info(&data->pdev->dev, "%s: Send event %s %s, status %d\n", __func__, envp[0], envp[1], ret);
	kfree(buf);

	return ret;
}

static int imx_wm8960_spk_amp_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *k, int event)
{
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(w->dapm->card);
	if (gpio_is_valid(data->options.spk_amp_gpio)) {
		dev_dbg(&data->pdev->dev, "%s: Spk Amp %d\n", __func__, event);
		if (SND_SOC_DAPM_EVENT_ON(event))
			gpio_set_value(data->options.spk_amp_gpio, data->options.spk_amp_active_level);
		else
			gpio_set_value(data->options.spk_amp_gpio, data->options.spk_amp_active_level ? 0 : 1);
	}
	return 0;
}

static const struct snd_soc_dapm_widget imx_wm8960_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Headphone Jack"),
	SND_SOC_DAPM_SPK("Ext Spk", imx_wm8960_spk_amp_event),
	SND_SOC_DAPM_MIC("AMIC", NULL),
};

static const struct snd_soc_dapm_widget cpum_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Headphone"),
	SND_SOC_DAPM_INPUT("Right Input"),
	SND_SOC_DAPM_INPUT("Left Input"),
};

static int imx_wm8960_jack_init(struct imx_wm8960_data *data)
{
	int ret;
	if (gpio_is_valid(data->options.hp_gpio)) {
		imx_hp_jack_gpio.gpio = data->options.hp_gpio;
		imx_hp_jack_gpio.data = data;
		imx_hp_jack_gpio.jack_status_check = hpjack_status_check;
		imx_hp_jack_gpio.invert = data->options.hp_active_level ? 0 : 1;
		ret = snd_soc_card_jack_new(&data->card, "Headphone Jack", SND_JACK_HEADPHONE,
						 &data->options.headphone_jack, imx_hp_jack_pins,
						 ARRAY_SIZE(imx_hp_jack_pins));
		snd_soc_jack_add_gpios(&data->options.headphone_jack, 1, &imx_hp_jack_gpio);
	}
	if (gpio_is_valid(data->options.mic_gpio)) {
		imx_mic_jack_gpio.gpio = data->options.mic_gpio;
		imx_mic_jack_gpio.data = data;
		imx_mic_jack_gpio.jack_status_check = micjack_status_check;
		imx_mic_jack_gpio.invert = data->options.mic_active_level ? 0 : 1;

		snd_soc_card_jack_new(&data->card, "AMIC", SND_JACK_MICROPHONE, &data->options.mic_jack,
					   imx_mic_jack_pins, ARRAY_SIZE(imx_mic_jack_pins));
		snd_soc_jack_add_gpios(&data->options.mic_jack, 1, &imx_mic_jack_gpio);
	}
	if (gpio_is_valid(data->options.hp_gpio)) {
		ret = device_create_file(&data->pdev->dev, &dev_attr_headphone);
		if (ret) {
			dev_err(&data->pdev->dev, "create hp attr failed (%d)\n", ret);
		}
		data->options.hp_attr = true;
	}

	if (gpio_is_valid(data->options.mic_gpio) ||
	    (data->options.iphone_jack && gpio_is_valid(data->options.hp_gpio))) {
		ret = device_create_file(&data->pdev->dev, &dev_attr_microphone);
		if (ret) {
			dev_err(&data->pdev->dev, "create mic attr failed (%d)\n", ret);
		}
		data->options.mic_attr = true;
	}
	return 0;
}

static int imx_wm8960_late_probe(struct snd_soc_card *card)
{
	int ret;
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = list_first_entry(
		&card->rtd_list, struct snd_soc_pcm_runtime, list);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);

	ret = clk_prepare_enable(data->codec_clk);
	if (ret) {
		dev_err(data->card.dev, "Failed to enable MCLK: %d\n", ret);
		return ret;
	}

	if (data->board_info->cpu_type == CPUTYPE_IMX6) {
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8960_SYSCLK_PLL,
					     data->clk_frequency,
					     SND_SOC_CLOCK_IN);
		if (ret) {
			dev_err(&data->pdev->dev,
				"failed to set codec sysclk: %d\n", ret);
			return ret;
		}
	}
	ret = imx_wm8960_jack_init(data);
	if (ret < 0)
		return ret;
	/* Use 0.9 factor on MIC BIAS voltage */
	snd_soc_component_update_bits(codec_dai->component, WM8960_ADDCTL4, 0x1,
				      0x0);
	/* Use MONO mixer */
	snd_soc_component_update_bits(codec_dai->component, WM8960_ADDCTL1,
				      0x10, 0x10);
	clk_disable_unprepare(data->codec_clk);
	return 0;
}

static const struct of_device_id imx_wm8960_dt_ids[] = {
	{ .compatible = "fsl,lm-imx-audio-wm8960", .data = &imx6_wm8960},
	{ .compatible = "fsl,lm-imx-audio-wm8962", .data = &imx8_wm8962},
	{ /* sentinel */ }
};

static int imx_wm8960_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cpu_np=0, *codec_np=0;
	struct platform_device *cpu_pdev;
	struct i2c_client *codec_i2c;
	struct imx_wm8960_data *data;
	int ret;
	enum of_gpio_flags gpio_flags;
	int int_port, ext_port;
	const struct of_device_id *of_id = of_match_device(imx_wm8960_dt_ids, &pdev->dev);
	const struct board_variant *board_info = of_id ? (struct board_variant*)of_id->data : NULL;
	if (!board_info) {
		return -ENODATA;
	}
	if (board_info->cpu_type == CPUTYPE_IMX6) {
		ret = of_property_read_u32(np, "mux-int-port", &int_port);
		if (ret) {
			dev_err(&pdev->dev,
				"mux-int-port missing or invalid\n");
			return ret;
		}
		ret = of_property_read_u32(np, "mux-ext-port", &ext_port);
		if (ret) {
			dev_err(&pdev->dev,
				"mux-ext-port missing or invalid\n");
			return ret;
		}

		/*
	 * The port numbering in the hardware manual starts at 1, while
	 * the audmux API expects it starts at 0.
	 */
		int_port--;
		ext_port--;
		ret = imx_audmux_v2_configure_port(
			int_port,
			IMX_AUDMUX_V2_PTCR_SYN |
				IMX_AUDMUX_V2_PTCR_TFSEL(ext_port) |
				IMX_AUDMUX_V2_PTCR_TCSEL(ext_port) |
				IMX_AUDMUX_V2_PTCR_TFSDIR |
				IMX_AUDMUX_V2_PTCR_TCLKDIR,
			IMX_AUDMUX_V2_PDCR_RXDSEL(ext_port));
		if (ret) {
			dev_err(&pdev->dev,
				"audmux internal port setup failed\n");
			return ret;
		}
		ret = imx_audmux_v2_configure_port(
			ext_port, IMX_AUDMUX_V2_PTCR_SYN,
			IMX_AUDMUX_V2_PDCR_RXDSEL(int_port));
		if (ret) {
			dev_err(&pdev->dev,
				"audmux external port setup failed\n");
			return ret;
		}
	}
	cpu_np = of_parse_phandle(pdev->dev.of_node, "audio-cpu", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		ret = -EINVAL;
		goto cleanup;
	}

	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -EINVAL;
		goto cleanup;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_err(&pdev->dev, "failed to find SSI platform device\n");
		ret = -EINVAL;
		goto cleanup;
	}
	codec_i2c = of_find_i2c_device_by_node(codec_np);
	if (!codec_i2c) {
		dev_dbg(&pdev->dev, "failed (%ld) to find codec platform device\n", PTR_ERR(codec_i2c));
		ret = -EPROBE_DEFER;
		goto cleanup;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto cleanup;
	}
	data->pdev = pdev;
	data->board_info = board_info;

	data->codec_clk = devm_clk_get(&codec_i2c->dev, NULL);
	if (IS_ERR(data->codec_clk)) {
		ret = PTR_ERR(data->codec_clk);
		dev_err(&codec_i2c->dev, "failed to get codec clk: %d\n", ret);
		goto cleanup;
	}

	data->clk_frequency = clk_get_rate(data->codec_clk);
	dev_info(&pdev->dev, "%s: codec clock is %d\n", __func__, data->clk_frequency);

	data->options.hp_gpio = of_get_named_gpio_flags(np, "hp-det-gpios", 0, &gpio_flags);
	data->options.hp_active_level = (gpio_flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;

	data->options.iphone_jack = of_property_read_bool(np, "iphone-jack");

	data->options.mic_gpio = of_get_named_gpio_flags(np, "mic-det-gpios", 0, &gpio_flags);
	data->options.mic_active_level = gpio_flags & OF_GPIO_ACTIVE_LOW ? 0 : 1;

	data->options.spk_amp_gpio = of_get_named_gpio_flags(np, "speaker-amp-gpios", 0, &gpio_flags);
	if (gpio_is_valid(data->options.spk_amp_gpio)) {
		data->options.spk_amp_active_level = gpio_flags & OF_GPIO_ACTIVE_LOW ? 0 : 1;
		if (gpio_request_one(data->options.spk_amp_gpio, 0, "wm8960-speaker-amp")) {
			dev_err(&pdev->dev, "%s: Unable to request gpio %d for amp\n", __func__, data->options.spk_amp_gpio);
			data->options.spk_amp_gpio = -1;
		}

	}

	data->options.auto_switch_speaker = of_property_read_bool(np, "speaker-auto-switch");
	data->dai.cpus = &data->comp[0];
	data->dai.codecs = &data->comp[1];
	data->dai.platforms = &data->comp[2];
	data->dai.num_cpus	= 1;
	data->dai.num_codecs	= 1;
	data->dai.num_platforms	= 1;
	data->dai.name = "HiFi";
	data->dai.stream_name = "HiFi";
	data->dai.codecs->dai_name = "wm8960-hifi";
	data->dai.codecs->of_node = codec_np;
	data->dai.cpus->of_node = cpu_np;
	data->dai.platforms->of_node = cpu_np;
	if (board_info->cpu_type == CPUTYPE_IMX6)	/* 8960 is master*/
		data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBP_CFP;
	else
		data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	data->dai.symmetric_channels = 1;

	data->card.dev = &pdev->dev;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse card name\n");
		goto cleanup;
	}
	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse audio routing\n");
		goto cleanup;
	}
	data->card.num_links = 1;
	data->card.dai_link = &data->dai;
	data->card.late_probe = imx_wm8960_late_probe;
	if (strncmp(data->card.name, "cpu-module", 10) == 0) {
		data->card.dapm_widgets = cpum_dapm_widgets;
		data->card.num_dapm_widgets = ARRAY_SIZE(cpum_dapm_widgets);
	} else {
		data->card.dapm_widgets = imx_wm8960_dapm_widgets;
		data->card.num_dapm_widgets = ARRAY_SIZE(imx_wm8960_dapm_widgets);
	}
	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);
	if (ret < 0)
		goto cleanup;

	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto cleanup;
	}

cleanup:
	if (cpu_np)
		of_node_put(cpu_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static int imx_wm8960_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);
	if (data->options.mic_attr)
		device_remove_file(&pdev->dev, &dev_attr_microphone);
	if (data->options.hp_attr)
		device_remove_file(&pdev->dev, &dev_attr_headphone);
	clk_put(data->codec_clk);
	return 0;
}


MODULE_DEVICE_TABLE(of, imx_wm8960_dt_ids);

static struct platform_driver lm_imx_wm8960_driver = {
	.driver = {
		.name = "lm-imx-wm8960",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_wm8960_dt_ids,
	},
	.probe = imx_wm8960_probe,
	.remove = imx_wm8960_remove,
};
module_platform_driver(lm_imx_wm8960_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_AUTHOR("DATA RESPONS AS");
MODULE_DESCRIPTION("Laerdal i.MX WM8960 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm-imx-wm8960");
