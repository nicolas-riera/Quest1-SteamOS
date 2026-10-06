#!/usr/bin/env python3
"""Backport the syscalls systemd >= 258 relies on to the Oculus 4.4 kernel.

  statx (291)              with STATX_MNT_ID and STATX_ATTR_MOUNT_ROOT (mount-point detection)
  pidfd_send_signal (424)
  pidfd_open (434)         anon-inode pidfd, pollable (POLLIN on exit), "Pid:" in fdinfo
  waitid(P_PIDFD)

usage: backport_syscalls.py <kernel tree>   (idempotent)
"""
import os
import sys

K = sys.argv[1]


def edit(path, old, new, marker):
    p = os.path.join(K, path)
    s = open(p).read()
    if marker in s:
        return
    if old not in s:
        sys.exit(f"{path}: anchor not found: {old[:60]!r}")
    open(p, "w").write(s.replace(old, new, 1))
    print("patched", path)


# --- syscall numbers (asm-generic, used by arm64) -----------------------------
edit("include/uapi/asm-generic/unistd.h",
     "#define __NR_mlock2 284\n__SYSCALL(__NR_mlock2, sys_mlock2)\n\n#undef __NR_syscalls\n#define __NR_syscalls 285\n",
     "#define __NR_mlock2 284\n__SYSCALL(__NR_mlock2, sys_mlock2)\n"
     "/* holo backport: numbers match mainline, gaps are sys_ni_syscall */\n"
     "__SYSCALL(285, sys_ni_syscall)\n__SYSCALL(286, sys_ni_syscall)\n__SYSCALL(287, sys_ni_syscall)\n"
     "__SYSCALL(288, sys_ni_syscall)\n__SYSCALL(289, sys_ni_syscall)\n__SYSCALL(290, sys_ni_syscall)\n"
     "#define __NR_statx 291\n__SYSCALL(__NR_statx, sys_statx)\n"
     + "".join(f"__SYSCALL({n}, sys_ni_syscall)\n" for n in range(292, 424)) +
     "#define __NR_pidfd_send_signal 424\n__SYSCALL(__NR_pidfd_send_signal, sys_pidfd_send_signal)\n"
     + "".join(f"__SYSCALL({n}, sys_ni_syscall)\n" for n in range(425, 434)) +
     "#define __NR_pidfd_open 434\n__SYSCALL(__NR_pidfd_open, sys_pidfd_open)\n"
     "\n#undef __NR_syscalls\n#define __NR_syscalls 435\n",
     "__NR_pidfd_open 434")

edit("include/linux/syscalls.h",
     "asmlinkage long sys_mlock2(",
     "struct statx;\n"
     "asmlinkage long sys_statx(int dfd, const char __user *path, unsigned flags,\n"
     "\t\t\t  unsigned mask, struct statx __user *buffer);\n"
     "asmlinkage long sys_pidfd_open(pid_t pid, unsigned int flags);\n"
     "asmlinkage long sys_pidfd_send_signal(int pidfd, int sig,\n"
     "\t\t\t\t       siginfo_t __user *info, unsigned int flags);\n"
     "asmlinkage long sys_mlock2(",
     "sys_pidfd_open")

