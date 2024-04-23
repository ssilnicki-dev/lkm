#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define DEVICE_DATASHEET_MINIMUM_READ_US (80)
#define SYSFS_DATA_ENDPOINTS_BASE "lkm_data"
#define SYSFS_DATA_STAT_ENDPOINT stat
#define SYSFS_CURRENT_DATA_ENDPOINT current_data
#define HIGH_PRIORITY_QUEUE_NAME "lkm_high_pri_queue"
#define DEVICE_DATA_WORDS (16)

typedef u32 device_word_t;
enum device_read_work_stage_t { IN_PROGRESS, DONE };
struct device_read_work_t {
	struct work_struct work;
	atomic_long_t work_stage;
};

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

static int use_high_pri_queue = 0;
module_param(use_high_pri_queue, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(use_high_pri_queue,
		 "Use dedicated high priority queue, [1|0]");

static ktime_t kt_period;
static struct hrtimer kt;

struct device_read_work_t device_read_work;

static atomic_long_t device_read_omissions;
static atomic_long_t device_data_reads;

static struct kobject *sysfs_kobject;

static ssize_t sysfs_output_device_statistics(struct kobject *,
					      struct kobj_attribute *, char *);
static struct kobj_attribute sysfs_stat_attribute = __ATTR(
	SYSFS_DATA_STAT_ENDPOINT, 0444, sysfs_output_device_statistics, NULL);

static ssize_t sysfs_output_current_data(struct kobject *,
					 struct kobj_attribute *, char *);
static struct kobj_attribute sysfs_current_data_attribute = __ATTR(
	SYSFS_CURRENT_DATA_ENDPOINT, 0444, sysfs_output_current_data, NULL);

static struct workqueue_struct *high_pri_queue = NULL;

static device_word_t published_device_data[DEVICE_DATA_WORDS];
static seqlock_t data_seqlock;
static atomic_long_t retries;
static atomic_long_t data_reads;
static atomic_long_t errors;

static ssize_t sysfs_output_device_statistics(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sprintf(buf,
		       "device reads: %ld, skips: %ld, data reads: %ld, "
		       "retries: %ld, errors: %ld\n",
		       atomic_long_read(&device_data_reads),
		       atomic_long_read(&device_read_omissions),
		       atomic_long_read(&data_reads),
		       atomic_long_read(&retries), atomic_long_read(&errors));
}

static ssize_t sysfs_output_current_data(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	unsigned sequence;
	long copy_attempts;
	device_word_t *current_data_buf;
	device_word_t calculated_magic_value;
	int i;

	copy_attempts = 0;
	current_data_buf =
		kmalloc(DEVICE_DATA_WORDS * sizeof(device_word_t), GFP_KERNEL);
	if (!current_data_buf) {
		atomic_long_inc(&errors);
		return 0;
	}

	do {
		copy_attempts++;
		sequence = read_seqbegin(&data_seqlock);
		memcpy(current_data_buf, published_device_data,
		       DEVICE_DATA_WORDS * sizeof(device_word_t));
	} while (read_seqretry(&data_seqlock, sequence));
	atomic_long_and(copy_attempts - 1, &retries);
	atomic_long_inc(&data_reads);

	calculated_magic_value = 0;
	for (i = 0; i < DEVICE_DATA_WORDS; i = i + 1) {
		calculated_magic_value += current_data_buf[i];
	}
	kfree(current_data_buf);

	return sprintf(buf, "device magic value: %016X\n",
		       calculated_magic_value);
}

static void hardware_device_blocking_read(void)
{
	static device_word_t runtime_data[DEVICE_DATA_WORDS];
	unsigned long usleep_us = DEVICE_DATASHEET_MINIMUM_READ_US;
	int i;

	for (i = 0; i < (sizeof(runtime_data) / sizeof(*runtime_data));
	     i = i + 1) {
		runtime_data[i] = get_random_u32();
	}
	atomic_long_inc(&device_data_reads);

	if (likely(device_read_jitter_range_param_us > 0)) {
		usleep_us +=
			get_random_u32() % device_read_jitter_range_param_us;
	}
	usleep_range(usleep_us - 1, usleep_us + 1);

	write_seqlock(&data_seqlock);
	memcpy(published_device_data, runtime_data,
	       sizeof(device_word_t) * DEVICE_DATA_WORDS);
	write_sequnlock(&data_seqlock);
}

static void read_data_from_device(struct work_struct *work)
{
	struct device_read_work_t *wrapper;
	hardware_device_blocking_read();
	wrapper = container_of(work, struct device_read_work_t, work);
	atomic_long_set(&wrapper->work_stage, DONE);
}

static enum hrtimer_restart kt_callback(struct hrtimer *timer)
{
	if (likely(kt_period_jitter_range_param_us > 0)) {
		u32 jitter_lag_us =
			get_random_u32() % kt_period_jitter_range_param_us;
		u64 period_us = kt_period_param_us + jitter_lag_us;
		kt_period =
			ktime_set(period_us / USEC_PER_SEC,
				  (period_us % USEC_PER_SEC) * NSEC_PER_USEC);
	}
	if (likely(atomic_long_read(&device_read_work.work_stage) == DONE)) {
		atomic_long_set(&device_read_work.work_stage, IN_PROGRESS);
		if (high_pri_queue)
			queue_work(high_pri_queue, &device_read_work.work);
		else
			schedule_work(&device_read_work.work);
	} else
		atomic_long_inc(&device_read_omissions);

	hrtimer_forward_now(timer, kt_period);
	return HRTIMER_RESTART;
}

static inline void init_timer(long period_us)
{
	kt_period =
		ktime_set(kt_period_param_us / USEC_PER_SEC,
			  (kt_period_param_us % USEC_PER_SEC) * NSEC_PER_USEC);
	hrtimer_init(&kt, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	kt.function = kt_callback;
	hrtimer_start(&kt, kt_period, HRTIMER_MODE_REL_HARD);
}

static inline int create_sysfs_files(void)
{
	int error;
	struct attribute *failed_attribute = NULL;

	error = sysfs_create_file(sysfs_kobject, &sysfs_stat_attribute.attr);
	if (error) {
		failed_attribute = &sysfs_stat_attribute.attr;
		goto err;
	}
	error = sysfs_create_file(sysfs_kobject,
				  &sysfs_current_data_attribute.attr);
	if (error) {
		failed_attribute = &sysfs_current_data_attribute.attr;
		goto err;
	}
	return 0;

err:
	if (failed_attribute)
		pr_err("lkm could not create sysfs file '%s'\n",
		       failed_attribute->name);
	return error;
}

static int __init lkm_init(void)
{
	int error;

	sysfs_kobject =
		kobject_create_and_add(SYSFS_DATA_ENDPOINTS_BASE, kernel_kobj);
	if (!sysfs_kobject) {
		error = -ENOMEM;
		pr_err("lkm failed to allocate sysfs object\n");
		goto err_sysfs_kobj;
	}
	error = create_sysfs_files();
	if (error)
		goto err_sysfs_files;

	if (use_high_pri_queue) {
		high_pri_queue = alloc_workqueue(HIGH_PRIORITY_QUEUE_NAME,
						 WQ_UNBOUND | WQ_HIGHPRI, 0);
		if (!high_pri_queue) {
			pr_err("lkm failed to allocate high pri queue\n");
			error = -ENOMEM;
			goto err_sysfs_files;
		}
		pr_info("lkm using dedicated high priority queue\n");
	}

	seqlock_init(&data_seqlock);
	atomic_long_set(&retries, 0);
	atomic_long_set(&data_reads, 0);
	atomic_long_set(&errors, 0);

	atomic_long_set(&device_read_omissions, 0);
	atomic_long_set(&device_data_reads, 0);
	atomic_long_set(&device_read_work.work_stage, DONE);
	INIT_WORK(&device_read_work.work, read_data_from_device);

	init_timer(kt_period_param_us);

	pr_info("lkm loaded\n");

	return 0;

err_sysfs_files:
	kobject_put(sysfs_kobject);

err_sysfs_kobj:
	return error;
}

static void __exit lkm_exit(void)
{
	hrtimer_cancel(&kt);
	cancel_work(&device_read_work.work);
	kobject_put(sysfs_kobject);

	if (high_pri_queue) {
		flush_workqueue(high_pri_queue);
		destroy_workqueue(high_pri_queue);
	}

	pr_info("lkm unloaded\n");
}

module_init(lkm_init);
module_exit(lkm_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ssilnicki");
MODULE_DESCRIPTION("Linux Kernel Module");
