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
#include <linux/memremap.h>
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
	bool gsm_loaded;
	bool lte_loaded;
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
		dev_warn(modem->dev, "%s truncated to region (%zu > %zu)\n",
			 img->name, len, img->region);
		len = img->region;
	}

	memcpy_toio(virt, payload, len);
	memset_io((void __iomem *)((uintptr_t)virt + len), 0, img->region - len);
	dev_info(modem->dev, "loaded %s: %zu bytes at %pa\n",
		 img->name, len, &img->phys);
out:
	release_firmware(fw);
	return ret;
}

static int load_gsm(struct ums9117_modem *modem)
{
	int ret;

	if (modem->gsm_loaded)
		return 0;
	ret = load_one(modem, &modem->gsm, modem->gsm_virt, true);
	if (!ret)
		modem->gsm_loaded = true;
	return ret;
}

static int load_lte(struct ums9117_modem *modem)
{
	int ret;

	if (modem->lte_loaded)
		return 0;
	ret = load_one(modem, &modem->lte, modem->lte_virt, false);
	if (!ret)
		modem->lte_loaded = true;
	return ret;
}

static ssize_t loaded_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", modem->gsm_loaded && modem->lte_loaded);
}

static ssize_t load_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	int ret;

	if (count && buf[0] == '1') {
		ret = load_gsm(modem);
		if (ret)
			return ret;
		ret = load_lte(modem);
		if (ret)
			return ret;
	}
	return count;
}

static ssize_t load_gsm_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (count && buf[0] == '1')
		return load_gsm(dev_get_drvdata(dev)) ?: count;
	return count;
}

static ssize_t load_lte_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (count && buf[0] == '1')
		return load_lte(dev_get_drvdata(dev)) ?: count;
	return count;
}

static DEVICE_ATTR_RO(loaded);
static DEVICE_ATTR_WO(load);
static DEVICE_ATTR_WO(load_gsm);
static DEVICE_ATTR_WO(load_lte);

static struct attribute *modem_attrs[] = {
	&dev_attr_loaded.attr,
	&dev_attr_load.attr,
	&dev_attr_load_gsm.attr,
	&dev_attr_load_lte.attr,
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
	/* The DT reserved-memory node already owns this region. It is SRAM, not
	 * an MMIO device, so map it as normal memory; ioremap(MT_DEVICE) is
	 * rejected for addresses memblock still reports as mapped RAM. */
	modem->gsm_virt = (void __iomem *)devm_memremap(dev, res->start, resource_size(res), MEMREMAP_WB);
	if (!modem->gsm_virt)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-lte");
	if (!res)
		return -ENODEV;
	modem->lte.phys = res->start;
	modem->lte.region = resource_size(res);
	modem->lte_virt = (void __iomem *)devm_memremap(dev, res->start, resource_size(res), MEMREMAP_WB);
	if (!modem->lte_virt)
		return -ENOMEM;

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
