/*
 * Copyright (C) 2023 DATA RESPONS AS
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
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>

#include "../codecs/sgtl5000.h"
#include "fsl_sai.h"
#include "imx-audmux.h"

#define DAI_NAME_SIZE	32

struct board_variant {
	bool codec_master;
	bool iphone_jack;
	bool has_speaker;
};

struct card_options {
	int hp_gpio;
	int hp_active_level;
	int mic_gpio;
	int mic_active_level;
	struct snd_soc_jack headphone_jack;
	struct snd_soc_jack mic_jack;
	bool mic_attr;
	bool hp_attr;
};

struct imx_sp2_audio_data {
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
	bool hp_connected;
	bool jacks_initiated;
	struct delayed_work mic_work;
	int current_jack_report;
	struct iio_channel *iioc;
};

const struct board_variant imx8_sp2_audio = {
	.codec_master = true,
	.iphone_jack = true,
	.has_speaker = true,
};

const struct board_variant imx8_lb2_audio = {
	.codec_master = false,
	.iphone_jack = false,
	.has_speaker = false,
};
static struct snd_soc_jack_pin imx_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static struct snd_soc_jack_pin imx_hs_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADSET,
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

static int imx_sp2_audio_jack_event(struct notifier_block *nb,
				     unsigned long event, void *handle)
{
	struct snd_soc_jack *jack = (struct snd_soc_jack *)handle;
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(jack->card);
	char *envp[3], *buf;
	int state=0;
	envp[0] = "NAME=headphone";
	data->hp_connected = event & SND_JACK_HEADPHONE ? true : false;
	state = data->hp_connected ? 2 : 0;
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}
	sprintf(buf, "STATE=%d", state);
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);
	return 0;
}

static int imx_sp2_audio_mic_jack_event(struct notifier_block *nb,
				     unsigned long event, void *handle)
{
	struct snd_soc_jack *jack = (struct snd_soc_jack *)handle;
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(jack->card);
	char *envp[3], *buf;
	int state=0;
	envp[0] = "NAME=microphone";
	data->mic_connected = event & SND_JACK_MICROPHONE ? true : false;
	state = data->mic_connected ? 2 : 0;
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}
	sprintf(buf, "STATE=%d", state);
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);
	return 0;
}

static int imx_sp2_audio_iphone_event(struct notifier_block *nb,
				     unsigned long event, void *handle)
{
	struct snd_soc_jack *jack = (struct snd_soc_jack *)handle;
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(jack->card);
	char *envp[3], *buf;
	int state=0;

	switch (event & SND_JACK_HEADSET) {
	case 0:
		envp[0] = (data->current_jack_report == SND_JACK_HEADSET) ?
				  "NAME=headset" :
				  "NAME=headphone";
		data->mic_connected = false;
		data->hp_connected = false;
		break;
	case SND_JACK_HEADSET:
		envp[0] = "NAME=headset";
		data->mic_connected = true;
		data->hp_connected = true;
		state = 2;
		break;
	case SND_JACK_HEADPHONE:
		envp[0] = "NAME=headphone";
		data->mic_connected = false;
		data->hp_connected = true;
		state = 2;
		break;
	default:
		return 0;
		break;
	}
	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&data->pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}
	sprintf(buf, "STATE=%d", state);
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&data->pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);
	return 0;
}

static struct notifier_block imx_sp2_jack_nb = {
	.notifier_call = imx_sp2_audio_jack_event,
};

static struct notifier_block imx_sp2_mic_jack_nb = {
	.notifier_call = imx_sp2_audio_mic_jack_event,
};

static struct notifier_block imx_sp2_iphone_nb = {
	.notifier_call = imx_sp2_audio_iphone_event,
};

static ssize_t show_headphone(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(card);

	if (data->hp_connected)
		strcpy(buf, "present\n");
	else
		strcpy(buf, "none\n");

	return strlen(buf);
}

static DEVICE_ATTR(headphone, S_IRUGO, show_headphone, NULL);

static ssize_t show_mic(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(card);

	if (data->mic_connected)
		strcpy(buf, "present\n");
	else
		strcpy(buf, "none\n");

	return strlen(buf);
}

static void imx_sp2_audio_read_bias_work(struct work_struct *work)
{
	int val, ret;
	struct imx_sp2_audio_data *data =
		container_of(work, struct imx_sp2_audio_data, mic_work.work);
	ret = iio_read_channel_raw(data->iioc, &val);
	if (ret < 0) {
		dev_warn(&data->pdev->dev, "Failed to read IIO channel (%d)\n",
			 ret);
	}
	ret = 0;
	if (val > 500 && val < 6000)
		ret = data->board_info->iphone_jack ? SND_JACK_HEADSET :
						      SND_JACK_MICROPHONE;
	else if (val >= 6000)
		ret = SND_JACK_HEADPHONE;

	if (ret != data->current_jack_report) {
		dev_dbg(&data->pdev->dev, "Bias current is %d\n", val);
		snd_soc_jack_report(&data->options.headphone_jack, ret,
				    SND_JACK_HEADSET);
		data->current_jack_report = ret;
	}
	queue_delayed_work(system_power_efficient_wq, &data->mic_work,
			   msecs_to_jiffies(500));
}

static DEVICE_ATTR(microphone, S_IRUGO, show_mic, NULL);

static int imx_sp2_hpjack_status_check(void *priv)
{
	struct imx_sp2_audio_data *data = priv;
	int hp_status, ret=0;
	if (!gpio_is_valid(data->options.hp_gpio))
		return 0;
	hp_status = gpio_get_value(data->options.hp_gpio) ? 1 : 0;

	dev_dbg(&data->pdev->dev, "%s: hpdet = %d (%d)\n", __func__, hp_status,
		data->options.hp_active_level);
	if(hp_status == data->options.hp_active_level) {
		ret = SND_JACK_HEADPHONE;
		data->hp_connected = true;
	} else {
		data->hp_connected = false;
	}
	dev_dbg(&data->pdev->dev, "%s: status = 0x%0x\n", __func__, ret);
	return ret;
}

static int imx_sp2_micjack_status_check(void *priv)
{
	struct imx_sp2_audio_data *data = priv;
	int mic_status, ret=0;
	if (!gpio_is_valid(data->options.mic_gpio))
		return 0;

	mic_status = gpio_get_value(data->options.mic_gpio) ? 1 : 0;
	if (mic_status == data->options.mic_active_level) {
		data->mic_connected = true;
		ret = SND_JACK_MICROPHONE;
	} else {
		data->mic_connected = false;
	}
	dev_dbg(&data->pdev->dev, "%s: status = 0x%0x\n", __func__, ret);
	return ret;
}


static const struct snd_soc_dapm_widget imx_sp2_audio_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Headphone Jack"),
	SND_SOC_DAPM_MIC("AMIC", 0),
	SND_SOC_DAPM_OUTPUT("Ext Spk"),
	SND_SOC_DAPM_REGULATOR_SUPPLY("SPKAMP", 0, 0),
};

static int imx_sp2_audio_jack_init(struct imx_sp2_audio_data *data)
{
	int ret;
	if (gpio_is_valid(data->options.hp_gpio)) {
		imx_hp_jack_gpio.gpio = data->options.hp_gpio;
		imx_hp_jack_gpio.data = data;
		imx_hp_jack_gpio.jack_status_check = imx_sp2_hpjack_status_check;
		imx_hp_jack_gpio.invert = data->options.hp_active_level ? 0 : 1;
		ret = snd_soc_card_jack_new(&data->card, "Headphone Jack",
					    SND_JACK_HEADPHONE,
					    &data->options.headphone_jack,
					    imx_hp_jack_pins,
					    ARRAY_SIZE(imx_hp_jack_pins));
		snd_soc_jack_add_gpios(&data->options.headphone_jack, 1,
				       &imx_hp_jack_gpio);
		snd_soc_jack_notifier_register(&data->options.headphone_jack,
					       &imx_sp2_jack_nb);
		data->options.hp_attr = true;
		data->options.mic_attr = true;
	} else if (data->board_info->iphone_jack) {
		dev_dbg(&data->pdev->dev, "Using headset jack\n");
		imx_hp_jack_gpio.gpio = -1;
		ret = snd_soc_card_jack_new(&data->card, "Headphone Jack",
					    SND_JACK_HEADSET,
					    &data->options.headphone_jack,
					    imx_hs_jack_pins,
					    ARRAY_SIZE(imx_hs_jack_pins));
		snd_soc_jack_notifier_register(&data->options.headphone_jack,
					       &imx_sp2_iphone_nb);
		data->options.hp_attr = true;
		data->options.mic_attr = true;
	}
	if (gpio_is_valid(data->options.mic_gpio)) {
		imx_mic_jack_gpio.gpio = data->options.mic_gpio;
		imx_mic_jack_gpio.data = data;
		imx_mic_jack_gpio.jack_status_check = imx_sp2_micjack_status_check;
		imx_mic_jack_gpio.invert =
			data->options.mic_active_level ? 0 : 1;

		snd_soc_card_jack_new(&data->card, "AMIC", SND_JACK_MICROPHONE,
				      &data->options.mic_jack,
				      imx_mic_jack_pins,
				      ARRAY_SIZE(imx_mic_jack_pins));
		snd_soc_jack_add_gpios(&data->options.mic_jack, 1,
				       &imx_mic_jack_gpio);
		snd_soc_jack_notifier_register(&data->options.mic_jack,
					       &imx_sp2_mic_jack_nb);
		data->options.mic_attr = true;
	}
	if (data->options.hp_attr) {
		ret = device_create_file(&data->pdev->dev, &dev_attr_headphone);
		if (ret) {
			dev_err(&data->pdev->dev,
				"create hp attr failed (%d)\n", ret);
		}
	}

	if (data->options.mic_attr) {
		ret = device_create_file(&data->pdev->dev, &dev_attr_microphone);
		if (ret) {
			dev_err(&data->pdev->dev, "create mic attr failed (%d)\n", ret);
		}
	}
	snd_soc_dapm_enable_pin(&data->card.dapm, "AMIC");
	snd_soc_dapm_sync(&data->card.dapm);
	data->jacks_initiated  = true;
	return 0;
}

static int imx_sp2_audio_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct device *dev = rtd->card->dev;
	int ret;

	ret = snd_soc_dai_set_sysclk(asoc_rtd_to_codec(rtd, 0), SGTL5000_SYSCLK,
				     data->clk_frequency, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(dev, "could not set codec driver clock params\n");
		return ret;
	}

	if (!data->board_info->codec_master) {
		ret = snd_soc_dai_set_sysclk(asoc_rtd_to_cpu(rtd, 0),
					     FSL_SAI_CLK_MAST1,
					     data->clk_frequency,
					     SND_SOC_CLOCK_OUT);
		if (ret) {
			dev_err(&data->pdev->dev,
				"failed to set cpu sysclk: %d\n", ret);
			return ret;
		}
	}
	return imx_sp2_audio_jack_init(data);
}


static int imx_sp2_audio_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	/* struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream); */
	return 0;
}

