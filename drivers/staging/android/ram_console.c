/* drivers/android/ram_console.c
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/persistent_ram.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include "ram_console.h"

static struct persistent_ram_zone *ram_console_zone;
static const char *bootinfo;
static size_t bootinfo_size;

static void
ram_console_write(struct console *console, const char *s, unsigned int count)
{
	struct persistent_ram_zone *prz = console->data;
	persistent_ram_write(prz, s, count);
}

static struct console ram_console = {
	.name	= "ram",
	.write	= ram_console_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED,
	.index	= -1,
};

void ram_console_enable_console(int enabled)
{
	if (enabled)
		ram_console.flags |= CON_ENABLED;
	else
		ram_console.flags &= ~CON_ENABLED;
}

static int __devinit ram_console_probe(struct platform_device *pdev)
{
	struct ram_console_platform_data *pdata = pdev->dev.platform_data;
	struct persistent_ram_zone *prz;
	
	prz = persistent_ram_init_ringbuffer(&pdev->dev, true);
	if (IS_ERR(prz))
		return PTR_ERR(prz);

	if (pdata) {
		bootinfo = kstrdup(pdata->bootinfo, GFP_KERNEL);
		if (bootinfo)
			bootinfo_size = strlen(bootinfo);
	}

	ram_console_zone = prz;
	ram_console.data = prz;

	register_console(&ram_console);
	return 0;
}

static struct platform_driver ram_console_driver = {
	.driver		= {
		.name	= "ram_console",
	},
	.probe = ram_console_probe,

};

static int __init ram_console_module_init(void)
{
	return platform_driver_register(&ram_console_driver);
}

#ifndef CONFIG_PRINTK
#define dmesg_restrict	0
#endif

#ifdef CONFIG_MDM9K_ERROR_CORRECTION
/*
 * check rpc link
 * stop checking if 1. link establishs. 2. link did not establish after 35 seconds.
 */
static void rpc_check_func(struct work_struct *work)
{
	struct rpc_link *rpc;
	static int count = 0;

	if (count++ >= 35) {
		printk(KERN_ERR "[K] MDM9K_ERROR_CORRECTION fail due to RPC connection is not ready\n");
		return;
	}

	rpc = container_of(work, struct rpc_link, dwork.work);
	rpc->rpc_client = oem_rapi_client_init();

	if (IS_ERR(rpc->rpc_client)) {
		schedule_delayed_work(&rpc->dwork, msecs_to_jiffies(1000));
		return;
	} else {
		RPC_READY = 1;
		wake_up(&rpc->rpcwq);
	}
}

/*
 * Get error message form mdm9k.
 * MDM9K_CHECK_ERROR, and input number(0,1) are confirmed by radio team.
 */
void query_error_message(struct msm_rpc_client *rpc_client, char *buf, int check_number)
{
	struct oem_rapi_client_streaming_func_arg arg;
	struct oem_rapi_client_streaming_func_ret ret;
	int err, ret_len;
	char input;

	err = 0;
	ret_len = MDM9K_BUFF_SIZE;
	input = check_number;
	arg.event = MDM9K_CHECK_ERROR;
	arg.cb_func = NULL;
	arg.handle = (void *)0;
	arg.in_len = 1;
	arg.input = &input;
	arg.out_len_valid = 1;
	arg.output_valid = 1;
	arg.output_size = MDM9K_BUFF_SIZE;
	ret.out_len = &ret_len;
	ret.output = NULL;
	err = oem_rapi_client_streaming_function(rpc_client, &arg, &ret);
	if (err) {
		printk(KERN_ERR "[K] ram_console: Receive data from modem failed: err = %d\n", err);
	} else if (!*ret.out_len) {
		if (check_number == 0)
			strncat(buf, "[SQA][ARM] no error occur\n", MDM9K_BUFF_SIZE);
		else if (check_number == 1)
			strncat(buf, "[SQA][QDSP6] no error occur\n", MDM9K_BUFF_SIZE);
		printk(KERN_INFO "[K] ram_console: query mdm9k message %d - out_len = 0\n", check_number);
		kfree(ret.out_len);
	} else {
		printk(KERN_INFO "[K] ram_console: query mdm9k message %d - out_len = %d\n", check_number, *ret.out_len);
		if (check_number == 0)
			strncpy(buf, ret.output, *ret.out_len);
		else if (check_number == 1)
			strncat(buf, ret.output, *ret.out_len);
		kfree(ret.out_len);
		kfree(ret.output);
	}
}
/*
 * Put error message in buf, and return buf length.
 * due to RPC link need a long time to create (35~50 seconds).
 * rpc_check_func() is used to check the link every 1 second(stop after 35 retries).
 */
