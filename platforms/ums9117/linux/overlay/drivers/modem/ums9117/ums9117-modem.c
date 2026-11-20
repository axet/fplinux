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
 *
 * The stock CP also writes one calibration/NV configuration word into the LTE
 * shared page at +0xe00 before starting the DSP (FUN_811c2414). Its value is
 * derived from a runtime NV lookup that is not yet reproducible offline, so
 * "nv_stub" lets userspace supply the final word; "power_on" applies it after
 * the cold-start setup and before the AON vector handshake when configured.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <linux/sysfs.h>

#define GSM_MAGIC "V1GGE_Bugid__990513_"

/* AON_APB (0x402e0000) offsets touched by the CP LTE ADX DSP init. */
#define AON_DSP_CTRL 0x24c /* bit0 toggled around DSP start */
#define AON_CLK_EB 0x130 /* bit16 = DSP clock enable */
#define AON_DSP_VEC 0x248 /* DSP boot/vector value */
#define AON_MISC_CFG 0x6c /* OR 0x9620 during init */

/* PMU_APB (0x402b0000) offsets written by FUN_80c25904(1). */
#define PMU_DSP_CLK_EB0 0x2044
#define PMU_DSP_CFG 0x20cc
#define PMU_DSP_CLK_EB1 0x2034

/* LTE DSP shared-memory page (0x80447000). */
/* FUN_811c2414 writes the NV-derived configuration word here. */
#define DSP_NV_STUB_OFFSET 0xe00
#define DSP_READY_OFFSET 0xe14
#define DSP_READY_MAGIC 0x22222222
#define DSP_READY_POLLS 100
#define DSP_READY_DELAY_MS 10

/* CP DAT_81147190 = 0x80448400; stored in AON_DSP_VEC as (addr >> 1). */
#define DSP_VECTOR_VALUE (0x80448400U >> 1)

/* CP FUN_811c2458 stages 55 NV records contiguously at 0x80670000 (offset
 * 0x228000 inside the 0x80448000 LTE load window) for 456124 bytes.
 */
#define LTE_NV_STAGE_OFFSET 0x228000
#define LTE_NV_STAGE_SIZE 456124

#define LTEC_MAGIC "LTEC"
#define LTEC_VERSION 1

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
	struct mbox_client mbox_client;
	struct mbox_chan *dsp_chan;
	struct class *status_class;
	struct device *status_dev;
	u32 nv_stub;
	bool nv_stub_set;
	u32 nv_e00_hi;
	bool nv_e00_hi_set;
	u32 nv_e04_mask;
	bool nv_e04_mask_set;
	bool gsm_loaded;
	bool lte_loaded;
	bool lte_nv_staged;
	unsigned int mbox_rx_count;
	u32 mbox_rx_last[2];
	/* Optional LTE calibration container (LTEC) used for P0 NV staging. */
	const char *lte_calib_name;
	/*
	 * Published by the Linux NAS/AT producer through sysfs. -1 and an empty
	 * name mean unknown; the loader never guesses either value from the DSP.
	 */
	int signal_level;
	char operator_name[64];
};