# --- statx uapi ---------------------------------------------------------------
edit("include/uapi/linux/stat.h",
     "#endif /* _UAPI_LINUX_STAT_H */",
     """#include <linux/types.h>

struct statx_timestamp {
	__s64	tv_sec;
	__u32	tv_nsec;
	__s32	__reserved;
};

struct statx {
	__u32	stx_mask;
	__u32	stx_blksize;
	__u64	stx_attributes;
	__u32	stx_nlink;
	__u32	stx_uid;
	__u32	stx_gid;
	__u16	stx_mode;
	__u16	__spare0[1];
	__u64	stx_ino;
	__u64	stx_size;
	__u64	stx_blocks;
	__u64	stx_attributes_mask;
	struct statx_timestamp	stx_atime;
	struct statx_timestamp	stx_btime;
	struct statx_timestamp	stx_ctime;
	struct statx_timestamp	stx_mtime;
	__u32	stx_rdev_major;
	__u32	stx_rdev_minor;
	__u32	stx_dev_major;
	__u32	stx_dev_minor;
	__u64	stx_mnt_id;
	__u64	__spare2;
	__u64	__spare3[12];
};

#define STATX_TYPE		0x00000001U
#define STATX_MODE		0x00000002U
#define STATX_NLINK		0x00000004U
#define STATX_UID		0x00000008U
#define STATX_GID		0x00000010U
#define STATX_ATIME		0x00000020U
#define STATX_MTIME		0x00000040U
#define STATX_CTIME		0x00000080U
#define STATX_INO		0x00000100U
#define STATX_SIZE		0x00000200U
#define STATX_BLOCKS		0x00000400U
#define STATX_BASIC_STATS	0x000007ffU
#define STATX_BTIME		0x00000800U
#define STATX_MNT_ID		0x00001000U

#define STATX_ATTR_MOUNT_ROOT	0x00002000

#endif /* _UAPI_LINUX_STAT_H */""",
     "struct statx {")

edit("include/uapi/linux/fcntl.h",
     "#define AT_EMPTY_PATH",
     "#define AT_STATX_SYNC_TYPE\t0x6000\n#define AT_STATX_SYNC_AS_STAT\t0x0000\n"
     "#define AT_STATX_FORCE_SYNC\t0x2000\n#define AT_STATX_DONT_SYNC\t0x4000\n"
     "#define AT_EMPTY_PATH",
     "AT_STATX_SYNC_TYPE")

edit("fs/stat.c",
     "SYSCALL_DEFINE4(newfstatat,",
     r"""SYSCALL_DEFINE5(statx, int, dfd, const char __user *, filename, unsigned, flags,
		unsigned int, mask, struct statx __user *, buffer)
{
	struct kstat stat;
	struct statx tmp;
	struct path path;
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	int error;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE))
		return -EINVAL;
	if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE)
		return -EINVAL;
	if (mask & 0x80000000U)	/* STATX__RESERVED */
		return -EINVAL;
	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	if (flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;
retry:
	error = user_path_at(dfd, filename, lookup_flags, &path);
	if (error)
		return error;
	error = vfs_getattr(&path, &stat);
	if (!error) {
		memset(&tmp, 0, sizeof(tmp));
		tmp.stx_mask = STATX_BASIC_STATS | STATX_MNT_ID;
		tmp.stx_blksize = stat.blksize;
		tmp.stx_attributes_mask = STATX_ATTR_MOUNT_ROOT;
		if (path.mnt->mnt_root == path.dentry)
			tmp.stx_attributes |= STATX_ATTR_MOUNT_ROOT;
		tmp.stx_nlink = stat.nlink;
		tmp.stx_uid = from_kuid_munged(current_user_ns(), stat.uid);
		tmp.stx_gid = from_kgid_munged(current_user_ns(), stat.gid);
		tmp.stx_mode = stat.mode;
		tmp.stx_ino = stat.ino;
		tmp.stx_size = stat.size;
		tmp.stx_blocks = stat.blocks;
		tmp.stx_atime.tv_sec = stat.atime.tv_sec;
		tmp.stx_atime.tv_nsec = stat.atime.tv_nsec;
		tmp.stx_mtime.tv_sec = stat.mtime.tv_sec;
		tmp.stx_mtime.tv_nsec = stat.mtime.tv_nsec;
		tmp.stx_ctime.tv_sec = stat.ctime.tv_sec;
		tmp.stx_ctime.tv_nsec = stat.ctime.tv_nsec;
		tmp.stx_rdev_major = MAJOR(stat.rdev);
		tmp.stx_rdev_minor = MINOR(stat.rdev);
		tmp.stx_dev_major = MAJOR(stat.dev);
		tmp.stx_dev_minor = MINOR(stat.dev);
		tmp.stx_mnt_id = real_mount(path.mnt)->mnt_id;
		if (copy_to_user(buffer, &tmp, sizeof(tmp)))
			error = -EFAULT;
	}
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE4(newfstatat,""",
     "SYSCALL_DEFINE5(statx,")

