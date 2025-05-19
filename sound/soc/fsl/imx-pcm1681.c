/*
 * Copyright (C) 2022 DATA RESPONS AS.
 *
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
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/power_supply.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include "fsl_esai.h"
#include "fsl_sai.h"

#define DAI_NAME_SIZE	32

enum cpu_type_id {CPUTYPE_IMX8, CPUTYPE_IMX6};

struct board_variant {
	enum cpu_type_id cpu_type;
};

const struct board_variant imx6_pcm1681 = { .cpu_type = CPUTYPE_IMX6 };
const struct board_variant imx8_pcm1681 = { .cpu_type = CPUTYPE_IMX8 };


struct imx_pcm1681_data {
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	struct snd_soc_dai_link_component comp[3];
	char codec_dai_name[DAI_NAME_SIZE];
	char platform_name[DAI_NAME_SIZE];
	struct gpio_descs *shutdown_gpios;
	const struct board_variant *board_info;
	unsigned int clk_frequency;
};

static const struct snd_soc_dapm_widget imx_pcm1681_dapm_widgets[] = {
	SND_SOC_DAPM_HP("OUT1", NULL),
	SND_SOC_DAPM_HP("OUT2", NULL),
	SND_SOC_DAPM_HP("OUT3", NULL),
	SND_SOC_DAPM_HP("OUT4", NULL),
	SND_SOC_DAPM_HP("OUT5", NULL),
	SND_SOC_DAPM_HP("OUT6", NULL),
	SND_SOC_DAPM_HP("OUT7", NULL),
	SND_SOC_DAPM_HP("OUT8", NULL),
};

static const struct snd_soc_dapm_route audio_map_1681[] = {
	{"OUT1", NULL, "VOUT1"},
	{"OUT2", NULL, "VOUT2"},
	{"OUT3", NULL, "VOUT3"},
	{"OUT4", NULL, "VOUT4"},
	{"OUT5", NULL, "VOUT5"},
	{"OUT6", NULL, "VOUT6"},
	{"OUT7", NULL, "VOUT7"},
	{"OUT8", NULL, "VOUT8"},

};

static int imx_pcm1681_hw_param(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	unsigned int sample_rate = params_rate(params);
	unsigned ch = params_channels(params);
	snd_pcm_format_t sample_format = params_format(params);
	int ret = 0;
	int slotw = 32;
	u32 width = snd_pcm_format_width(params_format(params));
	unsigned int clock_freq = 0;
	switch (ch) {
	case 2:
		switch (sample_format) {
		case SNDRV_PCM_FORMAT_S16_LE:
			clock_freq = sample_rate * 32;
			slotw = 16;
			break;
		case SNDRV_PCM_FORMAT_S24_LE:
		case SNDRV_PCM_FORMAT_S32_LE:
			clock_freq = sample_rate * 64;
			break;
		default:
			return -EINVAL;
			break;
		}
		snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0, 0, 0);
		snd_soc_dai_set_tdm_slot(codec_dai, 0, 0, 0, 0);
		break;
	case 8:
		if (width >= 24) {
			clock_freq = sample_rate * 256;
		} else {
			dev_err(rtd->dev, "%s: only S24_LE and S32_LE supported for TDM, was %d\n", __func__,
				sample_format);
			return -EINVAL;
		}
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, (1 << ch) - 1, 0x0, ch, slotw);
		if (ret) {
			dev_err(rtd->dev, "%s: failed to set cpu tdm fmt: %d\n", __func__, ret);
			return ret;
		}

		ret = snd_soc_dai_set_tdm_slot(codec_dai, (1 << ch) - 1, 0x0, ch, slotw);
		if (ret) {
			dev_err(rtd->dev, "%s: failed to set codec tdm fmt: %d\n", __func__, ret);
			return ret;
		}
		break;
	default:

		dev_err(rtd->dev, "%s: 2 or 8 channels must be used\n", __func__);
		return -EINVAL;
		break;
	}

	dev_dbg(rtd->dev, "%s: SAI clock is %d, rate is %d\n", __func__, clock_freq, sample_rate);

	return 0;
}

static void imx_pcm1681_set_amps(struct imx_pcm1681_data *priv, bool off)
{
	int n;
	if (priv->shutdown_gpios) {
		for (n=0; n < priv->shutdown_gpios->ndescs; n++) {
				gpiod_set_value(priv->shutdown_gpios->desc[n], off ? 1 : 0 );
		}
	}
}

static int imx_pcm1681_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct imx_pcm1681_data *data = snd_soc_card_get_drvdata(rtd->card);
	imx_pcm1681_set_amps(data, false);
	return 0;
}

static void imx_pcm1681_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct imx_pcm1681_data *data = snd_soc_card_get_drvdata(rtd->card);
	imx_pcm1681_set_amps(data, true);
}

static struct snd_soc_ops imx_hifi_ops = {
	.hw_params = imx_pcm1681_hw_param,
	.startup = imx_pcm1681_startup,
	.shutdown = imx_pcm1681_shutdown,
};

static int imx_pcm1681_late_probe(struct snd_soc_card *card)
{
	int ret = 0;
	struct imx_pcm1681_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = list_first_entry(
		&card->rtd_list, struct snd_soc_pcm_runtime, list);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);

	switch (data->board_info->cpu_type) {
	case CPUTYPE_IMX6:
		dev_info(card->dev, "Set ESAI clk to %d\n", data->clk_frequency);
		ret = snd_soc_dai_set_sysclk(cpu_dai, ESAI_HCKT_EXTAL,
					     data->clk_frequency,
					     SND_SOC_CLOCK_IN);
		break;

	case CPUTYPE_IMX8:
		dev_info(card->dev, "Set SAI mclk to %d\n", data->clk_frequency);
		ret = snd_soc_dai_set_sysclk(cpu_dai, FSL_SAI_CLK_MAST1,
					     data->clk_frequency,
					     SND_SOC_CLOCK_OUT);
		break;

	default:
		break;
	}
	if (ret) {
		dev_err(card->dev,
			"failed to set cpu sysclk: %d\n", ret);
		goto out;
	}

out:
	return ret;
}

static const struct of_device_id imx_pcm1681_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-pcm1681", .data = &imx6_pcm1681},
	{ .compatible = "fsl,imx-audio-mx8-pcm1681", .data = &imx8_pcm1681},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_pcm1681_dt_ids);

static int imx_pcm1681_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np=0;
	struct device_node *codec_np=0;
	struct platform_device *cpu_pdev;
	struct i2c_client *codec_dev;
	struct imx_pcm1681_data *data;
	struct clk *codec_clk;
	int ret;
	const struct of_device_id *of_id = of_match_device(imx_pcm1681_dt_ids, &pdev->dev);
	const struct board_variant *board_info = of_id ? (struct board_variant*)of_id->data : NULL;
	if (!board_info) {
		return -ENODATA;
	}

	cpu_np = of_parse_phandle(pdev->dev.of_node, "audio-cpu", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		ret = -ENODEV;
		goto cleanup;
	}

	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -ENODEV;
		goto cleanup;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_dbg(&pdev->dev, "failed to find CPU platform device - deferring\n");
		ret = -EPROBE_DEFER;
		goto cleanup;
	}
	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev) {
		dev_dbg(&pdev->dev, "failed (%ld) to find codec platform device\n", PTR_ERR(codec_dev));
		ret = -EPROBE_DEFER;
		goto cleanup;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto cleanup;
	}
	data->board_info = board_info;
	codec_clk = clk_get(&codec_dev->dev, NULL);
	if (IS_ERR(codec_clk)) {
		ret = PTR_ERR(codec_clk);
		dev_err_probe(&pdev->dev, ret, "failed to get codec clock\n");
		goto cleanup;
	}
	data->clk_frequency = clk_get_rate(codec_clk);
	clk_put(codec_clk);
	dev_info(&pdev->dev, "%s: PLL (HFTXC) = %d\n", __func__, data->clk_frequency);

	data->shutdown_gpios = devm_gpiod_get_array_optional(&pdev->dev, "amp-shutdown-gpios", GPIOD_OUT_HIGH);
	data->dai.name = "HiFi";
	data->dai.stream_name = "HiFi";
	data->dai.num_cpus	= 1;
	data->dai.num_codecs	= 1;
	data->dai.num_platforms	= 1;

	data->dai.codecs = &data->comp[1];
	data->dai.codecs->dai_name = "pcm1681-hifi";
	data->dai.codecs->of_node = codec_np;

	data->dai.cpus = &data->comp[0];
	data->dai.cpus->of_node = cpu_np;

	data->dai.platforms = &data->comp[2];
	data->dai.platforms->of_node = cpu_np;

	data->dai.ops = &imx_hifi_ops;
	data->dai.dai_fmt = SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_NB_IF |
			    SND_SOC_DAIFMT_CBS_CFS;

	data->card.dev = &pdev->dev;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret) {
		dev_err(&pdev->dev, "DT must supply model attribute");
		goto cleanup;
	}
	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret)
		goto cleanup;
	data->card.num_links = 1;
	data->card.dai_link = &data->dai;
	data->card.dapm_widgets = imx_pcm1681_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_pcm1681_dapm_widgets);
	data->card.late_probe = imx_pcm1681_late_probe;

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "snd_soc_register_card failed (%d)\n", ret);
	}

cleanup:
	if (cpu_pdev)
		put_device(&cpu_pdev->dev);
	if (codec_dev)
		put_device(&codec_dev->dev);
	if (cpu_np)
		of_node_put(cpu_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static int imx_pcm1681_remove(struct platform_device *pdev)
{
	return 0;
}



static struct platform_driver imx_pcm1681_driver = {
	.driver = {
		.name = "imx-pcm1681",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_pcm1681_dt_ids,
	},
	.probe = imx_pcm1681_probe,
	.remove = imx_pcm1681_remove,
};
module_platform_driver(imx_pcm1681_driver);

MODULE_AUTHOR("DATA RESPONS AS");
MODULE_DESCRIPTION("Freescale i.MX pcm1681 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-pcm1681");
