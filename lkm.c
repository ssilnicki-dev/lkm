#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/printk.h>

static long kt_period_param_us = USEC_PER_SEC;
module_param(kt_period_param_us, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(kt_period_param_us, "Timer period, us");

static ktime_t kt_period;
static struct hrtimer kt;

static enum hrtimer_restart kt_callback(struct hrtimer *timer) {
  hrtimer_forward_now(timer, kt_period);
  return HRTIMER_RESTART;
}

static inline void __lkm_init_timer(long period_us) {
  kt_period = ktime_set(kt_period_param_us / USEC_PER_SEC,
                        (kt_period_param_us % USEC_PER_SEC) * NSEC_PER_USEC);
  hrtimer_init(&kt, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
  kt.function = kt_callback;
  hrtimer_start(&kt, kt_period, HRTIMER_MODE_REL_HARD);
}

static int __init lkm_init(void) {
  pr_info("lkm loaded\n");

  __lkm_init_timer(kt_period_param_us);

  return 0;
}

static void __exit lkm_exit(void) {
  hrtimer_cancel(&kt);

  pr_info("lkm unloaded\n");
}

module_init(lkm_init);
module_exit(lkm_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ssilnicki");
MODULE_DESCRIPTION("Linux Kernel Module");