edit("fs/stat.c",
     "#include <asm/uaccess.h>\n",
     "#include <asm/uaccess.h>\n#include \"mount.h\"\n",
     '#include "mount.h"')

# --- pidfd ----------------------------------------------------------------------
edit("include/linux/pid.h",
     "\tstruct rcu_head rcu;\n\tstruct upid numbers[1];\n};",
     "\tstruct rcu_head rcu;\n\twait_queue_head_t wait_pidfd;\t/* holo backport */\n\tstruct upid numbers[1];\n};\n\n"
     "extern const struct file_operations pidfd_fops;\nstruct pid *pidfd_get_pid(unsigned int fd);",
     "wait_pidfd")

edit("include/linux/pid.h",
     "#include <linux/rcupdate.h>\n",
     "#include <linux/rcupdate.h>\n#include <linux/wait.h>\n",
     "#include <linux/wait.h>")

edit("kernel/pid.c",
     "\tatomic_set(&pid->count, 1);\n",
     "\tatomic_set(&pid->count, 1);\n\tinit_waitqueue_head(&pid->wait_pidfd);\n",
     "init_waitqueue_head(&pid->wait_pidfd)")

edit("kernel/pid.c",
     "void __init pidhash_init(void)",
     r"""/* --- pidfd (holo backport of v5.3) --- */
static int pidfd_release(struct inode *inode, struct file *file)
{
	struct pid *pid = file->private_data;

	file->private_data = NULL;
	put_pid(pid);
	return 0;
}

static void pidfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct pid *pid = f->private_data;
	struct pid_namespace *ns = task_active_pid_ns(current);
	pid_t nr = -1;

	if (likely(pid_task(pid, PIDTYPE_PID)))
		nr = pid_nr_ns(pid, ns);
	seq_printf(m, "Pid:\t%d\n", nr);
}

static unsigned int pidfd_poll(struct file *file, struct poll_table_struct *pts)
{
	struct pid *pid = file->private_data;
	struct task_struct *task;
	unsigned int poll_flags = 0;

	poll_wait(file, &pid->wait_pidfd, pts);

	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	if (!task || (task->exit_state && thread_group_empty(task)))
		poll_flags = POLLIN | POLLRDNORM;
	rcu_read_unlock();
	return poll_flags;
}

const struct file_operations pidfd_fops = {
	.release = pidfd_release,
	.poll = pidfd_poll,
	.show_fdinfo = pidfd_show_fdinfo,
};

struct pid *pidfd_get_pid(unsigned int fd)
{
	struct fd f = fdget(fd);
	struct pid *pid;

	if (!f.file)
		return ERR_PTR(-EBADF);
	if (f.file->f_op == &pidfd_fops)
		pid = get_pid(f.file->private_data);
	else
		pid = ERR_PTR(-EBADF);
	fdput(f);
	return pid;
}

SYSCALL_DEFINE2(pidfd_open, pid_t, upid, unsigned int, flags)
{
	struct task_struct *task;
	struct pid *p;
	int fd;

	if (flags & ~O_NONBLOCK)
		return -EINVAL;
	if (upid <= 0)
		return -EINVAL;
	p = find_get_pid(upid);
	if (!p)
		return -ESRCH;
	task = get_pid_task(p, PIDTYPE_PID);
	if (!task) {
		put_pid(p);
		return -ESRCH;
	}
	if (!thread_group_leader(task)) {
		fd = -EINVAL;
	} else {
		fd = anon_inode_getfd("[pidfd]", &pidfd_fops, get_pid(p), O_RDWR | O_CLOEXEC | flags);
		if (fd < 0)
			put_pid(p);	/* the ref meant for the file */
	}
	put_task_struct(task);
	put_pid(p);
	return fd;
}

void __init pidhash_init(void)""",
     "SYSCALL_DEFINE2(pidfd_open")