struct ltec_record {
	u16 id;
	u16 reserved;
	u32 len;
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
		dev_err(modem->dev, "%s too small (%zu)\n", img->name,
			fw->size);
		ret = -EINVAL;
		goto out;
	}
	if (check_magic && memcmp(fw->data, GSM_MAGIC, strlen(GSM_MAGIC))) {
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
	memset_io((void __iomem *)((uintptr_t)virt + len), 0,
		  img->region - len);
	dev_info(modem->dev, "loaded %s: %zu bytes at %pa\n", img->name, len,
		 &img->phys);
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

static void ums9117_modem_rx(struct mbox_client *cl, void *msg)
{
	struct ums9117_modem *modem = dev_get_drvdata(cl->dev);
	u32 *words = msg;

	modem->mbox_rx_count++;
	if (words) {
		modem->mbox_rx_last[0] = words[0];
		modem->mbox_rx_last[1] = words[1];
		dev_info(modem->dev, "DSP mbox rx: %08x %08x (count=%u)\n",
			 words[0], words[1], modem->mbox_rx_count);
	} else {
		dev_info(modem->dev, "DSP mbox rx: (null) (count=%u)\n",
			 modem->mbox_rx_count);
	}
}

/*
 * FUN_811c2458 staging order (decoded from CP payload): 55 physical LTE NV
 * records copied contiguously to 0x80670000. Total 456124 bytes. The source
 * is the LTEC calibration container (physical NV ids).
 */
static const u16 lte_nv_stage_ids[55] = {
	0x0a05, 0x09c4, 0x0a04, 0x09c8, 0x09c9, 0x09ca, 0x09cb, 0x09cc,
	0x09cd, 0x09ce, 0x09cf, 0x09d0, 0x09d1, 0x09d2, 0x09d3, 0x09d4,
	0x09d5, 0x09d6, 0x09d7, 0x0a01, 0x0a02, 0x0a08, 0x0a09, 0x0a0a,
	0x0a28, 0x0a30, 0x0a29, 0x0a31, 0x0a32, 0x0a33, 0x0a26, 0x0a2c,
	0x0a2f, 0x0a2d, 0x0a2b, 0x0a2e, 0x0a21, 0x0a22, 0x0a27, 0x0a23,
	0x0a24, 0x0a34, 0x0a35, 0x0a25, 0x0a2a, 0x0514, 0x0519, 0x051a,
	0x051f, 0x051c, 0x0528, 0x0841, 0x09c7, 0x051e, 0x0aa2,
};

static const struct firmware *ltec_find_record(const struct firmware *fw,
					       u16 id, const u8 **data,
					       u32 *len)
{
	const u8 *p = fw->data;
	size_t off;
	u16 magic_ver_src[4];
	u32 count;
	u32 i;

	if (fw->size < 12)
		return NULL;
	/* header: magic[4], version u16, source u16, count u32 */
	if (memcmp(p, LTEC_MAGIC, 4) != 0)
		return NULL;
	/* version */ if (get_unaligned_le16(p + 4) != LTEC_VERSION)
		return NULL;
	count = get_unaligned_le32(p + 8);
	off = 12;
	for (i = 0; i < count; i++) {
		u16 rid, rsv;
		u32 rlen, pad;

		if (off + 8 > fw->size)
			return NULL;
		rid = get_unaligned_le16(p + off);
		rsv = get_unaligned_le16(p + off + 2);
		rlen = get_unaligned_le32(p + off + 4);
		(void)rsv;
		off += 8;
		pad = (rlen + 3) & ~3U;
		if (off + pad > fw->size)
			return NULL;
		if (rid == id) {
			*data = p + off;
			*len = rlen;
			return fw;
		}
		off += pad;
	}
	(void)magic_ver_src;
	return NULL;
}

/* Stage calibration NV records in CP FUN_811c2458 order and derive +e00/+e04
 * side effects (FUN_811c2414 / tail). Best-effort: uses saved phone data only.
 */
static int dsp_stage_lte_calibration(struct ums9117_modem *modem)
{
	const struct firmware *fw;
	const u8 *rec;
	u32 rec_len, nv9 = 0;
	size_t off = 0;
	int i, ret;
	u32 e00, e04;

	modem->lte_nv_staged = false;
	if (!modem->lte_virt || !modem->shared_virt || !modem->lte_calib_name)
		return 0;
	if (LTE_NV_STAGE_OFFSET + LTE_NV_STAGE_SIZE > modem->lte.region)
		return -EINVAL;

	ret = request_firmware_direct(&fw, modem->lte_calib_name, modem->dev);
	if (ret) {
		dev_warn(modem->dev, "LTE calib %s unavailable: %d\n",
			 modem->lte_calib_name, ret);
		return 0;
	}
	if (fw->size < 12 || memcmp(fw->data, LTEC_MAGIC, 4) != 0) {
		dev_warn(modem->dev, "LTE calib %s has bad header\n",
			 modem->lte_calib_name);
		release_firmware(fw);
		return 0;
	}

	for (i = 0; i < 55; i++) {
		rec = NULL;
		rec_len = 0;
		if (!ltec_find_record(fw, lte_nv_stage_ids[i], &rec,
				      &rec_len)) {
			dev_warn(modem->dev,
				 "LTE calib missing NV 0x%04x at stage %d\n",
				 lte_nv_stage_ids[i], i);
			release_firmware(fw);
			return 0;
		}
		memcpy_toio(modem->lte_virt + LTE_NV_STAGE_OFFSET + off, rec,
			    rec_len);
		off += rec_len;
	}
	if (off != LTE_NV_STAGE_SIZE)
		dev_warn(modem->dev, "LTE NV staged %zu bytes (expected %u)\n",
			 off, LTE_NV_STAGE_SIZE);

	/* FUN_811c2414: +e00 = NV9_u32 (logical 9, physical 0x09c3) with its
	 * runtime-derived high half OR'ed in. NV9 is 4 bytes in saved data.
	 */
	rec = NULL;
	rec_len = 0;
	if (ltec_find_record(fw, 0x09c3, &rec, &rec_len) && rec_len >= 4)
		nv9 = get_unaligned_le32(rec);
	e00 = nv9;
	if (modem->nv_e00_hi_set)
		e00 |= (modem->nv_e00_hi & 0xffffu) << 16;
	if (modem->nv_stub_set)
		e00 = modem->nv_stub; /* explicit full override */
	writel(e00, modem->shared_virt + DSP_NV_STUB_OFFSET);

	/* FUN_811c2458 tail: *(+e04) |= (*(0x8198a7d8)+0x2c & ~0xf). Runtime
	 * source unknown outside MOCOR; apply optional mask if provided.
	 */
	if (modem->nv_e04_mask_set) {
		e04 = readl(modem->shared_virt + 0xe04);
		e04 |= modem->nv_e04_mask;
		writel(e04, modem->shared_virt + 0xe04);
	}

	modem->lte_nv_staged = true;
	dev_info(modem->dev,
		 "LTE NV staged %zu bytes from %s; nv9=0x%08x e00=0x%08x e04=0x%08x\n",
		 off, modem->lte_calib_name, nv9,
		 readl(modem->shared_virt + DSP_NV_STUB_OFFSET),
		 readl(modem->shared_virt + 0xe04));
	release_firmware(fw);
	return 0;
}

/* Reproduce the AON/PMU sequence from the CP LTE ADX DSP init. */
/* The CP only asks the DSP for a ready reply, and polls the shared magic,
 * when re-initialising an already-started DSP (FUN_81146c3a else branch).
 * The cold-start branch performs the register sequence below and returns
 * without touching the mailbox or the magic, so keep that handshake opt-in.
 */
static int dsp_handshake(struct ums9117_modem *modem)
{
	int ret, i;

	if (!modem->shared_virt)
		return -ENODEV;
	if (!modem->dsp_chan) {
		dev_warn(modem->dev,
			 "no DSP mailbox channel; skipping start command\n");
	} else {
		u32 msg[2] = { 0, 0 };

		ret = mbox_send_message(modem->dsp_chan, msg);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < DSP_READY_POLLS; i++) {
		if (dsp_ready(modem))
			return 0;
		mdelay(DSP_READY_DELAY_MS);
	}
	return -ETIMEDOUT;
}

static int dsp_power_on(struct ums9117_modem *modem)
{
	int ret;

	if (!modem->gsm_loaded || !modem->lte_loaded)
		return -EINVAL;
	if (!modem->aon_apb || !modem->pmu_apb || !modem->shared_virt)
		return -ENODEV;

	/* FUN_81146c24: clear DSP start bit, enable DSP clock. */
	ret = regmap_update_bits(modem->aon_apb, AON_DSP_CTRL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_CLK_EB, BIT(16), BIT(16));
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

	/* Stage LTE calibration/NV buffer and write shared-page side effects
	 * (FUN_811c2458/2414) before releasing the DSP. Falls back to an
	 * explicit nv_stub if no calibration container is available.
	 */
	ret = dsp_stage_lte_calibration(modem);
	if (ret)
		return ret;
	if (!modem->lte_nv_staged && modem->nv_stub_set) {
		writel(modem->nv_stub, modem->shared_virt + DSP_NV_STUB_OFFSET);
		dev_info(modem->dev, "DSP NV stub = 0x%08x\n", modem->nv_stub);
	}

	/* Tail of FUN_81146dd8: interrupt/vector handshake. */
	ret = regmap_update_bits(modem->aon_apb, AON_MISC_CFG, 0x9620, 0x9620);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_DSP_CTRL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(modem->aon_apb, AON_CLK_EB, BIT(16), BIT(16));
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

	/* Stock cold-start sets DSP-active flags after releasing the vector. */
	if (modem->dsp_chan)
		dev_info(modem->dev, "DSP mailbox channel bound (target 3)\n");

	{
		u32 aon_ctrl = 0, aon_vec = 0, pmu_2044 = 0, pmu_20cc = 0, pmu_2034 = 0;
		u32 e00 = readl(modem->shared_virt + DSP_NV_STUB_OFFSET);
		u32 e04 = readl(modem->shared_virt + 0xe04);
		u32 e08 = readl(modem->shared_virt + 0xe08);
		u32 e0c = readl(modem->shared_virt + 0xe0c);
		u32 e10 = readl(modem->shared_virt + 0xe10);
		u32 e14 = readl(modem->shared_virt + DSP_READY_OFFSET);

		regmap_read(modem->aon_apb, AON_DSP_CTRL, &aon_ctrl);
		regmap_read(modem->aon_apb, AON_DSP_VEC, &aon_vec);
		regmap_read(modem->pmu_apb, PMU_DSP_CLK_EB0, &pmu_2044);
		regmap_read(modem->pmu_apb, PMU_DSP_CFG, &pmu_20cc);
		regmap_read(modem->pmu_apb, PMU_DSP_CLK_EB1, &pmu_2034);
		dev_info(
			modem->dev,
			"DSP power-on done; ready=%d magic=0x%08x e00=0x%08x e04=0x%08x nv_staged=%d\n",
			e14 == DSP_READY_MAGIC, e14, e00, e04,
			modem->lte_nv_staged);
		dev_info(
			modem->dev,
			"DSP shared: e08=0x%08x e0c=0x%08x e10=0x%08x e14=0x%08x\n",
			e08, e0c, e10, e14);
		dev_info(
			modem->dev,
			"DSP power-on regs: aon[24c]=0x%08x aon[248]=0x%08x pmu[2044]=0x%08x pmu[20cc]=0x%08x pmu[2034]=0x%08x\n",
			aon_ctrl, aon_vec, pmu_2044, pmu_20cc, pmu_2034);
	}
	/* Cold start is complete here: the CP does not wait for a ready magic
	 * on the first boot path. Use the "handshake" attribute for the re-init
	 * mailbox exchange if that reply is needed.
	 */
	return 0;
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

/* The stock word combines the NV lookup result in the low half with flags in
 * the high half. Until the runtime NV table is reconstructed, userspace owns
 * the final 32-bit value; show reads back what is actually in shared memory.
 */
static ssize_t nv_stub_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	if (!modem->shared_virt)
		return -ENODEV;
	return sysfs_emit(buf, "0x%08x\n",
			  readl(modem->shared_virt + DSP_NV_STUB_OFFSET));
}

static ssize_t nv_stub_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	u32 value;
	int ret;

	if (!modem->shared_virt)
		return -ENODEV;
	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	modem->nv_stub = value;
	modem->nv_stub_set = true;
	writel(value, modem->shared_virt + DSP_NV_STUB_OFFSET);
	return count;
}

