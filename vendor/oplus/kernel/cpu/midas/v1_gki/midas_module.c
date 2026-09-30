/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2019-2020 Oplus. All rights reserved.
 */
#define pr_fmt(fmt) KBUILD_MODNAME " %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/module.h>
#include "midas_dev.h"

#include <linux/errno.h>

#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define MEM_1GB (1 << (30 - PAGE_SHIFT))
#define MEM_2GB (2 << (30 - PAGE_SHIFT))
#define MEM_3GB (3 << (30 - PAGE_SHIFT))
#define MEM_4GB (4 << (30 - PAGE_SHIFT))
#define MEM_6GB (6 << (30 - PAGE_SHIFT))
#define MEM_8GB (8 << (30 - PAGE_SHIFT))
#define MEM_12GB (12 << (30 - PAGE_SHIFT))
#define MEM_16GB (16 << (30 - PAGE_SHIFT))
#define MEM_24GB (24 << (30 - PAGE_SHIFT))

enum ram_val {
	RAM_1GB = 1, /*Ram Size*/
    RAM_2GB = 2,
    RAM_3GB = 3,
    RAM_4GB = 4,
    RAM_6GB = 6,
    RAM_8GB = 8,
    RAM_12GB = 12,
    RAM_16GB = 16,
    RAM_24GB = 24,
};

static int read_ram_gb(void)
{
	int ram = -1;
	unsigned long total = totalram_pages();

	if (kasan_enabled()) {
		pr_info("kasan enabled, add 1/8 to total\n");
		total = total / 7 * 8;
	}

	if (total < MEM_1GB)
		ram = RAM_1GB;
	else if (total < MEM_2GB)
		ram = RAM_2GB;
	else if (total < MEM_3GB)
		ram = RAM_3GB;
	else if (total < MEM_4GB)
		ram = RAM_4GB;
	else if (total < MEM_6GB)
		ram = RAM_6GB;
	else if (total < MEM_8GB)
		ram = RAM_8GB;
	else if (total < MEM_12GB)
		ram = RAM_12GB;
	else if (total < MEM_16GB)
		ram = RAM_16GB;
	else if (total < MEM_24GB)
		ram = RAM_24GB;
	pr_err("get RAM %d\n", ram);
	return ram;
}

static int is_feature_disable(void)
{
    struct device_node *node;
    int ret = 0;

    node = of_find_compatible_node(NULL, NULL, "oplus,bsp_midas");
    if (node == NULL) {
        pr_err("Can't find oplus,bsp_midas node\n");
        goto out;
    }
    ret = of_property_read_bool(node, "feature-disable");
    pr_err("feature-disable is %d\n", ret);
    of_node_put(node);

out:
    return ret;
}

extern int __init binder_stats_dev_init(void);
extern void __exit binder_stats_dev_exit(void);

static int __init midas_module_init(void)
{
	int ram_gb = read_ram_gb();
	pr_err("RAM size: %d\n", ram_gb);
	if (is_feature_disable() && ram_gb <= RAM_4GB) {
        pr_err("Midas: Feature disabled AND RAM <= %dGB\n", RAM_4GB);
        return -1;
    }
	midas_dev_init();
	binder_stats_dev_init();

	return 0;
}

static void __exit midas_module_exit(void)
{
	midas_dev_exit();
	binder_stats_dev_exit();
}

module_init(midas_module_init);
module_exit(midas_module_exit);
MODULE_LICENSE("GPL v2");