edit("kernel/pid.c",
     "#include <linux/init_task.h>\n",
     "#include <linux/init_task.h>\n#include <linux/anon_inodes.h>\n#include <linux/file.h>\n"
     "#include <linux/poll.h>\n#include <linux/seq_file.h>\n#include <linux/syscalls.h>\n",
     "#include <linux/anon_inodes.h>")

# PIDFD_GET_INFO ioctl (v6.13): systemd 258 queries pid/creds through it
edit("kernel/pid.c",
     "const struct file_operations pidfd_fops = {\n\t.release = pidfd_release,\n",
     r"""struct pidfd_info {
	__u64 mask;
	__u64 cgroupid;
	__u32 pid, tgid, ppid, ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
	__s32 exit_code;
	__u32 coredump_mask;
	__u32 __spare1;
};
#define PIDFD_INFO_PID		(1UL << 0)
#define PIDFD_INFO_CREDS	(1UL << 1)

static long pidfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pid *pid = file->private_data;
	struct pidfd_info __user *uinfo = (void __user *)arg;
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct user_namespace *user_ns = current_user_ns();
	struct pidfd_info kinfo;
	struct task_struct *task;
	const struct cred *c;
	size_t usize = _IOC_SIZE(cmd);

	/* only PIDFD_GET_INFO = _IOWR(0xFF, 11, struct pidfd_info) */
	if (_IOC_TYPE(cmd) != 0xFF || _IOC_NR(cmd) != 11 || _IOC_DIR(cmd) != (_IOC_READ | _IOC_WRITE))
		return -ENOTTY;
	if (usize < 64)
		return -EINVAL;
	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;
	memset(&kinfo, 0, sizeof(kinfo));
	c = get_task_cred(task);
	kinfo.ruid = from_kuid_munged(user_ns, c->uid);
	kinfo.rgid = from_kgid_munged(user_ns, c->gid);
	kinfo.euid = from_kuid_munged(user_ns, c->euid);
	kinfo.egid = from_kgid_munged(user_ns, c->egid);
	kinfo.suid = from_kuid_munged(user_ns, c->suid);
	kinfo.sgid = from_kgid_munged(user_ns, c->sgid);
	kinfo.fsuid = from_kuid_munged(user_ns, c->fsuid);
	kinfo.fsgid = from_kgid_munged(user_ns, c->fsgid);
	put_cred(c);
	rcu_read_lock();
	kinfo.ppid = task_ppid_nr_ns(task, ns);
	rcu_read_unlock();
	kinfo.tgid = task_tgid_nr_ns(task, ns);
	kinfo.pid = task_pid_nr_ns(task, ns);
	put_task_struct(task);
	if (!kinfo.pid || !kinfo.tgid)
		return -ESRCH;
	kinfo.mask = PIDFD_INFO_PID | PIDFD_INFO_CREDS;
	if (copy_to_user(uinfo, &kinfo, min_t(size_t, usize, sizeof(kinfo))))
		return -EFAULT;
	return 0;
}

const struct file_operations pidfd_fops = {
	.release = pidfd_release,
	.unlocked_ioctl = pidfd_ioctl,
	.compat_ioctl = pidfd_ioctl,
""",
     "pidfd_ioctl")

# clone(CLONE_PIDFD) (v5.2): glibc falls back to clone() when clone3 is missing and
# still expects the pidfd in *parent_tid
edit("include/uapi/linux/sched.h",
     "#define CLONE_PTRACE",
     "#define CLONE_PIDFD\t0x00001000\t/* set if a pidfd should be placed in parent (holo backport) */\n#define CLONE_PTRACE",
     "CLONE_PIDFD")

