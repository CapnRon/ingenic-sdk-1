#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/bug.h>
#include "tx-isp-module.h"

extern int tx_isp_sinfo_init(void);
extern void tx_isp_sinfo_exit(void);

static int __init tx_isp_driver_init(void)
{
	int ret = tx_isp_init();
	if (!ret)
		tx_isp_sinfo_init();
	return ret;
}

static void __exit tx_isp_driver_exit(void)
{
	tx_isp_sinfo_exit();
	return tx_isp_exit();
}

module_init(tx_isp_driver_init);
module_exit(tx_isp_driver_exit);

MODULE_AUTHOR("Ingenic xhshen");
MODULE_DESCRIPTION("tx isp driver");
MODULE_LICENSE("GPL");