static const struct snd_kcontrol_new imx_sp2_audio_controls[] = {
	SOC_DAPM_PIN_SWITCH("Ext Spk"),
};
static const struct snd_soc_ops imx_sp2_audio_card_ops = {
	.hw_params = imx_sp2_audio_hw_params,
};

static const struct of_device_id imx_sp2_audio_dt_ids[] = {
	{ .compatible = "fsl,lm-imx-audio-sp2-audio", .data = &imx8_sp2_audio},
	{ .compatible = "fsl,lm-imx-audio-lb2-audio", .data = &imx8_lb2_audio},
	{ /* sentinel */ }
};

static int imx_sp2_audio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cpu_np=0, *codec_np=0;
	struct platform_device *cpu_pdev = 0;
	struct i2c_client *codec_i2c = 0;
	struct imx_sp2_audio_data *data;
	struct clk *codec_clk=0;
	int ret;
	enum of_gpio_flags gpio_flags;
	const struct of_device_id *of_id = of_match_device(imx_sp2_audio_dt_ids, &pdev->dev);
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
		dev_err(&pdev->dev, "failed to find SAI platform device\n");
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
		dev_err_probe(&codec_i2c->dev, ret, "failed to get codec clk: %d\n", ret);
		goto cleanup;
	}

	data->clk_frequency = clk_get_rate(codec_clk);
	clk_put(codec_clk);
	dev_info(&pdev->dev, "%s: codec clock is %d\n", __func__, data->clk_frequency);

	data->options.hp_gpio = of_get_named_gpio_flags(np, "hp-det-gpios", 0, &gpio_flags);
	if (gpio_is_valid(data->options.hp_gpio))
		data->options.hp_active_level = (gpio_flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;

	data->options.mic_gpio = of_get_named_gpio_flags(np, "mic-det-gpios", 0, &gpio_flags);
	if (gpio_is_valid(data->options.mic_gpio))
		data->options.mic_active_level = gpio_flags & OF_GPIO_ACTIVE_LOW ? 0 : 1;

	INIT_DELAYED_WORK(&data->mic_work, imx_sp2_audio_read_bias_work);
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
	data->dai.ops = &imx_sp2_audio_card_ops;
	data->dai.init = &imx_sp2_audio_dai_init;

	if (board_info->codec_master)
		data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBP_CFP;
	else
		data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	data->dai.codecs->dai_name = "sgtl5000";
	data->dai.symmetric_channels = 1;

	data->card.dev = &pdev->dev;
	/* data->card.late_probe = imx_sp2_audio_late_probe; */
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse card name\n");
		goto cleanup;
	}

	if (data->board_info->has_speaker) {
		data->card.controls = imx_sp2_audio_controls;
		data->card.num_controls = ARRAY_SIZE(imx_sp2_audio_controls);
	}
	data->card.num_links = 1;
	data->card.owner = THIS_MODULE;
	data->card.dai_link = &data->dai;
	data->card.dapm_widgets = imx_sp2_audio_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_sp2_audio_dapm_widgets);
	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	data->iioc = iio_channel_get(&pdev->dev, "miccurrent");
	if (IS_ERR_OR_NULL(data->iioc)) {
		if (PTR_ERR(data->iioc) == -EPROBE_DEFER) {
			dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				      "Wait on IIO\n");
			ret = -EPROBE_DEFER;
			goto cleanup;
		} else {
			data->iioc = NULL;
		}
	}
	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse audio routing\n");
		goto cleanup;
	}
	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "register_card failed (%d)\n", ret);
		goto cleanup;
	}
	if (data->board_info->iphone_jack && !IS_ERR_OR_NULL(data->iioc)) {
		queue_delayed_work(system_power_efficient_wq, &data->mic_work,
				   msecs_to_jiffies(500));
	}
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

