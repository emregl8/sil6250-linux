// SPDX-License-Identifier: GPL-2.0
#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

struct lifetime_case {
	struct completion last_decrement;
	struct completion allow_close;
	struct completion remove_lock_attempt;
	struct completion remove_waiting;
	struct completion allow_remove_wait;
	struct completion removed;
	struct mutex *lifecycle;
	atomic_t *users;
	struct task_struct *remover;
	bool timed_out;
};

static struct lifetime_case *active_case;

static bool lifetime_dec_and_test(atomic_t *value)
{
	bool last = atomic_dec_and_test(value);
	struct lifetime_case *test = READ_ONCE(active_case);

	if (last && value == test->users) {
		complete(&test->last_decrement);
		if (!wait_for_completion_timeout(&test->allow_close,
					 msecs_to_jiffies(5000)))
			WRITE_ONCE(test->timed_out, true);
	}
	return last;
}

static void lifetime_mutex_lock(struct mutex *lock)
{
	struct lifetime_case *test = READ_ONCE(active_case);

	if (current == READ_ONCE(test->remover) && lock == test->lifecycle)
		complete(&test->remove_lock_attempt);
	mutex_lock(lock);
}

static void lifetime_wait_users(wait_queue_head_t *queue, atomic_t *users)
{
	struct lifetime_case *test = READ_ONCE(active_case);

	complete(&test->remove_waiting);
	if (!wait_for_completion_timeout(&test->allow_remove_wait,
				 msecs_to_jiffies(5000)))
		WRITE_ONCE(test->timed_out, true);
	wait_event(*queue, atomic_read(users) == 0);
}

#undef module_platform_driver
#undef wait_event
#define module_platform_driver(driver)
#define misc_deregister(misc) do { } while (0)
#define synchronize_irq(irq) do { } while (0)
#define atomic_dec_and_test(value) lifetime_dec_and_test(value)
#define mutex_lock(lock) lifetime_mutex_lock(lock)
#define wait_event(queue, condition) lifetime_wait_users(&(queue), &s->users)
#include "sil6250.c"
#undef wait_event
#undef mutex_lock
#undef atomic_dec_and_test
#undef synchronize_irq
#undef misc_deregister
#undef module_platform_driver

struct lifetime_worker {
	struct file file;
	struct completion done;
	struct task_struct *task;
};

