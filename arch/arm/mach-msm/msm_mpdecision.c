/*
 * arch/arm/mach-msm/msm_mpdecision.c
 *
 * hotplug cores of MSM multicore cpus based on demand and suspend
 * 
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * major revision:
 * July 2013, https://github.com/mrg666/android_kernel_shooter
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU 
 * General Public License included with the Linux kernel or available 
 * at www.gnu.org/licenses
 */

#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <asm-generic/cputime.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include "acpuclock.h"
#include <linux/rq_stats.h>

#define DEFAULT_RQ_POLL_JIFFIES		1
#define DEFAULT_DEF_TIMER_JIFFIES	5

#define MPDEC_TAG			"[MPDEC]: "
#define MSM_MPDEC_STARTDELAY		20000
#define MSM_MPDEC_DELAY			100
#define MSM_MPDEC_PAUSE			10000

struct global_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj,
			struct attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *a, struct attribute *b,
			 const char *c, size_t count);
};

#define define_one_global_ro(_name)		\
static struct global_attr _name =		\
__ATTR(_name, 0444, show_##_name, NULL)

#define define_one_global_rw(_name)		\
static struct global_attr _name =		\
__ATTR(_name, 0644, show_##_name, store_##_name)

struct msm_mpdec_cpudata_t {
	struct mutex hotplug_mutex;
	int online;
	long long unsigned int times_cpu_hotplugged;
};
static DEFINE_PER_CPU(struct msm_mpdec_cpudata_t, msm_mpdec_cpudata);

static struct delayed_work msm_mpdec_work;
static struct workqueue_struct *msm_mpdec_workq;
static DEFINE_MUTEX(mpdec_msm_cpu_lock);

static struct msm_mpdec_tuners {
	unsigned int delay;
	unsigned int pause;
	bool scroff_single_core;
	unsigned int max_cpus;
	unsigned int min_cpus;
} msm_mpdec_tuners_ins = {
	.delay = MSM_MPDEC_DELAY,
	.pause = MSM_MPDEC_PAUSE,
	.scroff_single_core = true,
	.max_cpus = CONFIG_NR_CPUS,
	.min_cpus = 1,
};

/* limit arrays: 1_up, 2_down, 2_up, 3_down, 3_up, 4_down, ...
 * if i=nr_cpu_online, up_index=2*i-2 and down_index=2*i-3, 
 * i>1 for down and i<CONFIG_NR_CPUS for up */ 
static unsigned int load_limit[2] = {35, 5};
static unsigned int time_limit[2] = {90, 450};

bool was_paused = false;
static cputime64_t mpdec_paused_until = 0;
static cputime64_t total_time = 0;
static cputime64_t last_time;
static int enabled = 1;

unsigned int get_rq_avg(void) {
	unsigned long flags = 0;
	unsigned int rq = 0;

	spin_lock_irqsave(&rq_lock, flags);
	rq = rq_info.rq_avg;
	rq_info.rq_avg = 0;
	spin_unlock_irqrestore(&rq_lock, flags);
	return rq;
}

static void mpdec_pause(int cpu) {
	pr_info(MPDEC_TAG"CPU[%d] bypassed mpdecision! | pausing [%d]ms\n",
		cpu, msm_mpdec_tuners_ins.pause);
	mpdec_paused_until = ktime_to_ms(ktime_get()) + msm_mpdec_tuners_ins.pause;
	was_paused = true;
}

#if CONFIG_NR_CPUS > 2
static int get_slowest_cpu(void) {
	int i, cpu = 1;
	unsigned long rate, slow_rate = 999999999;

	for (i = 1; i < nr_cpu_ids; i++) {
		if (cpu_online(i)) {
			rate = acpuclk_get_rate(i);
			if (rate < slow_rate) {
				cpu = i;
				slow_rate = rate;
			}
		}
	}
	return cpu;
}
#endif

static bool mpdec_cpu_down(int cpu) {
	bool ret;

	ret = cpu_online(cpu);
	if (ret) {
		mutex_lock(&per_cpu(msm_mpdec_cpudata, cpu).hotplug_mutex);
		cpu_down(cpu);
		per_cpu(msm_mpdec_cpudata, cpu).online = false;
		pr_info(MPDEC_TAG"CPU[%d] on->off | Mask=[%d%d]\n",
			cpu, cpu_online(0), cpu_online(1));
		mutex_unlock(&per_cpu(msm_mpdec_cpudata, cpu).hotplug_mutex);
	}
	return ret;
}

static bool mpdec_cpu_up(int cpu) {
	bool ret;

	ret = !cpu_online(cpu);
	if (ret) {
		mutex_lock(&per_cpu(msm_mpdec_cpudata, cpu).hotplug_mutex);
		cpu_up(cpu);
		per_cpu(msm_mpdec_cpudata, cpu).online = true;		
		per_cpu(msm_mpdec_cpudata, cpu).times_cpu_hotplugged += 1;
		pr_info(MPDEC_TAG"CPU[%d] off->on | Mask=[%d%d]\n",
			cpu, cpu_online(0), cpu_online(1));
		mutex_unlock(&per_cpu(msm_mpdec_cpudata, cpu).hotplug_mutex);
	}
	return ret;
}

static void rq_work_fn(struct work_struct *work) {
	int64_t diff, now;

	now = ktime_to_ns(ktime_get());
	diff = now - rq_info.def_start_time;
	do_div(diff, 1000*1000);
	rq_info.def_interval = (unsigned int) diff;
	rq_info.def_timer_jiffies = msecs_to_jiffies(rq_info.def_interval);
	rq_info.def_start_time = now;
}

static void msm_mpdec_work_thread(struct work_struct *work) {
	unsigned int cpu;
	int nr_cpu_online;
	int index;
	unsigned int rq_avg;
	cputime64_t current_time;

	current_time = ktime_to_ms(ktime_get());
	total_time += (current_time - last_time);

	if (was_paused) {
		if (mpdec_paused_until >= current_time) {
			goto out;
		} else {
			for_each_possible_cpu(cpu) {
				if (cpu_online(cpu))
					per_cpu(msm_mpdec_cpudata, cpu).online = true;
				else
					per_cpu(msm_mpdec_cpudata, cpu).online = false;
			}
			was_paused = false;
			mpdec_paused_until = 0;
		}
	}

	cpu = 1;
	rq_avg = get_rq_avg();
	nr_cpu_online = num_online_cpus();
	index = 2*nr_cpu_online - 2;

	if ((nr_cpu_online < msm_mpdec_tuners_ins.max_cpus) && 
	    (rq_avg >= load_limit[index])) {
		if (total_time >= time_limit[index]) {
#if CONFIG_NR_CPUS > 2
			cpu = cpumask_next_zero(0, cpu_online_mask);
#endif
			if (per_cpu(msm_mpdec_cpudata, cpu).online == false) {
				if (mpdec_cpu_up(cpu))
					total_time = 0;
				else
					mpdec_pause(cpu);
			}
		}
	} else if ((nr_cpu_online > msm_mpdec_tuners_ins.min_cpus) &&
		   (rq_avg <= load_limit[index-1])) {
		if (total_time >= time_limit[index-1]) {
#if CONFIG_NR_CPUS > 2
			cpu = get_slowest_cpu();
#endif
			if (per_cpu(msm_mpdec_cpudata, cpu).online == true) {
				if (mpdec_cpu_down(cpu))
					total_time = 0;
				else
					mpdec_pause(cpu);
			}
		}
	}
out:
	last_time = current_time;
	if (enabled)
		queue_delayed_work(msm_mpdec_workq, &msm_mpdec_work,
				   msecs_to_jiffies(msm_mpdec_tuners_ins.delay));
	return;
}

static void msm_mpdec_early_suspend(struct early_suspend *h) {
	int cpu = 1;

	/* unplug cpu cores */
	if (msm_mpdec_tuners_ins.scroff_single_core)
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			mpdec_cpu_down(cpu);

	/* suspend main work thread */
	if (enabled)
		cancel_delayed_work_sync(&msm_mpdec_work);

	pr_info(MPDEC_TAG"msm_mpdecision suspended.\n");
}

static void msm_mpdec_late_resume(struct early_suspend *h) {
	int cpu = 1;

	/* hotplug cpu cores */
	if (msm_mpdec_tuners_ins.scroff_single_core)
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			mpdec_cpu_up(cpu);

	/* resume main work thread */
	if (enabled) {
		was_paused = true;
		queue_delayed_work(msm_mpdec_workq, &msm_mpdec_work, 
				msecs_to_jiffies(msm_mpdec_tuners_ins.delay));
	}

	pr_info(MPDEC_TAG"msm_mpdecision resumed. | Mask=[%d%d]\n",
		cpu_online(0), cpu_online(1));
}

static struct early_suspend msm_mpdec_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = msm_mpdec_early_suspend,
	.resume = msm_mpdec_late_resume,
};

static int set_enabled(const char *val, const struct kernel_param *kp) {
	int ret = 0;
	int cpu = 1;

	ret = param_set_bool(val, kp);
	if (enabled) {
		was_paused = true;
		queue_delayed_work(msm_mpdec_workq, &msm_mpdec_work,
				msecs_to_jiffies(msm_mpdec_tuners_ins.delay));
		pr_info(MPDEC_TAG"msm_mpdecision enabled\n");
	} else {
		cancel_delayed_work_sync(&msm_mpdec_work);
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			mpdec_cpu_up(cpu);
		pr_info(MPDEC_TAG"msm_mpdecision disabled\n");
	}
	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "hotplug cpu cores based on demand");

/***************************** SYSFS START *****************************/
struct kobject *msm_mpdec_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", msm_mpdec_tuners_ins.object);	\
}
show_one(delay, delay);
show_one(pause, pause);
show_one(scroff_single_core, scroff_single_core);
show_one(min_cpus, min_cpus);
show_one(max_cpus, max_cpus);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	msm_mpdec_tuners_ins.object = input;				\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(delay, delay);
store_one(pause, pause);
store_one(scroff_single_core, scroff_single_core);
store_one(max_cpus, max_cpus);
store_one(min_cpus, min_cpus);

#define show_one_tlim(file_name, arraypos)				\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", time_limit[arraypos]);		\
}
show_one_tlim(time_limit_0, 0);
show_one_tlim(time_limit_1, 1);