static int imx_sp2_audio_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(card);
	if (data->options.mic_attr)
		device_remove_file(&pdev->dev, &dev_attr_microphone);
	if (data->options.hp_attr)
		device_remove_file(&pdev->dev, &dev_attr_headphone);
	return 0;
}

void imx_sp2_audio_shutdown(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_sp2_audio_data *data = snd_soc_card_get_drvdata(card);
	snd_soc_dapm_disable_pin(&data->card.dapm, "Headphone Jack");
	snd_soc_dapm_disable_pin(&data->card.dapm, "Ext Spk");
	snd_soc_dapm_sync(&data->card.dapm);
}

MODULE_DEVICE_TABLE(of, imx_sp2_audio_dt_ids);

static struct platform_driver lm_imx_sp2_audio_driver = {
	.driver = {
		.name = "lm-imx-sp2_audio",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_sp2_audio_dt_ids,
	},
	.probe = imx_sp2_audio_probe,
	.remove = imx_sp2_audio_remove,
	.shutdown = imx_sp2_audio_shutdown,
};
module_platform_driver(lm_imx_sp2_audio_driver);

MODULE_AUTHOR("DATA RESPONS AS");
MODULE_DESCRIPTION("Laerdal i.MX SGTL5000 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm-imx-sp2_audio");
