/*
 * Copyright (C) 2023 DATA RESPONS AS
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
#define DEBUG
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
#include "../codecs/wm8962.h"
#include "fsl_sai.h"
#include "imx-audmux.h"

#define DAI_NAME_SIZE	32

struct board_variant {
	bool codec_master;
	const char *micbias_name;
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
	struct snd_soc_jack headphone_jack;
	struct snd_soc_jack mic_jack;
	bool mic_attr;
	bool hp_attr;
};

struct imx_wm8962_data {
	struct platform_device *pdev;
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	char codec_dai_name[DAI_NAME_SIZE];
	char platform_name[DAI_NAME_SIZE];
	unsigned int clk_frequency;
	bool stream_active[2];
	struct snd_soc_dai_link_component comp[3];
	struct snd_soc_component *codec_comp;
	struct card_options options;
	const struct board_variant *board_info;
	bool mic_connected;
};

const struct board_variant imx8_wm8962 = { .codec_master = false,
					   .micbias_name = "MICBIAS" };
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

static int imx_wm8962_jack_event_mic(struct notifier_block *nb,
				     unsigned long event, void *handle)
{
	struct snd_soc_jack *jack = (struct snd_soc_jack *)handle;
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(jack->card);
	char *envp[3], *buf;

	if (event == 0)
		data->mic_connected = false;
	else if (event & SND_JACK_MICROPHONE)
		data->mic_connected = true;
	else
		return 0;
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}
	if (data->mic_connected)
		strcpy(buf, "STATE=2");
	else
		strcpy(buf, "STATE=0");
	envp[0] = "NAME=microphone";
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	dev_dbg(&data->pdev->dev, "%s: Send event %s %s, status %d\n", __func__,
		envp[0], envp[1], (int)(event & SND_JACK_MICROPHONE));
	kfree(buf);
	return 0;
}

static struct notifier_block jack_nb_mic = {
	.notifier_call = imx_wm8962_jack_event_mic,
};

static ssize_t show_headphone(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(card);
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
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(card);

	if (data->mic_connected)
		strcpy(buf, "present\n");
	else
		strcpy(buf, "none\n");

	return strlen(buf);
}

static DEVICE_ATTR(microphone, S_IRUGO, show_mic, NULL);

static int hpjack_status_check(void *priv)
{
	struct imx_wm8962_data *data = priv;
	char *envp[3], *buf;
	int hp_status, ret;
	if (!gpio_is_valid(data->options.hp_gpio))
		return 0;
	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;

	dev_dbg(&data->pdev->dev, "%s: hpdet = %d (%d)\n", __func__, hp_status, data->options.hp_active_level);
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}

	if (hp_status == data->options.hp_active_level) {
		strcpy(buf, "STATE=2");
		if (data->options.auto_switch_speaker) {
			snd_soc_dapm_enable_pin(&data->card.dapm, "Headphone Jack");
			snd_soc_dapm_disable_pin(&data->card.dapm, "Ext Spk");
			snd_soc_dapm_sync(&data->card.dapm);
		}
		ret = SND_JACK_HEADPHONE;
		if (data->options.iphone_jack && data->codec_comp) {
			wm8962_mic_detect(data->codec_comp, &data->options.headphone_jack);
		}

	} else {
		strcpy(buf, "STATE=0");
		if (data->options.auto_switch_speaker) {
			snd_soc_dapm_enable_pin(&data->card.dapm, "Ext Spk");
			snd_soc_dapm_disable_pin(&data->card.dapm, "Headphone Jack");
			snd_soc_dapm_sync(&data->card.dapm);
		}
		ret = 0;
		if (data->options.iphone_jack && data->codec_comp) {
			wm8962_mic_detect(data->codec_comp, 0);
			data->mic_connected = false;
		}
	}

	envp[0] = "NAME=headphone";
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	dev_dbg(&data->pdev->dev, "%s: Send event %s %s, status %d\n", __func__, envp[0], envp[1], ret);
	kfree(buf);

	return ret;
}

static int micjack_status_check(void *priv)
{
	struct imx_wm8962_data *data = priv;
	int mic_status, ret=0;
	if (!gpio_is_valid(data->options.mic_gpio))
		return 0;

	mic_status = gpio_get_value(data->options.mic_gpio) ? 1 : 0;
	if (mic_status == data->options.mic_active_level) {
		ret = SND_JACK_MICROPHONE;
		wm8962_mic_detect(data->codec_comp, &data->options.mic_jack);
	} else {
		wm8962_mic_detect(data->codec_comp, 0);
		data->mic_connected = false;
	}
	return ret;
}


static int imx_wm8962_spk_amp_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *k, int event)
{
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(w->dapm->card);
	if (gpio_is_valid(data->options.spk_amp_gpio)) {
		dev_dbg(&data->pdev->dev, "%s: Spk Amp %d\n", __func__, event);
		if (SND_SOC_DAPM_EVENT_ON(event))
			gpio_set_value(data->options.spk_amp_gpio, data->options.spk_amp_active_level);
		else
			gpio_set_value(data->options.spk_amp_gpio, data->options.spk_amp_active_level ? 0 : 1);
	}
	return 0;
}

static const struct snd_soc_dapm_widget imx_wm8962_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Headphone Jack"),
	SND_SOC_DAPM_SPK("Ext Spk", imx_wm8962_spk_amp_event),
	SND_SOC_DAPM_MIC("AMIC", NULL),
};

static int imx_wm8962_jack_init(struct imx_wm8962_data *data)
{
	int ret;
	if (gpio_is_valid(data->options.hp_gpio)) {
		imx_hp_jack_gpio.gpio = data->options.hp_gpio;
		imx_hp_jack_gpio.data = data;
		imx_hp_jack_gpio.jack_status_check = hpjack_status_check;
		imx_hp_jack_gpio.invert = data->options.hp_active_level ? 0 : 1;
		ret = snd_soc_card_jack_new(&data->card, "Headphone Jack",
					    SND_JACK_HEADPHONE, &data->options.headphone_jack,
					    imx_hp_jack_pins,
					    ARRAY_SIZE(imx_hp_jack_pins));
		snd_soc_jack_add_gpios(&data->options.headphone_jack, 1, &imx_hp_jack_gpio);
		if (data->options.iphone_jack)
			snd_soc_jack_notifier_register(&data->options.headphone_jack, &jack_nb_mic);
	}
	if (gpio_is_valid(data->options.mic_gpio)) {
		imx_mic_jack_gpio.gpio = data->options.mic_gpio;
		imx_mic_jack_gpio.data = data;
		imx_mic_jack_gpio.jack_status_check = micjack_status_check;
		imx_mic_jack_gpio.invert = data->options.mic_active_level ? 0 : 1;

		snd_soc_card_jack_new(&data->card, "AMIC", SND_JACK_MICROPHONE, &data->options.mic_jack,
				imx_mic_jack_pins, ARRAY_SIZE(imx_mic_jack_pins));
		snd_soc_jack_add_gpios(&data->options.mic_jack, 1, &imx_mic_jack_gpio);
		snd_soc_jack_notifier_register(&data->options.mic_jack, &jack_nb_mic);
	}
	if (gpio_is_valid(data->options.hp_gpio)) {

		ret = device_create_file(&data->pdev->dev, &dev_attr_headphone);
		if (ret) {
			dev_err(&data->pdev->dev, "create hp attr failed (%d)\n", ret);
		}
		data->options.hp_attr = true;
	}

	if (gpio_is_valid(data->options.mic_gpio) || (data->options.iphone_jack && gpio_is_valid(data->options.hp_gpio))) {
		ret = device_create_file(&data->pdev->dev, &dev_attr_microphone);
		if (ret) {
			dev_err(&data->pdev->dev, "create mic attr failed (%d)\n", ret);
		}
		data->options.mic_attr = true;
	}
	return 0;
}

static int imx_wm896x_late_probe(struct snd_soc_card *card)
{
	int ret;
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = list_first_entry(
		&card->rtd_list, struct snd_soc_pcm_runtime, list);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	data->codec_comp = codec_dai->component;
	if (!data->codec_comp)
		dev_warn(card->dev, "Failed to get codec component");

	ret = snd_soc_dai_set_sysclk(cpu_dai, FSL_SAI_CLK_MAST1,
						data->clk_frequency,
						SND_SOC_CLOCK_OUT);
	if (ret) {
		dev_err(&data->pdev->dev,
			"failed to set codec sysclk: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx_wm896x_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	/* struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream); */
	return 0;
}