static ssize_t shared_word_show(struct device *dev, u32 offset, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	if (!modem->shared_virt)
		return -ENODEV;
	return sysfs_emit(buf, "0x%08x\n", readl(modem->shared_virt + offset));
}

static ssize_t shared_word_store(struct device *dev, u32 offset,
				 const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	u32 value;
	int ret;

	if (!modem->shared_virt)
		return -ENODEV;
	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	writel(value, modem->shared_virt + offset);
	return count;
}

#define SHARED_ATTR(name, offset)                                            \
	static ssize_t name##_show(struct device *dev,                       \
				   struct device_attribute *attr, char *buf)  \
	{                                                                     \
		return shared_word_show(dev, offset, buf);                    \
	}                                                                     \
	static ssize_t name##_store(struct device *dev,                      \
				    struct device_attribute *attr,            \
				    const char *buf, size_t count)            \
	{                                                                     \
		return shared_word_store(dev, offset, buf, count);            \
	}                                                                     \
	static DEVICE_ATTR_RW(name)

SHARED_ATTR(shared_e04, 0xe04);
SHARED_ATTR(shared_e08, 0xe08);
SHARED_ATTR(shared_e0c, 0xe0c);
SHARED_ATTR(shared_e10, 0xe10);
SHARED_ATTR(shared_e18, 0xe18);
SHARED_ATTR(shared_e1c, 0xe1c);
SHARED_ATTR(shared_e20, 0xe20);
SHARED_ATTR(shared_e24, 0xe24);

