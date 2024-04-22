#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

static long kt_period_param_us = USEC_PER_SEC;
module_param(kt_period_param_us, long, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(kt_period_param_us, "Timer period, us");
static int kt_period_jitter_range_param_us = 100;
module_param(kt_period_jitter_range_param_us, int,
             S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(kt_period_jitter_range_us, "Timer jitter range, us");
static int device_read_jitter_range_param_us = 25;
module_param(device_read_jitter_range_param_us, int,
             S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(device_read_jitter_range_param_us, "Device read jitter, us");
#define DEVICE_DATASHEET_MINIMUM_READ_US (80)

static ktime_t kt_period;
static struct hrtimer kt;

enum device_read_work_stage_t { IN_PROGRESS, DONE };
struct device_read_work_t {
  struct work_struct work;
  atomic_long_t work_stage;
};

struct device_read_work_t device_read_work;

static void hardware_device_blocking_read(void) {
  static u32 runtime_data[16];
  unsigned long usleep_us = DEVICE_DATASHEET_MINIMUM_READ_US;
  int i;

  for (i = 0; i < (sizeof(runtime_data) / sizeof(*runtime_data)); i = i + 1) {
    runtime_data[i] = get_random_u32();
  }

  if (likely(device_read_jitter_range_param_us > 0)) {
    usleep_us += get_random_u32() % device_read_jitter_range_param_us;
  }
  usleep_range(usleep_us - 1, usleep_us + 1);
}

static void read_data_from_device(struct work_struct *work) {
  struct device_read_work_t *wrapper;
  hardware_device_blocking_read();
  wrapper = container_of(work, struct device_read_work_t, work);
  atomic_long_set(&wrapper->work_stage, DONE);
}

static enum hrtimer_restart kt_callback(struct hrtimer *timer) {
  if (likely(kt_period_jitter_range_param_us > 0)) {
    u32 jitter_lag_us = get_random_u32() % kt_period_jitter_range_param_us;
    u64 period_us = kt_period_param_us + jitter_lag_us;
    kt_period = ktime_set(period_us / USEC_PER_SEC,
                          (period_us % USEC_PER_SEC) * NSEC_PER_USEC);
  }
  if (likely(atomic_long_read(&device_read_work.work_stage) == DONE)) {
    atomic_long_set(&device_read_work.work_stage, IN_PROGRESS);
    schedule_work(&device_read_work.work);
  }
  hrtimer_forward_now(timer, kt_period);
  return HRTIMER_RESTART;
}

static inline void init_timer(long period_us) {
  kt_period = ktime_set(kt_period_param_us / USEC_PER_SEC,
                        (kt_period_param_us % USEC_PER_SEC) * NSEC_PER_USEC);
  hrtimer_init(&kt, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
  kt.function = kt_callback;
  hrtimer_start(&kt, kt_period, HRTIMER_MODE_REL_HARD);
}

static int __init lkm_init(void) {
  pr_info("lkm loaded\n");

  atomic_long_set(&device_read_work.work_stage, DONE);
  INIT_WORK(&device_read_work.work, read_data_from_device);

  init_timer(kt_period_param_us);

  return 0;
}

static void __exit lkm_exit(void) {
  hrtimer_cancel(&kt);
  cancel_work(&device_read_work.work);

  pr_info("lkm unloaded\n");
}

module_init(lkm_init);
module_exit(lkm_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ssilnicki");
MODULE_DESCRIPTION("Linux Kernel Module");