#define store_one_tlim(file_name, arraypos)				\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	time_limit[arraypos] = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one_tlim(time_limit_0, 0);
store_one_tlim(time_limit_1, 1);

#define show_one_llim(file_name, arraypos)				\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", load_limit[arraypos]);		\
}
show_one_llim(load_limit_0, 0);
show_one_llim(load_limit_1, 1);

#define store_one_llim(file_name, arraypos)				\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	load_limit[arraypos] = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one_llim(load_limit_0, 0);
store_one_llim(load_limit_1, 1);

static struct attribute *msm_mpdec_attributes[] = {
	&delay.attr,
	&pause.attr,
	&scroff_single_core.attr,
	&min_cpus.attr,
	&max_cpus.attr,
	&time_limit_0.attr,
	&time_limit_1.attr,
	&load_limit_0.attr,
	&load_limit_1.attr,
	NULL
};

static struct attribute_group msm_mpdec_attr_group = {
	.attrs = msm_mpdec_attributes,
	.name = "conf",
};

static ssize_t show_times_cpus_hotplugged(struct kobject *a, 
					struct attribute *b, char *buf) {
	ssize_t len = 0;
	int cpu = 0;

	for_each_possible_cpu(cpu) {
		len += sprintf(buf + len, "%i %llu\n", cpu, 
			per_cpu(msm_mpdec_cpudata, cpu).times_cpu_hotplugged);
	}
	return len;
}
define_one_global_ro(times_cpus_hotplugged);