static ssize_t mbox_rx_count_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", modem->mbox_rx_count);
}

static ssize_t mbox_rx_last_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x 0x%08x\n", modem->mbox_rx_last[0],
			  modem->mbox_rx_last[1]);
}

static DEVICE_ATTR_RO(mbox_rx_count);
static DEVICE_ATTR_RO(mbox_rx_last);

static ssize_t nv_e00_hi_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%04x\n", modem->nv_e00_hi & 0xffff);
}
static ssize_t nv_e00_hi_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	u32 v;
	int ret = kstrtou32(buf, 0, &v);
	if (ret)
		return ret;
	modem->nv_e00_hi = v & 0xffff;
	modem->nv_e00_hi_set = true;
	return count;
}
static DEVICE_ATTR_RW(nv_e00_hi);

static ssize_t nv_e04_mask_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%08x\n", modem->nv_e04_mask);
}
static ssize_t nv_e04_mask_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	u32 v;
	int ret = kstrtou32(buf, 0, &v);
	if (ret)
		return ret;
	modem->nv_e04_mask = v;
	modem->nv_e04_mask_set = true;
	return count;
}
static DEVICE_ATTR_RW(nv_e04_mask);

static ssize_t lte_nv_staged_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", modem->lte_nv_staged);
}
static DEVICE_ATTR_RO(lte_nv_staged);