int get_mdm9k_error_message(char *buf)
{
	struct rpc_link rpc;

	rpc.rpc_client = ERR_PTR(-ENOMEM);

	if (RPC_READY) {
		rpc.rpc_client = oem_rapi_client_init();
	} else {
		INIT_DELAYED_WORK(&rpc.dwork, rpc_check_func);
		schedule_delayed_work(&rpc.dwork, msecs_to_jiffies(25000));
		init_waitqueue_head(&rpc.rpcwq);
		wait_event_timeout(rpc.rpcwq, RPC_READY == 1, msecs_to_jiffies(70000));
		flush_delayed_work(&rpc.dwork); /* avoid RPC_READY is set by others after schedule rpc.dwork */
	}

	if (IS_ERR(rpc.rpc_client)) {
		strcpy(buf, "[mdm9k] MDM9K_ERROR_CORRECTION fail due to RPC link is not ready\n");
		return strlen(buf);
	}

	printk(KERN_INFO "[K] ram_console: RPC client ready...\n");
	query_error_message(rpc.rpc_client, buf, 0);
	query_error_message(rpc.rpc_client, buf, 1);
	oem_rapi_client_close();
	return strlen(buf);
}
#endif

static ssize_t ram_console_read_old(struct file *file, char __user *buf,
				    size_t len, loff_t *offset)
{
	loff_t pos = *offset;
	ssize_t count;
	struct persistent_ram_zone *prz = ram_console_zone;
	size_t old_log_size = persistent_ram_old_size(prz);
	const char *old_log = persistent_ram_old(prz);
	char *str;
	int ret;

	/* Main last_kmsg log */
	if (pos < old_log_size) {
		count = min(len, (size_t)(old_log_size - pos));
		if (copy_to_user(buf, old_log + pos, count))
			return -EFAULT;
		goto out;
	}

	/* ECC correction notice */
	pos -= old_log_size;
	count = persistent_ram_ecc_string(prz, NULL, 0);
	if (pos < count) {
		str = kmalloc(count, GFP_KERNEL);
		if (!str)
			return -ENOMEM;
		persistent_ram_ecc_string(prz, str, count + 1);
		count = min(len, (size_t)(count - pos));
		ret = copy_to_user(buf, str + pos, count);
		kfree(str);
		if (ret)
			return -EFAULT;
		goto out;
	}

	/* EOF */
	return 0;

out:
	*offset += count;
	return count;
}

static const struct file_operations ram_console_file_ops = {
	.owner = THIS_MODULE,
	.read = ram_console_read_old,
};

static int __init ram_console_late_init(void)
{
	struct proc_dir_entry *entry;
	struct persistent_ram_zone *prz = ram_console_zone;

	if (!prz)
		return 0;
		
	if (persistent_ram_old_size(prz) == 0)
		return 0;

	entry = create_proc_entry("last_kmsg", S_IFREG | S_IRUGO, NULL);
	if (!entry) {
		printk(KERN_ERR "[K] ram_console: failed to create proc entry\n");
		persistent_ram_free_old(prz);
		return 0;
	}

	entry->proc_fops = &ram_console_file_ops;
	entry->size = persistent_ram_old_size(prz) +
		persistent_ram_ecc_string(prz, NULL, 0) +
		bootinfo_size;

	return 0;
}

late_initcall(ram_console_late_init);
postcore_initcall(ram_console_module_init);