static struct attribute *msm_mpdec_stats_attributes[] = {
	&times_cpus_hotplugged.attr,
	NULL
};

static struct attribute_group msm_mpdec_stats_attr_group = {
	.attrs = msm_mpdec_stats_attributes,
	.name = "stats",
};
/****************************** SYSFS END ******************************/

static int __init msm_mpdec_init(void) {
	int cpu, rc, err = 0;

	rq_wq = create_singlethread_workqueue("rq_stats");
	BUG_ON(!rq_wq);
	INIT_WORK(&rq_info.def_timer_work, rq_work_fn);
	spin_lock_init(&rq_lock);
	rq_info.rq_poll_jiffies = DEFAULT_RQ_POLL_JIFFIES;
	rq_info.def_timer_jiffies = DEFAULT_DEF_TIMER_JIFFIES;
	rq_info.def_start_time = ktime_to_ns(ktime_get());
	rq_info.rq_poll_last_jiffy = 0;
	rq_info.def_timer_last_jiffy = 0;
	rq_info.hotplug_disabled = 0;
	rq_info.init = 1;

	was_paused = true;
	last_time = ktime_to_ms(ktime_get());
	for_each_possible_cpu(cpu) {
		mutex_init(&(per_cpu(msm_mpdec_cpudata, cpu).hotplug_mutex));
		per_cpu(msm_mpdec_cpudata, cpu).online = true;
		per_cpu(msm_mpdec_cpudata, cpu).times_cpu_hotplugged = 0;
	}

	msm_mpdec_workq = alloc_workqueue("mpdec",
					WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);
	if (!msm_mpdec_workq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&msm_mpdec_work, msm_mpdec_work_thread);
	if (enabled)
		queue_delayed_work(msm_mpdec_workq, &msm_mpdec_work,
				   msecs_to_jiffies(MSM_MPDEC_STARTDELAY));

	register_early_suspend(&msm_mpdec_early_suspend_handler);

	msm_mpdec_kobject = kobject_create_and_add("msm_mpdecision", kernel_kobj);
	if (msm_mpdec_kobject) {
		rc = sysfs_create_group(msm_mpdec_kobject,
					&msm_mpdec_attr_group);
		if (rc)
			pr_warn(MPDEC_TAG"sysfs: ERROR, could not create sysfs group");
		rc = sysfs_create_group(msm_mpdec_kobject,
					&msm_mpdec_stats_attr_group);
		if (rc)
			pr_warn(MPDEC_TAG"sysfs: ERROR, could not create sysfs stats group");
	} else
		pr_warn(MPDEC_TAG"sysfs: ERROR, could not create sysfs kobj");

	pr_info(MPDEC_TAG"%s init complete.", __func__);
	return err;
}
late_initcall(msm_mpdec_init);