static ssize_t lte_calib_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n",
			  modem->lte_calib_name ? modem->lte_calib_name : "");
}
static ssize_t lte_calib_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	size_t len = count;
	char *name;

	if (len && buf[len - 1] == '\n')
		len--;
	name = devm_kstrndup(dev, buf, len, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	modem->lte_calib_name = name;
	modem->lte_nv_staged = false;
	return count;
}
static DEVICE_ATTR_RW(lte_calib);

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

static ssize_t handshake_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	int ret;

	if (!count || buf[0] != '1')
		return count;
	ret = dsp_handshake(modem);
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

/*
 * The loader does not derive cellular status from the DSP. A Linux NAS/AT
 * producer owns these two attributes; until it publishes a measurement, signal
 * is -1 and the operator name is empty.
 */
static ssize_t signal_level_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", modem->signal_level);
}

static ssize_t signal_level_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	int level;
	int ret;

	ret = kstrtoint(buf, 10, &level);
	if (ret)
		return ret;
	if (level < -1 || level > 5)
		return -EINVAL;
	modem->signal_level = level;
	return count;
}

static bool operator_name_valid(const char *name, size_t length)
{
	if (length >= sizeof(((struct ums9117_modem *)0)->operator_name))
		return false;
	for (size_t i = 0; i < length; ++i) {
		unsigned char byte = name[i];

		if (byte < 0x20 || byte == 0x7f)
			return false;
	}
	return true;
}