static const struct snd_soc_ops imx_wm896x_card_ops = {
	.hw_params = imx_wm896x_hw_params,
};

static const struct of_device_id imx_wm8962_dt_ids[] = {
	{ .compatible = "fsl,lm-imx-audio-wm8962", .data = &imx8_wm8962},
	{ /* sentinel */ }
};

static int imx_wm8962_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cpu_np=0, *codec_np=0;
	struct platform_device *cpu_pdev;
	struct i2c_client *codec_i2c;
	struct imx_wm8962_data *data;
	struct clk *codec_clk=0;
	int ret;
	enum of_gpio_flags gpio_flags;
	const struct of_device_id *of_id = of_match_device(imx_wm8962_dt_ids, &pdev->dev);
	const struct board_variant *board_info = of_id ? (struct board_variant*)of_id->data : NULL;
	if (!board_info) {
		return -ENODATA;
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
	if (!codec_i2c || !&codec_i2c->dev) {
		dev_err(&pdev->dev, "failed to find codec platform device\n");
		ret = -ENODEV;
		goto cleanup;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto cleanup;
	}
	data->pdev = pdev;
	data->board_info = board_info;

	codec_clk = clk_get(&codec_i2c->dev, NULL);
	if (IS_ERR(codec_clk)) {
		ret = PTR_ERR(codec_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(&codec_i2c->dev, "failed to get codec clk: %d\n", ret);
		goto cleanup;
	}

	data->clk_frequency = clk_get_rate(codec_clk);
	clk_put(codec_clk);
	dev_info(&pdev->dev, "%s: codec clock is %d\n", __func__, data->clk_frequency);

	data->options.hp_gpio = of_get_named_gpio_flags(np, "hp-det-gpios", 0, &gpio_flags);
	data->options.hp_active_level = (gpio_flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;

	data->options.iphone_jack = of_property_read_bool(np, "iphone-jack");

	data->options.mic_gpio = of_get_named_gpio_flags(np, "mic-det-gpios", 0, &gpio_flags);
	data->options.mic_active_level = gpio_flags & OF_GPIO_ACTIVE_LOW ? 0 : 1;

	data->options.spk_amp_gpio = of_get_named_gpio_flags(np, "speaker-amp-gpios", 0, &gpio_flags);
	if (gpio_is_valid(data->options.spk_amp_gpio)) {
		data->options.spk_amp_active_level = gpio_flags & OF_GPIO_ACTIVE_LOW ? 0 : 1;
		ret = devm_gpio_request_one(&pdev->dev, data->options.spk_amp_gpio, 0, "wm8962-speaker-amp");
		if (ret < 0) {
			dev_err(&pdev->dev, "%s: Unable to request gpio %d for amp\n", __func__, data->options.spk_amp_gpio);
			data->options.spk_amp_gpio = -1;
			goto cleanup;
		}

	}

	data->options.auto_switch_speaker = of_property_read_bool(np, "speaker-auto-switch");
	data->dai.cpus = &data->comp[0];
	data->dai.codecs = &data->comp[1];
	data->dai.platforms = &data->comp[2];
	data->dai.num_cpus = 1;
	data->dai.num_codecs = 1;
	data->dai.num_platforms = 1;
	data->dai.name = "HiFi";
	data->dai.stream_name = "HiFi";
	data->dai.codecs->of_node = codec_np;
	data->dai.cpus->of_node = cpu_np;
	data->dai.platforms->of_node = cpu_np;
	data->dai.ops = &imx_wm896x_card_ops;
	data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	data->dai.codecs->dai_name = "wm8962";
	data->dai.symmetric_channels = 1;

	data->card.dev = &pdev->dev;
	data->card.late_probe = imx_wm896x_late_probe;
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
	data->card.dapm_widgets = imx_wm8962_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_wm8962_dapm_widgets);
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
	ret = imx_wm8962_jack_init(data);
cleanup:
	if (cpu_pdev)
		put_device(&cpu_pdev->dev);
	if (codec_i2c)
		put_device(&codec_i2c->dev);
	if (cpu_np)
		of_node_put(cpu_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static int imx_wm8962_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_wm8962_data *data = snd_soc_card_get_drvdata(card);
	if (data->options.mic_attr)
		device_remove_file(&pdev->dev, &dev_attr_microphone);
	if (data->options.hp_attr)
		device_remove_file(&pdev->dev, &dev_attr_headphone);
	return 0;
}


MODULE_DEVICE_TABLE(of, imx_wm8962_dt_ids);

static struct platform_driver lm_imx_wm8962_driver = {
	.driver = {
		.name = "lm-imx-wm8962",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_wm8962_dt_ids,
	},
	.probe = imx_wm8962_probe,
	.remove = imx_wm8962_remove,
};
module_platform_driver(lm_imx_wm8962_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_AUTHOR("DATA RESPONS AS");
MODULE_DESCRIPTION("Laerdal i.MX WM8962 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm-imx-wm8962");
