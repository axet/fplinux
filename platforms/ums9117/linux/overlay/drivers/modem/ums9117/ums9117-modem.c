// SPDX-License-Identifier: GPL-2.0-only
/*
 * UMS9117 cellular DSP loader.
 *
 * Loads the stock GSM/LTE CEVA layer-1 images into their fixed physical
 * regions and exposes a minimal sysfs interface for checking the result.
 * The header offset is configurable per image because the GSM partition has
 * a 0x80-byte "V1GGE_Bugid..." header while the LTE partition starts at 0.
 *
 * The optional "power_on" attribute reproduces the AON/PMU register sequence
 * reconstructed from the MOCOR CP (LTE ADX init) and polls for the DSP ready
 * magic in the LTE shared-memory page. It is opt-in: probing never touches
 * clocks or resets on its own. Enabling this driver requires the DSP regions
 * to be excluded from System RAM in the device tree.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mfd/syscon.h>
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#define GSM_MAGIC "V1GGE_Bugid__990513_"

/* AON_APB (0x402e0000) offsets touched by the CP LTE ADX DSP init. */
#define AON_DSP_CTRL		0x24c	/* bit0 toggled around DSP start */
#define AON_CLK_EB		0x130	/* bit16 = DSP clock enable */
#define AON_DSP_VEC		0x248	/* DSP boot/vector value */
#define AON_MISC_CFG		0x6c	/* OR 0x9620 during init */

/* PMU_APB (0x402b0000) offsets written by FUN_80c25904(1). */
#define PMU_DSP_CLK_EB0		0x2044
#define PMU_DSP_CFG		0x20cc
#define PMU_DSP_CLK_EB1		0x2034

/* LTE DSP shared-memory page (0x80447000) +0xe14 ready magic. */
#define DSP_READY_OFFSET	0xe14
#define DSP_READY_MAGIC		0x22222222
#define DSP_READY_POLLS		100
#define DSP_READY_DELAY_MS	10

/* CP DAT_81147190 = 0x80448400; stored in AON_DSP_VEC as (addr >> 1). */
#define DSP_VECTOR_VALUE	(0x80448400U >> 1)

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
	void __iomem *shared_virt;
	struct regmap *aon_apb;
	struct regmap *pmu_apb;
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

static int dsp_ready(struct ums9117_modem *modem)
{
	return readl(modem->shared_virt + DSP_READY_OFFSET) == DSP_READY_MAGIC;
}

/* Reproduce the AON/PMU sequence from the CP LTE ADX DSP init. */
static int dsp_power_on(struct ums9117_modem *modem)
{
	int ret, i;

	if (!modem->gsm_loaded || !modem->lte_loaded)
		return -EINVAL;
	if (!modem->aon_apb || !modem->pmu_apb || !modem->shared_virt)
		return -ENODEV;

	/* FUN_81146c24: clear DSP start bit, enable DSP clock. */
	ret = regmap_update_bits(modem->aon_apb, AON_DSP_CTRL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_CLK_EB,
				 BIT(16), BIT(16));
	if (ret)
		return ret;

	/* FUN_80c25904(1): PMU DSP clock/power configuration. */
	ret = regmap_write(modem->pmu_apb, PMU_DSP_CLK_EB0, 0x02000000);
	if (ret)
		return ret;
	ret = regmap_write(modem->pmu_apb, PMU_DSP_CLK_EB0, 0x01000000);
	if (ret)
		return ret;
	ret = regmap_write(modem->pmu_apb, PMU_DSP_CFG, 0x00020000);
	if (ret)
		return ret;
	ret = regmap_write(modem->pmu_apb, PMU_DSP_CLK_EB1, 0x03000000);
	if (ret)
		return ret;

	/* FUN_81146c3a: poll the DSP ready magic (CP does 100 x 10 ms). */
	for (i = 0; i < DSP_READY_POLLS; i++) {
		if (dsp_ready(modem))
			break;
		mdelay(DSP_READY_DELAY_MS);
	}

	/* Tail of FUN_81146dd8: interrupt/vector handshake. */
	ret = regmap_update_bits(modem->aon_apb, AON_MISC_CFG,
				 0x9620, 0x9620);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_DSP_CTRL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_CLK_EB,
				 BIT(16), BIT(16));
	if (ret)
		return ret;
	ret = regmap_write(modem->aon_apb, AON_DSP_VEC, DSP_VECTOR_VALUE);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_DSP_CTRL, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_CLK_EB, BIT(16), 0);
	if (ret)
		return ret;

	dev_info(modem->dev,
		 "DSP power-on done; ready=%d magic=0x%08x\n",
		 dsp_ready(modem),
		 readl(modem->shared_virt + DSP_READY_OFFSET));
	return dsp_ready(modem) ? 0 : -ETIMEDOUT;
}