static ssize_t operator_name_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", modem->operator_name);
}

static ssize_t operator_name_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ums9117_modem *modem = dev_get_drvdata(dev);
	size_t length = count;

	/* sysfs show appends a newline; accept it as a terminator only. */
	if (length && buf[length - 1] == '\n')
		--length;
	if (!operator_name_valid(buf, length))
		return -EINVAL;
	memcpy(modem->operator_name, buf, length);
	modem->operator_name[length] = '\0';
	return count;
}

static DEVICE_ATTR_RO(loaded);
static DEVICE_ATTR_RO(dsp_ready);
static DEVICE_ATTR_RW(nv_stub);
static DEVICE_ATTR_WO(load);
static DEVICE_ATTR_WO(load_gsm);
static DEVICE_ATTR_WO(load_lte);
static DEVICE_ATTR_WO(power_on);
static DEVICE_ATTR_WO(handshake);
static DEVICE_ATTR_RW(signal_level);
static DEVICE_ATTR_RW(operator_name);

static struct attribute *status_attrs[] = {
	&dev_attr_signal_level.attr,
	&dev_attr_operator_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(status);

static void destroy_status(struct ums9117_modem *modem)
{
	if (modem->status_dev) {
		device_destroy(modem->status_class, 0);
		modem->status_dev = NULL;
	}
	if (!IS_ERR_OR_NULL(modem->status_class)) {
		class_destroy(modem->status_class);
		modem->status_class = NULL;
	}
}

static struct attribute *modem_attrs[] = {
	&dev_attr_loaded.attr,
	&dev_attr_dsp_ready.attr,
	&dev_attr_nv_stub.attr,
	&dev_attr_nv_e00_hi.attr,
	&dev_attr_nv_e04_mask.attr,
	&dev_attr_lte_nv_staged.attr,
	&dev_attr_lte_calib.attr,
	&dev_attr_shared_e04.attr,
	&dev_attr_shared_e08.attr,
	&dev_attr_shared_e0c.attr,
	&dev_attr_shared_e10.attr,
	&dev_attr_shared_e18.attr,
	&dev_attr_shared_e1c.attr,
	&dev_attr_shared_e20.attr,
	&dev_attr_shared_e24.attr,
	&dev_attr_mbox_rx_count.attr,
	&dev_attr_mbox_rx_last.attr,
	&dev_attr_load.attr,
	&dev_attr_load_gsm.attr,
	&dev_attr_load_lte.attr,
	&dev_attr_power_on.attr,
	&dev_attr_handshake.attr,
	NULL,
};
ATTRIBUTE_GROUPS(modem);

static int ums9117_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_modem *modem;
	struct resource *res;
	int ret;