edit("kernel/fork.c",
     "\tp = copy_process(clone_flags, stack_start, stack_size,\n\t\t\t child_tidptr, NULL, trace, tls, NUMA_NO_NODE);\n",
     "\tif ((clone_flags & CLONE_PIDFD) && (clone_flags & (CLONE_THREAD | CLONE_PARENT_SETTID)))\n\t\treturn -EINVAL;\n\n"
     "\tp = copy_process(clone_flags, stack_start, stack_size,\n\t\t\t child_tidptr, NULL, trace, tls, NUMA_NO_NODE);\n",
     "clone_flags & CLONE_PIDFD) && (clone_flags")

edit("kernel/fork.c",
     "\t\tif (clone_flags & CLONE_PARENT_SETTID)\n\t\t\tput_user(nr, parent_tidptr);\n",
     "\t\tif (clone_flags & CLONE_PARENT_SETTID)\n\t\t\tput_user(nr, parent_tidptr);\n\n"
     "\t\tif (clone_flags & CLONE_PIDFD) {\t/* holo backport */\n"
     "\t\t\tint pidfd = anon_inode_getfd(\"[pidfd]\", &pidfd_fops, get_pid(pid),\n"
     "\t\t\t\t\t\t     O_RDWR | O_CLOEXEC);\n"
     "\t\t\tif (pidfd < 0)\n\t\t\t\tput_pid(pid);\n"
     "\t\t\tput_user(pidfd, parent_tidptr);\n\t\t}\n",
     "if (clone_flags & CLONE_PIDFD) {")

edit("kernel/fork.c",
     "#include <linux/init.h>\n",
     "#include <linux/init.h>\n#include <linux/anon_inodes.h>\n",
     "#include <linux/anon_inodes.h>")

# cgroup2: systemd mounts with "nsdelegate" (4.13) and memory_* options; accept and
# ignore the ones newer kernels know instead of failing the mount (PID 1 freezes).
edit("kernel/cgroup.c",
     "\tif (is_v2) {\n\t\tif (data) {\n\t\t\tpr_err(\"cgroup2: unknown option \\\"%s\\\"\\n\", (char *)data);\n\t\t\treturn ERR_PTR(-EINVAL);\n\t\t}\n",
     "\tif (is_v2) {\n\t\tchar *opts = data, *tok;\n\n"
     "\t\twhile (opts && (tok = strsep(&opts, \",\")) != NULL) {\t/* holo backport */\n"
     "\t\t\tif (!*tok || !strcmp(tok, \"nsdelegate\") || !strcmp(tok, \"favordynmods\") ||\n"
     "\t\t\t    !strncmp(tok, \"memory_\", 7))\n\t\t\t\tcontinue;\n"
     "\t\t\tpr_err(\"cgroup2: unknown option \\\"%s\\\"\\n\", tok);\n"
     "\t\t\treturn ERR_PTR(-EINVAL);\n\t\t}\n",
     "/* holo backport */\n\t\t\tif (!*tok")