static ssize_t dsp_ready_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	if (!modem->shared_virt)
		return -ENODEV;
	return sysfs_emit(buf, "0x%08x\n",
			  readl(modem->shared_virt + DSP_READY_OFFSET));
}

static ssize_t power_on_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	int ret;

	if (!count || buf[0] != '1')
		return count;
	ret = dsp_power_on(modem);
	if (ret)
		return ret;
	return count;
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
static DEVICE_ATTR_RO(dsp_ready);
static DEVICE_ATTR_WO(load);
static DEVICE_ATTR_WO(load_gsm);
static DEVICE_ATTR_WO(load_lte);
static DEVICE_ATTR_WO(power_on);

static struct attribute *modem_attrs[] = {
	&dev_attr_loaded.attr,
	&dev_attr_dsp_ready.attr,
	&dev_attr_load.attr,
	&dev_attr_load_gsm.attr,
	&dev_attr_load_lte.attr,
	&dev_attr_power_on.attr,
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

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-lte");
	if (!res)
		return -ENODEV;
	modem->lte.phys = res->start;
	modem->lte.region = resource_size(res);

	/* A bulk image copy into System RAM would corrupt the running kernel and
	 * hard-reset the phone. Refuse to probe unless both DSP windows are
	 * excluded from System RAM (the DTB memory node must start above them). */
	if (region_intersects(modem->gsm.phys, modem->gsm.region,
			      IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) !=
	    REGION_DISJOINT ||
	    region_intersects(modem->lte.phys, modem->lte.region,
			      IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) !=
	    REGION_DISJOINT)
		return dev_err_probe(dev, -EBUSY,
				     "DSP regions overlap System RAM\n");

	/* The DT reserved-memory node already owns these regions. They are SRAM,
	 * not MMIO devices, so map them as normal memory; ioremap(MT_DEVICE) is
	 * rejected for addresses memblock still reports as mapped RAM. */
	modem->gsm_virt = (void __iomem *)devm_memremap(dev, modem->gsm.phys,
			modem->gsm.region, MEMREMAP_WB);
	if (!modem->gsm_virt)
		return -ENOMEM;
	modem->lte_virt = (void __iomem *)devm_memremap(dev, modem->lte.phys,
			modem->lte.region, MEMREMAP_WB);
	if (!modem->lte_virt)
		return -ENOMEM;

	/* LTE DSP shared-memory page holds the ready magic at +0xe14. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-shared");
	if (res) {
		modem->shared_virt = (void __iomem *)devm_memremap(
			dev, res->start, resource_size(res), MEMREMAP_WB);
		if (!modem->shared_virt)
			return -ENOMEM;
	}

	/* AON/PMU regmaps are optional so the loader still works without them. */
	modem->aon_apb = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "fplinux,aon-apb");
	if (IS_ERR(modem->aon_apb))
		modem->aon_apb = NULL;
	modem->pmu_apb = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "fplinux,pmu-apb");
	if (IS_ERR(modem->pmu_apb))
		modem->pmu_apb = NULL;

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