static int lifetime_close(void *data)
{
	struct lifetime_worker *worker = data;

	sil6250_release(NULL, &worker->file);
	complete(&worker->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 0;
}

static int lifetime_remove(void *data)
{
	struct platform_device *pdev = data;
	struct lifetime_case *test = READ_ONCE(active_case);

	WRITE_ONCE(test->remover, current);
	sil6250_remove(pdev);
	complete(&test->removed);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 0;
}

static int lifetime_run_case(bool remove_first, int count)
{
	struct lifetime_case test;
	struct lifetime_worker *workers;
	struct platform_device *pdev;
	struct sil6250 *s;
	const char *stage = "setup";
	bool removed_early = false;
	int index, ret = 0;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	pdev = platform_device_alloc("sil6250-lifetime-test", PLATFORM_DEVID_NONE);
	if (!pdev) {
		kfree(s);
		return -ENOMEM;
	}
	workers = kcalloc(count, sizeof(*workers), GFP_KERNEL);
	if (!workers) {
		platform_device_put(pdev);
		kfree(s);
		return -ENOMEM;
	}
	init_completion(&s->rx_irq);
	mutex_init(&s->irq_lock);
	mutex_init(&s->lifecycle_lock);
	init_waitqueue_head(&s->users_wait);
	atomic_set(&s->users, count);
	platform_set_drvdata(pdev, s);
	init_completion(&test.last_decrement);
	init_completion(&test.allow_close);
	init_completion(&test.remove_lock_attempt);
	init_completion(&test.remove_waiting);
	init_completion(&test.allow_remove_wait);
	init_completion(&test.removed);
	test.lifecycle = &s->lifecycle_lock;
	test.users = &s->users;
	test.remover = NULL;
	test.timed_out = false;
	WRITE_ONCE(active_case, &test);

	if (!remove_first)
		complete(&test.allow_remove_wait);
	else {
		test.remover = kthread_run(lifetime_remove, pdev,
					    "sil6250-remove-test");
		if (IS_ERR(test.remover)) {
			ret = PTR_ERR(test.remover);
			test.remover = NULL;
			goto out;
		}
		stage = "remove wait";
		if (!wait_for_completion_timeout(&test.remove_waiting,
						 msecs_to_jiffies(2000))) {
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	for (index = 0; index < count; index++) {
		workers[index].file.private_data = s;
		init_completion(&workers[index].done);
		workers[index].task = kthread_run(lifetime_close,
						  &workers[index], "sil6250-close-test");
		if (IS_ERR(workers[index].task)) {
			ret = PTR_ERR(workers[index].task);
			workers[index].task = NULL;
			goto out;
		}
	}
	stage = "last decrement";
	if (!wait_for_completion_timeout(&test.last_decrement,
					 msecs_to_jiffies(2000))) {
		ret = -ETIMEDOUT;
		goto out;
	}

	if (!remove_first) {
		test.remover = kthread_run(lifetime_remove, pdev,
					    "sil6250-remove-test");
		if (IS_ERR(test.remover)) {
			ret = PTR_ERR(test.remover);
			test.remover = NULL;
			goto out;
		}
		stage = "remove lock";
		if (!wait_for_completion_timeout(&test.remove_lock_attempt,
						 msecs_to_jiffies(2000))) {
			ret = -ETIMEDOUT;
			goto out;
		}
	} else {
		complete(&test.allow_remove_wait);
	}
	stage = "premature removal";
	removed_early = wait_for_completion_timeout(&test.removed,
					     msecs_to_jiffies(100)) != 0;
	if (removed_early)
		ret = -EPROTO;

out:
	complete_all(&test.allow_close);
	complete_all(&test.allow_remove_wait);
	for (index = 0; index < count; index++) {
		if (!workers[index].task)
			continue;
		if (!wait_for_completion_timeout(&workers[index].done,
						 msecs_to_jiffies(2000)))
			ret = -ETIMEDOUT;
		kthread_stop(workers[index].task);
	}
	if (test.remover) {
		if (ret && atomic_read(&s->users) != 0) {
			atomic_set(&s->users, 0);
			wake_up_all(&s->users_wait);
		}
		if (!removed_early &&
		    !wait_for_completion_timeout(&test.removed,
						 msecs_to_jiffies(2000)))
			ret = -ETIMEDOUT;
		kthread_stop(test.remover);
	}
	if (READ_ONCE(test.timed_out))
		ret = -ETIMEDOUT;
	if (ret)
		pr_err("sil6250_lifetime_test: order=%d count=%d stage=%s users=%d error=%d\n",
		       remove_first, count, stage, atomic_read(&s->users), ret);
	WRITE_ONCE(active_case, NULL);
	platform_device_put(pdev);
	kfree(s);
	kfree(workers);
	return ret;
}

static int __init lifetime_test_init(void)
{
	int ret, iteration, count;
	bool remove_first;

	(void)&sil6250_driver;
	for (iteration = 0; iteration < 8; iteration++) {
		for (count = 1; count <= 4; count += 3) {
			for (remove_first = false; ; remove_first = true) {
				ret = lifetime_run_case(remove_first, count);
				if (ret)
					goto failed;
				if (remove_first)
					break;
			}
		}
	}
	pr_info("sil6250_lifetime_test: passed 32 removal/close schedules\n");
	return 0;
failed:
	pr_err("sil6250_lifetime_test: failed: %d\n", ret);
	return ret;
}

static void __exit lifetime_test_exit(void)
{
}

module_init(lifetime_test_init);
module_exit(lifetime_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SIL6250 device removal lifetime regression test");
