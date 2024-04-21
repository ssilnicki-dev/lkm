#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static int __init lkm_init(void) {
  pr_info("lkm loaded\n");
  return 0;
}

static void __exit lkm_exit(void) { pr_info("lkm unloaded\n"); }

module_init(lkm_init);
module_exit(lkm_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ssilnicki");
MODULE_DESCRIPTION("Linux Kernel Module");