# synthetic uevents (v4.13): udevadm trigger writes "add <UUID> [KEY=VAL ...]" to
# /sys/.../uevent; 4.4 rejects that, so coldplug never announces devices to systemd
edit("lib/kobject_uevent.c",
     "#ifdef CONFIG_NET\n",
     r"""/* --- holo backport: kobject_synth_uevent (v4.13) --- */
static bool holo_uuid_valid(const char *s)
{
	int i;

	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return false;
		} else if (!isxdigit(s[i])) {
			return false;
		}
	}
	return true;
}

static const char *action_arg_word_end(const char *buf, const char *buf_end, char delim)
{
	const char *next = buf;

	while (next <= buf_end && *next != delim)
		if (!isalnum(*next++))
			return NULL;
	if (next == buf)
		return NULL;
	return next;
}

static int kobject_action_args(const char *buf, size_t count, struct kobj_uevent_env **ret_env)
{
	struct kobj_uevent_env *env;
	const char *next, *buf_end, *key;
	int key_len;
	int r = -EINVAL;

	if (count && (buf[count - 1] == '\n' || buf[count - 1] == '\0'))
		count--;
	if (!count)
		return -EINVAL;
	env = kzalloc(sizeof(*env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;
	if (count < 36 || !holo_uuid_valid(buf) || add_uevent_var(env, "SYNTH_UUID=%.*s", 36, buf))
		goto out;
	next = buf + 36;
	buf_end = buf + count - 1;
	while (next <= buf_end) {
		if (*next != ' ')
			goto out;
		key = ++next;
		if (key > buf_end)
			goto out;
		buf = next;
		next = action_arg_word_end(buf, buf_end, '=');
		if (!next || next > buf_end || *next != '=')
			goto out;
		key_len = next - buf;
		if (++next > buf_end)
			goto out;
		buf = next;
		next = action_arg_word_end(buf, buf_end, ' ');
		if (!next)
			goto out;
		if (add_uevent_var(env, "SYNTH_ARG_%.*s=%.*s", key_len, key, (int)(next - buf), buf))
			goto out;
	}
	r = 0;
out:
	if (r)
		kfree(env);
	else
		*ret_env = env;
	return r;
}

int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count)
{
	char *no_uuid_envp[] = { "SYNTH_UUID=0", NULL };
	enum kobject_action action;
	const char *action_args;
	struct kobj_uevent_env *env;
	size_t count_first;
	int r;

	action_args = strnchr(buf, count, ' ');
	if (action_args) {
		count_first = action_args - buf;
		action_args++;
		count -= count_first + 1;
	} else {
		count_first = count;
	}
	if (kobject_action_type(buf, count_first, &action))
		return -EINVAL;
	if (!action_args)
		return kobject_uevent_env(kobj, action, no_uuid_envp);
	r = kobject_action_args(action_args, count, &env);
	if (r)	/* unparsable args: still deliver the plain event */
		return kobject_uevent_env(kobj, action, no_uuid_envp);
	r = kobject_uevent_env(kobj, action, env->envp);
	kfree(env);
	return r;
}
EXPORT_SYMBOL_GPL(kobject_synth_uevent);

#ifdef CONFIG_NET
""",
     "kobject_synth_uevent(struct kobject *kobj")

edit("lib/kobject_uevent.c",
     "#include <linux/string.h>\n",
     "#include <linux/string.h>\n#include <linux/ctype.h>\n",
     "#include <linux/ctype.h>")

edit("include/linux/kobject.h",
     "int kobject_action_type(const char *buf, size_t count,",
     "int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count);\n"
     "int kobject_action_type(const char *buf, size_t count,",
     "kobject_synth_uevent")

for path, obj in (("kernel/module.c", "&mk->kobj"),
                  ("drivers/base/bus.c", "&drv->p->kobj"),
                  ("drivers/base/bus.c", "&bus->p->subsys.kobj")):
    var = "buffer" if path == "kernel/module.c" else "buf"
    edit(path,
         f"\tif (kobject_action_type({var}, count, &action) == 0)\n\t\tkobject_uevent({obj}, action);\n",
         f"\tkobject_synth_uevent({obj}, {var}, count);\t/* holo backport */\n",
         f"kobject_synth_uevent({obj}")
    edit(path,
         f"\tenum kobject_action action;\n\n\tkobject_synth_uevent({obj}",
         f"\tkobject_synth_uevent({obj}",
         f"{{\n\tkobject_synth_uevent({obj}")

edit("drivers/base/core.c",
     "\tif (kobject_action_type(buf, count, &action) == 0)\n\t\tkobject_uevent(&dev->kobj, action);\n\telse\n",
     "\tif (kobject_synth_uevent(&dev->kobj, buf, count))\t/* holo backport */\n",
     "kobject_synth_uevent(&dev->kobj")
edit("drivers/base/core.c",
     "\tenum kobject_action action;\n\n\tif (kobject_synth_uevent(&dev->kobj",
     "\tif (kobject_synth_uevent(&dev->kobj",
     "{\n\tif (kobject_synth_uevent(&dev->kobj")