	modem = devm_kzalloc(dev, sizeof(*modem), GFP_KERNEL);
	if (!modem)
		return -ENOMEM;
	modem->dev = dev;
	modem->signal_level = -1;

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
	 * excluded from System RAM (the DTB memory node must start above them).
	 */
	if (region_intersects(modem->gsm.phys, modem->gsm.region,
			      IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT ||
	    region_intersects(modem->lte.phys, modem->lte.region,
			      IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return dev_err_probe(dev, -EBUSY,
				     "DSP regions overlap System RAM\n");

	/* The DSP regions are SRAM shared with another processor, not ordinary
	 * kernel memory. Map them uncached so memcpy_toio() writes reach the SRAM
	 * (and readl() of the ready magic is not served from a stale cache line).
	 * devm_ioremap_resource() cannot be used because the reserved-memory nodes
	 * already own these ranges, but the ranges are now outside System RAM so
	 * plain devm_ioremap() succeeds.
	 */
	modem->gsm_virt = devm_ioremap(dev, modem->gsm.phys, modem->gsm.region);
	if (!modem->gsm_virt)
		return -ENOMEM;
	modem->lte_virt = devm_ioremap(dev, modem->lte.phys, modem->lte.region);
	if (!modem->lte_virt)
		return -ENOMEM;

	/* LTE DSP shared-memory page holds the ready magic at +0xe14. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsp-shared");
	if (res) {
		modem->shared_virt =
			devm_ioremap(dev, res->start, resource_size(res));
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

	/* CP LTE ADX registers target 3 (MBOX_DSP_CMD) for the ready exchange. */
	modem->mbox_client.dev = dev;
	modem->mbox_client.rx_callback = ums9117_modem_rx;
	modem->dsp_chan =
		mbox_request_channel_byname(&modem->mbox_client, "dsp");
	if (IS_ERR(modem->dsp_chan)) {
		ret = PTR_ERR(modem->dsp_chan);
		dev_warn(dev, "DSP mailbox channel unavailable: %d\n", ret);
		modem->dsp_chan = NULL;
	}

	{
		u32 header;

		modem->gsm.header = 0x80; /* V1GGE_Bugid header */
		modem->lte.header = 0; /* raw CEVA image */
		if (!of_property_read_u32(dev->of_node, "fplinux,gsm-header",
					  &header))
			modem->gsm.header = header;
		if (!of_property_read_u32(dev->of_node, "fplinux,lte-header",
					  &header))
			modem->lte.header = header;
	}

	if (of_property_read_string(dev->of_node, "firmware-name-gsm",
				    &modem->gsm.name))
		modem->gsm.name = "ums9117/gsm_dsp.bin";
	if (of_property_read_string(dev->of_node, "firmware-name-lte",
				    &modem->lte.name))
		modem->lte.name = "ums9117/lte_dsp.bin";
	if (of_property_read_string(dev->of_node, "firmware-name-lte-calib",
				    &modem->lte_calib_name))
		modem->lte_calib_name = "fplinux/inoi244-lte-calibration-downloaded.bin";

	/* Optional pre-seeded value for the NV calibration word. The sysfs
	 * attribute can still override it before "power_on" is written.
	 */
	if (!of_property_read_u32(dev->of_node, "fplinux,dsp-nv-stub",
				  &modem->nv_stub))
		modem->nv_stub_set = true;
	if (!of_property_read_u32(dev->of_node, "fplinux,dsp-nv-e00-hi",
				  &modem->nv_e00_hi))
		modem->nv_e00_hi_set = true;
	if (!of_property_read_u32(dev->of_node, "fplinux,dsp-nv-e04-mask",
				  &modem->nv_e04_mask))
		modem->nv_e04_mask_set = true;

	platform_set_drvdata(pdev, modem);

	/* A stable class device lets userspace publish cellular status without
	 * depending on the platform device's numbered path.
	 */
	modem->status_class = class_create("fplinux_modem");
	if (IS_ERR(modem->status_class)) {
		ret = PTR_ERR(modem->status_class);
		modem->status_class = NULL;
		goto out_mbox;
	}
	modem->status_dev = device_create_with_groups(
		modem->status_class, dev, 0, modem, status_groups, "modem");
	if (IS_ERR(modem->status_dev)) {
		ret = PTR_ERR(modem->status_dev);
		modem->status_dev = NULL;
		destroy_status(modem);
		goto out_mbox;
	}
	ret = sysfs_create_groups(&dev->kobj, modem_groups);
	if (ret)
		goto out_status;
	return 0;

out_status:
	destroy_status(modem);
out_mbox:
	if (modem->dsp_chan)
		mbox_free_channel(modem->dsp_chan);
	return ret;
}

static void ums9117_modem_remove(struct platform_device *pdev)
{
	struct ums9117_modem *modem = platform_get_drvdata(pdev);

	sysfs_remove_groups(&pdev->dev.kobj, modem_groups);
	destroy_status(modem);
	if (modem->dsp_chan)
		mbox_free_channel(modem->dsp_chan);
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
