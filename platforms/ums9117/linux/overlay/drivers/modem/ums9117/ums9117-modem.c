// SPDX-License-Identifier: GPL-2.0-only
/*
 * UMS9117 cellular DSP loader foundation.
 *
 * Loads the stock GSM/LTE CEVA layer-1 images into their fixed physical
 * regions and exposes a minimal sysfs interface for checking the result.
 * The header offset is configurable per image because the GSM partition has
 * a 0x80-byte "V1GGE_Bugid..." header while the LTE partition starts at 0.
 *
 * Deliberately does NOT release DSP resets or touch clocks: that sequence is
 * still being reconstructed from the MOCOR binary. Enabling this driver
 * requires the DSP regions to be excluded from System RAM in the device tree.
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#define GSM_MAGIC "V1GGE_Bugid__990513_"

struct dsp_image {
	const char *name;
	phys_addr_t phys;
	size_t region;
	size_t header;
};

struct ums9117_modem {
	struct device *dev;
	struct dsp_image gsm;
	struct dsp_image lte;
	void __iomem *gsm_virt;
	void __iomem *lte_virt;
	bool loaded;
};

static int load_one(struct ums9117_modem *modem, const struct dsp_image *img,
		    void __iomem *virt, bool check_magic)
{
	const struct firmware *fw;
	const u8 *payload;
	size_t len;
	int ret;

	ret = request_firmware_direct(&fw, img->name, modem->dev);
	if (ret) {
		dev_err(modem->dev, "failed to load %s: %d\n", img->name, ret);
		return ret;
	}
	if (fw->size <= img->header) {
		dev_err(modem->dev, "%s too small (%zu)\n", img->name, fw->size);
		ret = -EINVAL;
		goto out;
	}
	if (check_magic &&
	    memcmp(fw->data, GSM_MAGIC, strlen(GSM_MAGIC))) {
		dev_err(modem->dev, "%s bad magic\n", img->name);
		ret = -EINVAL;
		goto out;
	}

	payload = fw->data + img->header;
	len = fw->size - img->header;
	if (len > img->region) {
		dev_err(modem->dev, "%s too large (%zu > %zu)\n",
			img->name, len, img->region);
		ret = -EINVAL;
		goto out;
	}

	memcpy_toio(virt, payload, len);
	memset_io((void __iomem *)((uintptr_t)virt + len), 0, img->region - len);
	dev_info(modem->dev, "loaded %s: %zu bytes at %pa\n",
		 img->name, len, &img->phys);
out:
	release_firmware(fw);
	return ret;
}

static int do_load(struct ums9117_modem *modem)
{
	int ret;

	if (modem->loaded)
		return 0;
	ret = load_one(modem, &modem->gsm, modem->gsm_virt, true);
	if (ret)
		return ret;
	ret = load_one(modem, &modem->lte, modem->lte_virt, false);
	if (ret)
		return ret;
	modem->loaded = true;
	return 0;
}

static ssize_t loaded_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  ((struct ums9117_modem *)dev_get_drvdata(dev))->loaded);
}

static ssize_t load_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	int ret;

	if (count && buf[0] == '1') {
		ret = do_load(modem);
		if (ret)
			return ret;
	}
	return count;
}

static DEVICE_ATTR_RO(loaded);
static DEVICE_ATTR_WO(load);

static struct attribute *modem_attrs[] = {
	&dev_attr_loaded.attr,
	&dev_attr_load.attr,
	NULL,
};
ATTRIBUTE_GROUPS(modem);

static int ums9117_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_modem *modem;
	struct resource *res;

	modem = devm_kzalloc(dev, sizeof(*modem), GFP_KERNEL);
	if (!modem)
		return -ENOMEM;
	modem->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-gsm");
	if (!res)
		return -ENODEV;
	modem->gsm.phys = res->start;
	modem->gsm.region = resource_size(res);
	modem->gsm_virt = devm_ioremap_resource(dev, res);
	if (IS_ERR(modem->gsm_virt))
		return PTR_ERR(modem->gsm_virt);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-lte");
	if (!res)
		return -ENODEV;
	modem->lte.phys = res->start;
	modem->lte.region = resource_size(res);
	modem->lte_virt = devm_ioremap_resource(dev, res);
	if (IS_ERR(modem->lte_virt))
		return PTR_ERR(modem->lte_virt);

	{
		u32 header;

		modem->gsm.header = 0x80;   /* V1GGE_Bugid header */
		modem->lte.header = 0;      /* raw CEVA image */
		if (!of_property_read_u32(dev->of_node, "fplinux,gsm-header", &header))
			modem->gsm.header = header;
		if (!of_property_read_u32(dev->of_node, "fplinux,lte-header", &header))
			modem->lte.header = header;
	}

	if (of_property_read_string(dev->of_node, "firmware-name-gsm",
				    &modem->gsm.name))
		modem->gsm.name = "ums9117/gsm_dsp.bin";
	if (of_property_read_string(dev->of_node, "firmware-name-lte",
				    &modem->lte.name))
		modem->lte.name = "ums9117/lte_dsp.bin";

	platform_set_drvdata(pdev, modem);
	return sysfs_create_groups(&dev->kobj, modem_groups);
}

static void ums9117_modem_remove(struct platform_device *pdev)
{
	sysfs_remove_groups(&pdev->dev.kobj, modem_groups);
}

static const struct of_device_id ums9117_modem_of_match[] = {
	{ .compatible = "fplinux,ums9117-modem" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ums9117_modem_of_match);

static struct platform_driver ums9117_modem_driver = {
	.probe = ums9117_modem_probe,
	.remove = ums9117_modem_remove,
	.driver = {
		.name = "ums9117-modem",
		.of_match_table = ums9117_modem_of_match,
	},
};
module_platform_driver(ums9117_modem_driver);

MODULE_DESCRIPTION("UMS9117 cellular DSP loader foundation");
MODULE_LICENSE("GPL");