# wake pidfd pollers when a thread-group leader is reaped / notifies its parent
edit("kernel/signal.c",
     " \t/* do_notify_parent_cldstop should have been called instead.  */\n \tBUG_ON(task_is_stopped_or_traced(tsk));\n",
     " \t/* do_notify_parent_cldstop should have been called instead.  */\n \tBUG_ON(task_is_stopped_or_traced(tsk));\n"
     "\n\t/* holo backport: wake up all pidfd waiters */\n\twake_up_all(&task_pid(tsk)->wait_pidfd);\n",
     "wake up all pidfd waiters")

edit("kernel/signal.c",
     "/**\n *  sys_tkill - send signal to one specific task",
     r"""/* holo backport of v5.1 pidfd_send_signal (pidfds from pidfd_open only) */
SYSCALL_DEFINE4(pidfd_send_signal, int, pidfd, int, sig,
		siginfo_t __user *, info, unsigned int, flags)
{
	struct fd f;
	struct pid *pid;
	siginfo_t kinfo;
	int ret;

	if (flags)
		return -EINVAL;
	f = fdget(pidfd);
	if (!f.file)
		return -EBADF;
	ret = -EINVAL;
	if (f.file->f_op != &pidfd_fops)
		goto err;
	pid = f.file->private_data;

	if (info) {
		ret = -EFAULT;
		if (copy_from_user(&kinfo, info, sizeof(siginfo_t)))
			goto err;
		ret = -EINVAL;
		if (unlikely(sig != kinfo.si_signo))
			goto err;
		ret = -EPERM;
		if ((task_pid(current) != pid) &&
		    (kinfo.si_code >= 0 || kinfo.si_code == SI_TKILL))
			goto err;
	} else {
		memset(&kinfo, 0, sizeof(kinfo));
		kinfo.si_signo = sig;
		kinfo.si_errno = 0;
		kinfo.si_code = SI_USER;
		kinfo.si_pid = task_tgid_vnr(current);
		kinfo.si_uid = from_kuid_munged(current_user_ns(), current_uid());
	}
	ret = kill_pid_info(sig, &kinfo, pid);
err:
	fdput(f);
	return ret;
}

/**
 *  sys_tkill - send signal to one specific task""",
     "SYSCALL_DEFINE4(pidfd_send_signal")

edit("kernel/signal.c",
     "#include <linux/syscalls.h>\n",
     "#include <linux/syscalls.h>\n#include <linux/file.h>\n",
     "#include <linux/file.h>")

# waitid(P_PIDFD, fd, ...)
edit("include/uapi/linux/wait.h",
     "#define P_PGID\t\t2\n",
     "#define P_PGID\t\t2\n#define P_PIDFD\t\t3\n",
     "P_PIDFD")

edit("kernel/exit.c",
     "\tcase P_PGID:\n\t\ttype = PIDTYPE_PGID;\n\t\tif (upid <= 0)\n\t\t\treturn -EINVAL;\n\t\tbreak;\n\tdefault:\n\t\treturn -EINVAL;\n\t}\n\n\tif (type < PIDTYPE_MAX)\n\t\tpid = find_get_pid(upid);\n",
     "\tcase P_PGID:\n\t\ttype = PIDTYPE_PGID;\n\t\tif (upid <= 0)\n\t\t\treturn -EINVAL;\n\t\tbreak;\n"
     "\tcase P_PIDFD:\t/* holo backport */\n\t\ttype = PIDTYPE_PID;\n\t\tif (upid < 0)\n\t\t\treturn -EINVAL;\n"
     "\t\tpid = pidfd_get_pid(upid);\n\t\tif (IS_ERR(pid))\n\t\t\treturn PTR_ERR(pid);\n\t\tbreak;\n"
     "\tdefault:\n\t\treturn -EINVAL;\n\t}\n\n\tif (type < PIDTYPE_MAX && !pid)\n\t\tpid = find_get_pid(upid);\n",
     "case P_PIDFD:")

print("syscall backport applied")
