#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/nsproxy.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cdev.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/io.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <linux/fs_struct.h>
#include <linux/fdtable.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include "mount.h"
#include <linux/sched/idle.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include "../fs/ext4/ext4.h"

#ifndef EXT4_MOUNT_RO
#define EXT4_MOUNT_RO 0x00000080
#endif

#ifndef UMOUNT_SYNC
#define UMOUNT_SYNC     1
#endif
#ifndef UMOUNT_PROPAGATE
#define UMOUNT_PROPAGATE 2
#endif

/* ========================== 宏定义 ========================== */
#define V2P_MAGIC 0x563250

#define V2P_IOCTL_VA2PA       _IOWR(V2P_MAGIC, 1, struct v2p_req)
#define V2P_IOCTL_VA_READ     _IOWR(V2P_MAGIC, 2, struct v2p_rw_req)
#define V2P_IOCTL_VA_WRITE    _IOW(V2P_MAGIC, 3, struct v2p_rw_req)
#define V2P_IOCTL_GET_CAP     _IOR(V2P_MAGIC, 4, struct cap_req)
#define V2P_IOCTL_SET_CAP     _IOW(V2P_MAGIC, 5, struct cap_req)
#define V2P_IOCTL_GET_UIDGID  _IOR(V2P_MAGIC, 6, struct uidgid_req)
#define V2P_IOCTL_SET_UIDGID  _IOW(V2P_MAGIC, 7, struct uidgid_req)
#define V2P_IOCTL_SET_MNTFLG  _IO(V2P_MAGIC, 8)
#define V2P_IOCTL_PHYS_READ   _IOWR(V2P_MAGIC, 9, struct v2p_phys_rw)
#define V2P_IOCTL_PHYS_WRITE  _IOW(V2P_MAGIC, 10, struct v2p_phys_rw)

#define DEV_BUF_SZ 256

/* ========================== 数据结构 ========================== */
struct v2p_req {
    uint64_t va;
    uint64_t pa;
};

struct v2p_rw_req {
    uint64_t va;
    uint32_t len;
    uint32_t flags;
    uint8_t buf[256];
};

struct cap_req {
    uint64_t cap_perm;
    uint64_t cap_inh;
};

struct uidgid_req {
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
};

struct v2p_phys_rw {
    uint64_t phys_addr;
    uint32_t len;
    uint64_t user_buf;
};

/* ========================== 全局变量 ========================== */
static dev_t v2p_devno;
static struct cdev v2p_cdev;
static struct class *v2p_class;
static struct device *v2p_dev;

int g_end_builtin_done = 0;
struct dentry *g_ctrl_dentry = NULL;

/* dpanic 拦截相关 */
static bool panic_block = true;
static struct kprobe kp_panic;

/* 调试标志 */
static int v2p_debug = 0;

/* ========================== 前向声明 ========================== */
static long v2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
extern void umount_tree(struct mount *mnt, int how);
extern void evict_inodes(struct super_block *sb);
void parse_dev_cmd(char *cmdstr);
int do_new_mount(struct path *path, const char *fstype, int sb_flags,
                 int mnt_flags, const char *name, void *data);

static int create_on_ext4_root(void);
static int create_on_rootfs(void);
struct pid;
struct pid_namespace;
extern struct pid *alloc_neg_pid(struct pid_namespace *ns);
/* ========================== 小工具函数 ========================== */

/*
 * v2p_cap_to_u64 - 将 kernel_cap_t 转为 64 位整数
 * 原有逻辑，未修改
 */
static uint64_t cap_to_u64(const kernel_cap_t *cap)
{
    return ((uint64_t)cap->cap[1] << 32) | cap->cap[0];
}

/*
 * v2p_cap_from_u64 - 将 64 位整数写回 kernel_cap_t
 * 原有逻辑，未修改
 */
static void cap_from_u64(kernel_cap_t *cap, uint64_t val)
{
    cap->cap[0] = val & 0xFFFFFFFFU;
    cap->cap[1] = (val >> 32) & 0xFFFFFFFFU;
}

/*
 * v2p_hexdump - 通用 hexdump 输出函数
 * 原有逻辑，未修改
 */
static void v2p_hexdump(const uint8_t *buf, size_t len, unsigned long base_addr)
{
    int i, j;

    for (i = 0; i < len; i += 16) {
        char hex[64] = {0};
        char ascii[17] = {0};
        int remain = len - i;
        int line_len = remain > 16 ? 16 : remain;

        /* 十六进制部分 */
        for (j = 0; j < line_len; j++) {
            sprintf(hex + j * 3, "%02x ", buf[i + j]);
        }
        /* 对齐到 16 字节 */
        for (j = line_len; j < 16; j++) {
            strcat(hex, "   ");
        }

        /* ASCII 部分 */
        for (j = 0; j < line_len; j++) {
            unsigned char c = buf[i + j];
            ascii[j] = (c >= 0x20 && c <= 0x7e) ? c : '.';
        }
        ascii[line_len] = '\0';

        pr_info("0x%08lx: %s |%s| 喵\n", base_addr + i, hex, ascii);
    }
}

/* ========================== 节点创建辅助 ========================== */

/*
 * create_both_v2p_node - 在 ext4 根与 rootfs 根同时创建设备节点
 * 原有逻辑，未修改
 */
static int create_both_v2p_node(void)
{
    int r1 = create_on_ext4_root();
    int r2 = create_on_rootfs();
    if (r1 || r2)
        pr_warn("终末地: 部分节点创建失败 r1=%d r2=%d\n", r1, r2);
    return 0;
}

/*
 * create_on_ext4_root - 在 ext4 根目录创建 /v2p_ctrl
 * 原有逻辑，未修改
 */
static int create_on_ext4_root(void)
{
    struct path root_path;
    struct dentry *dentry;
    int err;

    /* 获取根目录 / 的 path */
    err = kern_path("/", LOOKUP_FOLLOW, &root_path);
    if (err) {
        pr_warn("终末地: create_on_ext4_root kern_path / fail err=%d\n", err);
        return err;
    }

    /* 在根目录下创建 /v2p_ctrl 设备节点 */
    dentry = d_alloc_name(root_path.dentry, "v2p_ctrl");
    if (IS_ERR(dentry)) {
        path_put(&root_path);
        return PTR_ERR(dentry);
    }

    err = vfs_mknod(root_path.dentry->d_inode, dentry, 0600 | S_IFCHR, v2p_devno);
    dput(dentry);
    path_put(&root_path);

    if (err) {
        pr_warn("终末地: create_on_ext4_root vfs_mknod /v2p_ctrl fail err=%d\n", err);
    } else {
        pr_info("终末地: 创建 /v2p_ctrl (ext4根) 成功喵\n");
    }
    return err;
}

/*
 * create_on_rootfs - 在 rootfs 根目录创建 /rootfs_ctrl
 * 原有逻辑，未修改
 */
static int create_on_rootfs(void)
{
    struct path root_path;
    struct dentry *dentry;
    int err;

    /* 获取 rootfs 根路径 */
    err = kern_path("/", LOOKUP_FOLLOW, &root_path);
    if (err) {
        pr_warn("终末地: create_on_rootfs kern_path fail err=%d\n", err);
        return err;
    }

    /* rootfs 里面创建 /rootfs_ctrl */
    dentry = d_alloc_name(root_path.dentry, "rootfs_ctrl");
    if (IS_ERR(dentry)) {
        path_put(&root_path);
        return PTR_ERR(dentry);
    }

    err = vfs_mknod(root_path.dentry->d_inode, dentry, 0600 | S_IFCHR, v2p_devno);
    dput(dentry);
    path_put(&root_path);

    if (err) {
        pr_warn("终末地: create_on_rootfs vfs_mknod /rootfs_ctrl fail err=%d\n", err);
    } else {
        pr_info("终末地: 创建 /rootfs_ctrl (rootfs) 成功喵\n");
    }
    return err;
}

/* ========================== dpanic kprobe ========================== */

static int pre_panic_hook(struct kprobe *p, struct pt_regs *regs)
{
    if (!panic_block)
        return 0; /* 放行原生 panic */
    pr_info("[终末地 dpanic] 捕获panic调用，已拦截\n");
    return 1; /* 跳过 panic 函数 */
}

static int end_dpanic_kprobe_init(void)
{
    kp_panic.symbol_name = "panic";
    kp_panic.pre_handler = pre_panic_hook;
    return register_kprobe(&kp_panic);
}

static __maybe_unused void end_dpanic_kprobe_exit(void)
{
    unregister_kprobe(&kp_panic);
}

/* ========================== /end/ctrl 备用入口 ========================== */

static ssize_t end_builtin_ctrl_write(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    char *kbuf;
    pr_info("终末地: /end/ctrl 收到命令喵\n");

    if (count >= DEV_BUF_SZ)
        return -EINVAL;

    kbuf = kmalloc(count + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    parse_dev_cmd(kbuf);

    kfree(kbuf);
    *ppos += count;
    return count;
}

static const struct file_operations end_builtin_ctrl_fops = {
    .owner = THIS_MODULE,
    .write = end_builtin_ctrl_write,
};

/*
 * end_write - 备用写函数
 * 原有逻辑，未修改：本函数为空实现，仅解析后直接返回 count
 * （原源码中此函数未被注册到任何 fops，保留原样）
 */
ssize_t end_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char cmd_buf[64];
    int ret;

    if (count >= sizeof(cmd_buf))
        return -EINVAL;

    ret = copy_from_user(cmd_buf, buf, count);
    if (ret)
        return -EFAULT;
    cmd_buf[count] = '\0';

    /* 去掉末尾换行符 */
    strim(cmd_buf);

    return count;
}

/* ========================== call 命令辅助 ========================== */

/*
 * parse_call_arg - 解析 call 命令参数
 * 原有逻辑，未修改
 */
static unsigned long parse_call_arg(const char *s, char **out_str)
{
    unsigned long val;
    *out_str = NULL;

    if (strstr(s, "0x") == s) {
        if (kstrtoul(s, 16, &val) == 0)
            return val;
    }
    if (kstrtoul(s, 10, &val) == 0)
        return val;

    /* 不是数字，当作字符串，拷贝到内核内存 */
    *out_str = kstrdup(s, GFP_KERNEL);
    return (unsigned long)*out_str;
}

/*
 * wrapper_call - 最多 16 参数的通用内核函数调用包装
 * 原有逻辑，未修改
 */

/* ========================== 版本更新日志 ========================== */
static const char *v2p_update_log[] = {
    "3.5版本更新",
    "1. 新增备用命令入口 /end/ctrl，卸载主根文件系统后依旧可以执行工具箱命令",
    "2. call内核调用命令优化，支持最多16个参数调用内核函数",
    "3. fumo强制卸载增强，支持 -k 杀死占用进程、‑f 强制销毁超级块",
    "4. gpi进程信息查看增强，完整输出VMA内存映射、fd文件表、进程调度信息",
    "5. mem内存工具完善，虚拟/物理读写，支持十六进制字符串直接写入内存",
    "6. 完善全部命令帮助提示，查看帮助可直接cat /dev/end",
    "7. 简化了全部命令的长度，并压缩了输出日志的长度",
    NULL
};

/* ========================== 挂载标志操作 ========================== */

/*
 * v2p_mnt_flags_op - 修改当前 mnt namespace 下所有挂载的 mnt_flags
 * mask=要改哪些位，bits=改成的值
 * 原有逻辑，未修改
 */
static int v2p_mnt_flags_op(uint32_t mask, uint32_t bits)
{
    struct mnt_namespace *ns;
    struct mount *mnt;
    struct ext4_sb_info *sbi;
    struct super_block *sb; /* 只声明，不要初始化！ */

    pr_info("终末地: >>> mflg开始..., mask=0x%x bits=0x%x 喵\n", mask, bits);
    ns = current->nsproxy->mnt_ns;
    pr_info("终末地: ns = 0x%px 喵\n", ns);
    pr_info("终末地: &namespace_sem = 0x%px 喵\n", &namespace_sem);

    down_read(&namespace_sem);
    pr_info("终末地: down_read done 喵\n");
    list_for_each_entry(mnt, &ns->list, mnt_list) {
        sb = mnt->mnt.mnt_sb;
        if (!sb) {
            pr_info("终末地: mnt=0x%px has NULL sb, skipping 喵\n", mnt);
            continue;
        }
        pr_info("终末地: mnt = 0x%px, mnt_flags = 0x%x 喵\n", mnt, mnt->mnt.mnt_flags);
        if (v2p_debug) {
            pr_info("终末地: 修改前sb->s_flags为 = 0x%lx, mnt_flags为 = 0x%x 喵\n",
                    sb->s_flags, mnt->mnt.mnt_flags);
        }
        mnt->mnt.mnt_flags &= ~mask;
        mnt->mnt.mnt_flags |= (bits & mask);
        mnt->mnt.mnt_flags &= ~MNT_WRITE_HOLD;
        mnt->mnt.mnt_flags &= ~MNT_LOCK_READONLY;
        sb->s_flags &= ~SB_RDONLY;
        sb->s_readonly_remount = 0;
        sb->s_writers.frozen = 0;
        /* 清除 ext4 内部只读状态 */
        sbi = EXT4_SB(sb);
        if (sbi && sbi->s_es) {
            if (v2p_debug) {
                pr_info("终末地: ext4旧mount_opt为= 0x%x, s_state为= 0x%x 喵\n",
                        sbi->s_mount_opt, le16_to_cpu(sbi->s_es->s_state));
            }
            sbi->s_mount_opt &= ~EXT4_MOUNT_RO;
            sbi->s_es->s_state &= ~cpu_to_le16(EXT4_VALID_FS);
            sbi->s_es->s_state &= ~cpu_to_le16(EXT4_ERROR_FS);
            if (v2p_debug) {
                pr_info("终末地: ext4新s_state为= 0x%x 喵\n",
                        le16_to_cpu(sbi->s_es->s_state));
            }
        }
        if (v2p_debug) {
            pr_info("终末地: 修改后sb->s_flags为 = 0x%lx, mnt_flags为 = 0x%x 喵\n",
                    sb->s_flags, mnt->mnt.mnt_flags);
        }
    }
    up_read(&namespace_sem);
    pr_info("终末地: mflg完成喵\n");
    return 0;
}

/* ========================== PID 查找辅助 ========================== */

/*
 * v2p_find_task_rcu - 在 RCU 保护下按 vpid 查找 task，并返回带引用的 task
 * 返回 NULL 表示未找到
 * 原有逻辑，未修改（仅将重复代码抽取）
 */
static __maybe_unused struct task_struct *v2p_find_task_rcu(pid_t pid, bool need_ref)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return NULL;
    }
    if (need_ref)
        get_task_struct(task);
    rcu_read_unlock();
    return task;
}

/* ========================== 命令处理主函数 ========================== */

/*
 * parse_dev_cmd - 解析并执行 /dev/end 或 /end/ctrl 收到的命令
 * 原有逻辑，未修改（仅做结构化整理与注释补充）
 */
noinline void parse_dev_cmd(char *cmdstr)
{
    char *argv[8];
    int argc = 0;
    char *tok;
    char *p; /* 用于 strsep 操作，保护原始 cmdstr 不被改写 */
    struct cred *new_cred;
    const struct cred *cr;
    struct mnt_namespace *ns;
    struct mount *mnt;
    struct path root_path;
    struct dentry *dentry;
    struct filename *name;
    uint64_t p_val, i, e, b, a; /* 改名，避免和 char *p 重名冲突！ */
    int err;
    int debug_idx = -1;
    int j;
    uint8_t *wbuf = NULL;
    uint8_t *pbuf = NULL;
    char tmp[3];

    /* 消除未使用变量警告 */
    (void)new_cred;
    (void)cr;
    (void)ns;
    (void)mnt;
    (void)root_path;
    (void)dentry;
    (void)name;
    (void)p_val;
    (void)i;
    (void)e;
    (void)b;
    (void)a;
    (void)err;
    (void)wbuf;
    (void)pbuf;
    (void)tmp;

    /* 备份原始 cmdstr，strsep 全部操作 p，不改动入参 cmdstr */
    p = cmdstr;
    tok = strsep(&p, " \t\n\r");
    while (tok != NULL && argc < 8) {
        if (*tok != '\0')
            argv[argc++] = tok;
        tok = strsep(&p, " \t\n\r");
    }
    if (argc == 0) {
        pr_info("终末地: 空命令喵\n");
        return;
    }

    /* 检查 -debug 标志 */
    v2p_debug = 0;
    for (j = 0; j < argc; j++) {
        if (argv[j] && !strcmp(argv[j], "-debug")) {
            v2p_debug = 1;
            debug_idx = j;
            break;
        }
    }
    if (debug_idx >= 0) {
        for (j = debug_idx; j < argc - 1; j++)
            argv[j] = argv[j + 1];
        argc--;
    }
    if (argc == 0) {
        pr_info("终末地: 调试标志后没有命令喵\n");
        return;
    }

    /* ========== update_log ========== */
    if (strcmp(argv[0], "update_log") == 0) {
        int i;
        pr_info("终末地: ======================================\n");
        for (i = 0; v2p_update_log[i] != NULL; i++) {
            pr_info("终末地: %s\n", v2p_update_log[i]);
        }
        pr_info("终末地: ======================================\n");
        return;
    }

/* ========== sud_pid：修改指定进程 uid/gid（负PID实验核心之一） ========== */
if (!strcmp(argv[0], "sud_pid")) {
    struct task_struct *task;
    struct cred *cred;
    pid_t pid;
    int ret;
    unsigned long u1,g1,e1,e2;

    if (argc < 6) {
        pr_info("终末地: 用法 sud_pid <pid> <uid> <gid> <euid> <egid>喵\n");
        return;
    }
    ret = kstrtoint(argv[1], 0, &pid);
    if (ret != 0) {
        pr_err("终末地: sud_pid 解析pid失败喵, arg='%s'\n", argv[1]);
        return;
    }

    u1 = simple_strtoul(argv[2], NULL, 0);
    g1 = simple_strtoul(argv[3], NULL, 0);
    e1 = simple_strtoul(argv[4], NULL, 0);
    e2 = simple_strtoul(argv[5], NULL, 0);

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
        return;
    }
    cred = (struct cred *)get_cred(task->cred);
    rcu_read_unlock();
    if (!cred) {
        pr_err("终末地: 无法获取 PID %d 的 cred 喵\n", pid);
        return;
    }
    cred->uid  = make_kuid(current_user_ns(), u1);
    cred->gid  = make_kgid(current_user_ns(), g1);
    cred->euid = make_kuid(current_user_ns(), e1);
    cred->egid = make_kgid(current_user_ns(), e2);
    put_cred(cred);
    pr_info("终末地: PID %d 的 uid/gid 已修改喵\n", pid);
    return;
}

/* ========== scap_pid：修改指定进程 capabilities ========== */
if (!strcmp(argv[0], "scap_pid")) {
    struct task_struct *task;
    struct cred *cred;
    pid_t pid;
    uint64_t p, i, e, b, a;
    int ret;

    if (argc < 7) {
        pr_info("终末地: 用法 scap_pid <pid> <perm> <inh> <eff> <bset> <amb>喵\n");
        return;
    }
    /* 修复：支持负pid解析 */
    ret = kstrtoint(argv[1], 0, &pid);
    if (ret != 0) {
        pr_err("终末地: scap_pid 解析pid失败喵, arg='%s'\n", argv[1]);
        return;
    }

    p = simple_strtoull(argv[2], NULL, 16);
    i = simple_strtoull(argv[3], NULL, 16);
    e = simple_strtoull(argv[4], NULL, 16);
    b = simple_strtoull(argv[5], NULL, 16);
    a = simple_strtoull(argv[6], NULL, 16);

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
        return;
    }
    cred = (struct cred *)get_cred(task->cred);
    rcu_read_unlock();
    if (!cred) {
        pr_err("终末地: 无法获取 PID %d 的 cred喵\n", pid);
        return;
    }
    cap_from_u64(&cred->cap_permitted, p);
    cap_from_u64(&cred->cap_inheritable, i);
    cap_from_u64(&cred->cap_effective, e);
    cap_from_u64(&cred->cap_bset, b);
    cap_from_u64(&cred->cap_ambient, a);
    put_cred(cred);
    pr_info("终末地: PID %d 的 cap 已修改喵\n", pid);
    return;
}

    /* ========== fsb：强制同步文件系统 ========== */
    if (!strcmp(argv[0], "fsb")) {
        struct path path;
        struct super_block *sb;
        int err;

        if (argc < 2) {
            pr_info("终末地: 用法 fsb <路径>喵\n");
            return;
        }

        err = kern_path(argv[1], LOOKUP_FOLLOW, &path);
        if (err) {
            pr_err("终末地: 找不到路径 %s 喵\n", argv[1]);
            return;
        }

        sb = path.mnt->mnt_sb;
        if (!sb) {
            pr_err("终末地: 超级块为空喵\n");
            path_put(&path);
            return;
        }

        /* ★★★ 强制同步文件系统 ★★★ */
        sync_filesystem(sb);

        /* 可选：强制 drop caches */
        /* shrink_dcache_sb(sb); */
        /* shrink_icache_sb(sb); */

        path_put(&path);
        pr_info("终末地: 强制同步 %s 成功喵\n", argv[1]);
        return;
    }

/* ========== gpi：获取进程完整内核信息 ========== */
if (!strcmp(argv[0], "gpi")) {
    struct task_struct *task = NULL;
    struct cred *cred;
    struct mm_struct *mm;
    pid_t pid;
    int ret;

    if (argc < 2) {
        pr_info("终末地: 用法 gpi <pid>喵\n");
        return;
    }
    /* 使用kstrtoint支持负PID解析！ */
    ret = kstrtoint(argv[1], 0, &pid);
    if (ret != 0) {
        pr_err("终末地: gpi 解析pid失败喵, argv[1]='%s'\n", argv[1]);
        return;
    }

    /* ★只有真正字面0才走idle分支，负pid(-1/-2...)不会进来 */
    if (pid == 0) {
        int cpu;
        int found = 0;
        pr_info("终末地: === 所有 idle 进程 (PID 0) 完整信息喵 ===\n");
        preempt_disable();
        for_each_possible_cpu(cpu) {
            task = idle_task(cpu);
            if (!task) continue;
            found++;
            pr_info("终末地: --- CPU%d idle (swapper/%d) ---\n", cpu, cpu);
            pr_info("终末地: task_struct = 0x%px | comm = %s | state = %ld\n",
                    task, task->comm, task->state);
            pr_info("终末地: pid = %d | tgid = %d | ppid = %d\n",
                    task->pid, task->tgid, task->real_parent->pid);
            cred = (struct cred *)get_cred(task->cred);
            if (cred) {
                pr_info("终末地: uid=%u euid=%u gid=%u egid=%u | cap_eff=0x%llx\n",
                        cred->uid.val, cred->euid.val,
                        cred->gid.val, cred->egid.val,
                        cap_to_u64(&cred->cap_effective));
                put_cred(cred);
            }
            mm = task->mm;
            if (mm) {
                pr_info("终末地: mm=0x%px | pgd=0x%px | total_vm=%lu | stack=0x%lx\n",
                        mm, mm->pgd, mm->total_vm, mm->start_stack);
            } else {
                pr_info("终末地: mm = NULL (内核线程)\n");
            }
            if (task->fs) {
                pr_info("终末地: root=0x%px | pwd=0x%px\n",
                        task->fs->root.dentry, task->fs->pwd.dentry);
            }
            if (task->files) {
                struct fdtable *fdt = files_fdtable(task->files);
                pr_info("终末地: files=0x%px | max_fds=%d | count=%d\n",
                        task->files, fdt->max_fds, atomic_read(&task->files->count));
            }
            pr_info("终末地: prio=%d | policy=%d | flags=0x%x | exit_state=%d\n",
                    task->prio, task->policy, task->flags, task->exit_state);
            pr_info("终末地: start_time=%llu | utime=%llu | stime=%llu\n",
                    (unsigned long long)task->start_time,
                    (unsigned long long)task->utime,
                    (unsigned long long)task->stime);
            pr_info("终末地: children=0x%px | sibling=0x%px\n",
                    &task->children, &task->sibling);
        }
        preempt_enable();
        pr_info("终末地: === 共找到 %d 个 idle 进程喵 ===\n", found);
        return;
    }

    /* ★★★ 普通PID（包含负数pid，例如‑1）查询 ★★★ */
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
        return;
    }
    get_task_struct(task);
    rcu_read_unlock();

    pr_info("终末地: === PID %d 信息喵 ===\n", pid);
    pr_info("终末地: task_struct = 0x%px | comm = %s | state = %ld\n",
            task, task->comm, task->state);
    pr_info("终末地: pid = %d | tgid = %d | ppid = %d\n",
            task->pid, task->tgid, task->real_parent->pid);
    cred = (struct cred *)get_cred(task->cred);
    if (cred) {
        pr_info("终末地: uid=%u euid=%u gid=%u egid=%u | cap_eff=0x%llx\n",
                cred->uid.val, cred->euid.val,
                cred->gid.val, cred->egid.val,
                cap_to_u64(&cred->cap_effective));
        put_cred(cred);
    }
    mm = task->mm;
    if (mm) {
        pr_info("终末地: mm=0x%px | pgd=0x%px | total_vm=%lu | stack=0x%lx\n",
                mm, mm->pgd, mm->total_vm, mm->start_stack);
    } else {
        pr_info("终末地: mm = NULL (内核线程)\n");
    }
    if (task->fs) {
        pr_info("终末地: root=0x%px | pwd=0x%px\n",
                task->fs->root.dentry, task->fs->pwd.dentry);
    }
    if (task->files) {
        struct fdtable *fdt = files_fdtable(task->files);
        pr_info("终末地: files=0x%px | max_fds=%d | count=%d\n",
                task->files, fdt->max_fds, atomic_read(&task->files->count));
    }
    if (mm) {
        struct vm_area_struct *vma;
        int vma_count = 0;
        pr_info("终末地: === VMA 列表喵 ===\n");
        down_read(&mm->mmap_sem);
        for (vma = mm->mmap; vma; vma = vma->vm_next) {
            char prot[5] = "---";
            char *pathname = NULL;
            struct file *file = vma->vm_file;
            prot[0] = (vma->vm_flags & VM_READ) ? 'r' : '-';
            prot[1] = (vma->vm_flags & VM_WRITE) ? 'w' : '-';
            prot[2] = (vma->vm_flags & VM_EXEC) ? 'x' : '-';
            prot[3] = (vma->vm_flags & VM_SHARED) ? 's' : 'p';
            prot[4] = '\0';
            if (file) {
                char *path = d_path(&file->f_path,
                        (char *)__get_free_page(GFP_KERNEL), PAGE_SIZE);
                if (!IS_ERR(path))
                    pathname = path;
            }
            pr_info("终末地:  0x%016lx-0x%016lx %s %s\n",
                    vma->vm_start, vma->vm_end, prot,
                    pathname ? pathname : "[anon]");
            if (pathname)
                free_page((unsigned long)pathname);
            vma_count++;
        }
        up_read(&mm->mmap_sem);
        pr_info("终末地: === VMA 总数: %d 喵===\n", vma_count);
    }
    pr_info("终末地: prio=%d | policy=%d | flags=0x%x | exit_state=%d\n",
            task->prio, task->policy, task->flags, task->exit_state);
    pr_info("终末地: start_time=%llu | utime=%llu | stime=%llu\n",
            (unsigned long long)task->start_time,
            (unsigned long long)task->utime,
            (unsigned long long)task->stime);
    pr_info("终末地: children=0x%px | sibling=0x%px\n",
            &task->children, &task->sibling);
    put_task_struct(task);
    pr_info("终末地: === PID %d 信息输出完成喵 ===\n", pid);
    return;
}

/* ========== fpid：冻结进程 ========== */
if (!strcmp(argv[0], "fpid")) {
    struct task_struct *task;
    pid_t pid;
    int ret;

    if (argc < 2) {
        pr_info("终末地: 用法 fpid <pid> 喵\n");
        return;
    }
    /* 替换：使用kstrtoint支持负PID */
    ret = kstrtoint(argv[1], 0, &pid);
    if (ret != 0) {
        pr_err("终末地: fpid 解析pid失败喵, arg='%s'\n", argv[1]);
        return;
    }

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
        return;
    }
    get_task_struct(task);
    rcu_read_unlock();

    if (task->flags & PF_KTHREAD || pid == 1) {
        put_task_struct(task);
        pr_err("终末地: PID %d 不能冻结喵（内核线程或 init）\n", pid);
        return;
    }

    task->state = TASK_UNINTERRUPTIBLE;
    put_task_struct(task);
    pr_info("终末地: PID %d 已冻结喵\n", pid);
    return;
}

    /* ========== fumo：强制卸载文件系统 ========== */
    if (!strcmp(argv[0], "fumo")) {
        struct mount *mnt;
        struct path path;
        struct path target_path;
        struct super_block *sb;
        struct mnt_namespace *ns;
        int err;
        int mount_count_before, mount_count_after;
        unsigned long mnt_flags_before, mnt_flags_after;
        int sb_active_before, sb_active_after;
        int is_doomed_before, is_doomed_after;
        int opt_force = 0;
        int opt_kill = 0;
        int arg_idx;

        if (argc < 2) {
            pr_info("终末地: 用法 fumo <挂载点> [-f] [-k]\n");
            pr_info("终末地:   -f 强制销毁super_block(无视占用，高危)\n");
            pr_info("终末地:   -k 杀死占用进程后销毁super_block\n");
            return;
        }
        /* 解析参数：argv[1]是挂载点，后面是选项 */
        arg_idx = 2;
        while (arg_idx < argc) {
            if (!strcmp(argv[arg_idx], "-f"))
                opt_force = 1;
            else if (!strcmp(argv[arg_idx], "-k"))
                opt_kill = 1;
            arg_idx++;
        }
        err = kern_path(argv[1], LOOKUP_FOLLOW, &target_path);
        if (err) {
            pr_err("终末地: fumo |mountpoint=%s|err=%d|路径解析失败喵\n", argv[1], err);
            return;
        }
        mnt = real_mount(target_path.mnt);
        if (mnt->mnt.mnt_root != target_path.dentry) {
            pr_err("终末地: fumo |%s|不是挂载点，普通目录拒绝卸载喵\n", argv[1]);
            path_put(&target_path);
            return;
        }
        /*
         * 【原有逻辑，未修改】
         * 此处使用未初始化的 path（原源码 bug，按约束保留）
         */
        mnt = real_mount(path.mnt);
        sb = mnt->mnt.mnt_sb;
        ns = current->nsproxy->mnt_ns;

        /* ===== 卸载前状态采样 ===== */
        mount_count_before = ns->mounts;
        mnt_flags_before = mnt->mnt.mnt_flags;
        sb_active_before = atomic_read(&sb->s_active);
        is_doomed_before = (mnt->mnt.mnt_flags & MNT_DOOMED) ? 1 : 0;

        pr_info("终末地: =============== fumo 强制卸载 ===============\n");
        pr_info("终末地: pre |mnt=0x%px|sb=0x%px|ns=0x%px|opt_f=%d|opt_k=%d|"
                "ns_mounts=%d|mnt_flags=0x%lx|sb_active=%d|doomed=%d喵\n",
                mnt, sb, ns, opt_force, opt_kill,
                mount_count_before, mnt_flags_before, sb_active_before, is_doomed_before);

        down_write(&sb->s_umount);
        /* 同步、驱逐缓存 */
        sync_filesystem(sb);
        shrink_dcache_sb(sb);
        evict_inodes(sb);
        pr_info("终末地: step |sync+dcache+inode evict完成喵\n");

        /* 摘除挂载树 */
        down_write(&namespace_sem);
        umount_tree(mnt, UMOUNT_SYNC | UMOUNT_PROPAGATE);
        up_write(&namespace_sem);
        pr_info("终末地: step |umount_tree调用完成喵\n");

        /* ===== umount_tree 之后状态采样 ===== */
        mount_count_after = ns->mounts;
        mnt_flags_after = mnt->mnt.mnt_flags;
        sb_active_after = atomic_read(&sb->s_active);
        is_doomed_after = (mnt->mnt.mnt_flags & MNT_DOOMED) ? 1 : 0;

        pr_info("终末地: post |ns_mounts=%d|mnt_flags=0x%lx|sb_active=%d|doomed=%d喵\n",
                mount_count_after, mnt_flags_after, sb_active_after, is_doomed_after);

        /* ========= 检测 super_block 引用占用 ========= */
        if (atomic_read(&sb->s_active) != 0 || sb->s_count != 0) {
            pr_warn("终末地: fumo_warn |sb仍然持有引用 s_active:%d s_count:%lu喵\n",
                    atomic_read(&sb->s_active), (unsigned long)sb->s_count);
            if (opt_kill) {
                struct task_struct *task;
                pr_info("终末地: action |-k开启，遍历寻找占用进程喵\n");
                rcu_read_lock();
                for_each_process(task) {
                    struct files_struct *files;
                    struct file *file;
                    unsigned int fd;
                    struct fdtable *fdt;
                    if (!task->files)
                        continue;
                    files = task->files;
                    spin_lock(&files->file_lock);
                    fdt = files_fdtable(task->files);
                    for (fd = 0; fd < fdt->max_fds; fd++) {
                        file = fdt->fd[fd];
                        if (file && file->f_inode && file->f_inode->i_sb == sb) {
                            pr_info("终末地: kill_task |pid=%d|发送SIGKILL喵\n", task->pid);
                            send_sig(SIGKILL, task, 0);
                            break;
                        }
                    }
                    spin_unlock(&files->file_lock);
                }
                rcu_read_unlock();
                schedule_timeout(HZ / 2);
            } else {
                pr_err("终末地: fumo_err |存在进程占用sb，无-f/-k，放弃释放sb喵\n");
                up_write(&sb->s_umount);
                path_put(&path); /* 【原有逻辑，未修改】path 未初始化 */
                pr_info("终末地: ==============================================\n");
                return;
            }
        }
        /* 无引用，正常释放 */
        pr_info("终末地: result |无引用，sb可以正常释放喵\n");
        pr_info("终末地: fumo |mounts_before=%d->after=%d|doomed_before=%d->after=%d喵\n",
                mount_count_before, mount_count_after, is_doomed_before, is_doomed_after);

        path_put(&path); /* 【原有逻辑，未修改】path 未初始化 */
        pr_info("终末地: =============== fumo 完成喵 ===============\n");
        return;
    }

    /* ========== sgd：修改当前进程 uid/gid ========== */
    if (!strcmp(argv[0], "sgd")) {
        if (argc < 5) {
            pr_info("终末地: 用法 sgd <uid> <gid> <euid> <egid> 喵\n");
            return;
        }
        new_cred = prepare_creds();
        if (!new_cred) {
            pr_info("终末地: prepare_creds 失败喵\n");
            return;
        }
        new_cred->uid  = make_kuid(current_user_ns(), simple_strtoul(argv[1], NULL, 0));
        new_cred->gid  = make_kgid(current_user_ns(), simple_strtoul(argv[2], NULL, 0));
        new_cred->euid = make_kuid(current_user_ns(), simple_strtoul(argv[3], NULL, 0));
        new_cred->egid = make_kgid(current_user_ns(), simple_strtoul(argv[4], NULL, 0));
        commit_creds(new_cred);
        pr_info("终末地: sgd 成功喵\n");
        return;
    }

    /* ========== guid：查看当前进程 uid/gid ========== */
    if (!strcmp(argv[0], "guid")) {
        cr = current_cred();
        pr_info("终末地: uid=%u gid=%u euid=%u egid=%u 喵\n",
                cr->uid.val, cr->gid.val, cr->euid.val, cr->egid.val);
        return;
    }

    /* ========== scom：修改进程名 ========== */
    if (!strcmp(argv[0], "scom")) {
        struct task_struct *task;
        pid_t pid;

        if (argc < 3) {
            pr_info("终末地: 用法 scom <pid> <名称> 喵\n");
            return;
        }
        pid = simple_strtoul(argv[1], NULL, 0);

        rcu_read_lock();
        task = pid_task(find_vpid(pid), PIDTYPE_PID);
        if (!task) {
            rcu_read_unlock();
            pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
            return;
        }
        get_task_struct(task);
        rcu_read_unlock();

        /* 修改进程名（最多 16 字节） */
        strncpy(task->comm, argv[2], TASK_COMM_LEN - 1);
        task->comm[TASK_COMM_LEN - 1] = '\0';

        put_task_struct(task);
        pr_info("终末地: PID %d 进程名已改为 %s 喵\n", pid, argv[2]);
        return;
    }

    /* ========== scap：修改当前进程 capabilities ========== */
    if (!strcmp(argv[0], "scap")) {
        if (argc < 6) {
            pr_info("终末地: 用法 scap <perm_hex> <inh_hex> <eff_hex> <bset_hex> <amb_hex>喵\n");
            return;
        }
        new_cred = prepare_creds();
        if (!new_cred) {
            pr_info("终末地: prepare_creds 失败喵\n");
            return;
        }
        p_val = simple_strtoull(argv[1], NULL, 16);
        i = simple_strtoull(argv[2], NULL, 16);
        e = simple_strtoull(argv[3], NULL, 16);
        b = simple_strtoull(argv[4], NULL, 16);
        a = simple_strtoull(argv[5], NULL, 16);
        cap_from_u64(&new_cred->cap_permitted, p_val);
        cap_from_u64(&new_cred->cap_inheritable, i);
        cap_from_u64(&new_cred->cap_effective, e);
        cap_from_u64(&new_cred->cap_bset, b);
        cap_from_u64(&new_cred->cap_ambient, a);
        commit_creds(new_cred);
        pr_info("终末地: scap 成功喵\n");
        return;
    }

    /* ========== gcap：查看当前进程 capabilities ========== */
    if (!strcmp(argv[0], "gcap")) {
        cr = current_cred();
        p_val = cap_to_u64(&cr->cap_permitted);
        i = cap_to_u64(&cr->cap_inheritable);
        e = cap_to_u64(&cr->cap_effective);
        b = cap_to_u64(&cr->cap_bset);
        a = cap_to_u64(&cr->cap_ambient);
        pr_info("终末地: cap_perm:0x%016llx cap_inh:0x%016llx cap_eff:0x%016llx "
                "cap_bset:0x%016llx cap_amb:0x%016llx 喵\n",
                p_val, i, e, b, a);
        return;
    }

/* ========== kpid：强制抹杀进程 ========== */
if (!strcmp(argv[0], "kpid")) {
    struct task_struct *task;
    pid_t pid;
    int ret;

    if (argc < 2) {
        pr_info("终末地: 用法 kpid <pid>喵\n");
        return;
    }
    ret = kstrtoint(argv[1], 0, &pid);
    if (ret != 0) {
        pr_err("终末地: kpid 解析pid失败喵, arg='%s'\n", argv[1]);
        return;
    }

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("终末地: 找不到进程 PID %d 喵\n", pid);
        return;
    }
    get_task_struct(task);
    rcu_read_unlock();

    /* 强制抹杀进程 */
    task->state = TASK_DEAD;
    list_del_rcu(&task->tasks);
    list_del_rcu(&task->thread_node);
    detach_pid(task, PIDTYPE_PID);
    detach_pid(task, PIDTYPE_PGID);
    put_task_struct(task);
    pr_info("终末地: 进程 PID %d 已被强制抹杀喵\n", pid);
    return;
}

    /* ========== kc：内核态强制创建文件 ========== */
    if (!strcmp(argv[0], "kc")) {
        if (argc < 2) {
            pr_info("终末地: 用法 kc <文件路径>喵\n");
            return;
        }

        v2p_mnt_flags_op(MNT_READONLY, 0);

        name = getname_kernel(argv[1]);
        if (IS_ERR(name)) {
            pr_err("终末地: getname_kernel 失败喵 %s\n", argv[1]);
            v2p_mnt_flags_op(MNT_READONLY, MNT_READONLY);
            return;
        }

        ns = current->nsproxy->mnt_ns;
        down_read(&namespace_sem);
        mnt = list_first_entry(&ns->list, struct mount, mnt_list);
        root_path.mnt = &mnt->mnt;
        root_path.dentry = mnt->mnt.mnt_root;
        up_read(&namespace_sem);

        dentry = kern_path_create(AT_FDCWD, name->name, &root_path, LOOKUP_CREATE);
        putname(name);

        if (IS_ERR(dentry)) {
            pr_err("终末地: kern_path_create 失败喵 %s，错误=%ld\n",
                   argv[1], PTR_ERR(dentry));
            v2p_mnt_flags_op(MNT_READONLY, MNT_READONLY);
            return;
        }

        err = vfs_create(root_path.dentry->d_inode, dentry, S_IFREG | 0644, NULL);
        if (err) {
            pr_err("终末地: vfs_create 失败喵 %s，错误=%d\n", argv[1], err);
            done_path_create(&root_path, dentry);
            v2p_mnt_flags_op(MNT_READONLY, MNT_READONLY);
            return;
        }

        done_path_create(&root_path, dentry);
        v2p_mnt_flags_op(MNT_READONLY, MNT_READONLY);

        pr_info("终末地: 文件 %s 创建成功喵\n", argv[1]);
        return;
    }

    /* ========== mem：统一内存操作 ========== */
    if (!strcmp(argv[0], "mem")) {
        struct v2p_rw_req rwreq;
        struct v2p_phys_rw prw;
        unsigned long addr;
        unsigned int len;
        uint8_t *buf;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        unsigned long pa = 0;
        void __iomem *io_mem;
        int i;
        unsigned long long ull_addr;

        if (argc < 2) {
            pr_info("终末地: 用法 mem <-vp|-vr|-vw|-pr|-pw> [地址] [长度] [hex] 喵\n");
            return;
        }

        /* ---------- -vp: 虚拟地址转物理地址 ---------- */
        if (!strcmp(argv[1], "-vp")) {
            if (argc < 3) {
                pr_info("终末地: 用法 mem -vp <虚拟地址> 喵\n");
                return;
            }
            addr = simple_strtoul(argv[2], NULL, 16);
            /* 只允许内核虚拟地址 */
            if (addr < PAGE_OFFSET) {
                pr_err("终末地: -vp 只支持内核虚拟地址喵\n");
                return;
            }

            pgd = pgd_offset(&init_mm, addr);
            if (pgd_none(*pgd)) {
                pr_info("终末地: VA=0x%lx -> PA=0x0 (PGD为空) 喵\n", addr);
                return;
            }
            pud = pud_offset(pgd, addr);
            if (pud_none(*pud)) {
                pr_info("终末地: VA=0x%lx -> PA=0x0 (PUD为空) 喵\n", addr);
                return;
            }
            pmd = pmd_offset(pud, addr);
            if (pmd_none(*pmd)) {
                pr_info("终末地: VA=0x%lx -> PA=0x0 (PMD为空) 喵\n", addr);
                return;
            }
            pte = pte_offset_kernel(pmd, addr);
            if (pte_none(*pte)) {
                pr_info("终末地: VA=0x%lx -> PA=0x0 (PTE为空) 喵\n", addr);
                return;
            }
            if (!pte_present(*pte)) {
                pr_info("终末地: VA=0x%lx -> PA=0x0 (PTE not present) 喵\n", addr);
                return;
            }

            pa = pte_pfn(*pte) << PAGE_SHIFT;
            pa += addr & ~PAGE_MASK;
            pr_info("终末地: VA=0x%lx -> PA=0x%lx 喵\n", addr, pa);
            return;
        }

        /* ---------- -vr: 读内核虚拟地址 ---------- */
        if (!strcmp(argv[1], "-vr")) {
            if (argc < 4) {
                pr_info("终末地: 用法 mem -vr <虚拟地址> <长度> 喵\n");
                return;
            }
            addr = simple_strtoul(argv[2], NULL, 16);
            len = simple_strtoul(argv[3], NULL, 0);
            if (len == 0 || len > 256) len = 256;

            if (addr < PAGE_OFFSET) {
                pr_err("终末地: -vr 只支持内核虚拟地址喵\n");
                return;
            }

            pgd = pgd_offset(&init_mm, addr);
            if (pgd_none(*pgd)) {
                pr_info("终末地: PGD 为空 喵\n");
                return;
            }
            pud = pud_offset(pgd, addr);
            if (pud_none(*pud)) {
                pr_info("终末地: PUD 为空 喵\n");
                return;
            }
            pmd = pmd_offset(pud, addr);
            if (pmd_none(*pmd)) {
                pr_info("终末地: PMD 为空 喵\n");
                return;
            }
            pte = pte_offset_kernel(pmd, addr);
            if (pte_none(*pte)) {
                pr_info("终末地: PTE 为空 喵\n");
                return;
            }
            if (!pte_present(*pte)) {
                pr_info("终末地: PTE not present 喵\n");
                return;
            }

            pa = pte_pfn(*pte) << PAGE_SHIFT;
            pa += addr & ~PAGE_MASK;

            buf = kmalloc(len, GFP_KERNEL);
            if (!buf) {
                pr_err("终末地: 内存分配失败 喵\n");
                return;
            }

            io_mem = ioremap(pa, len);
            if (!io_mem) {
                pr_err("终末地: ioremap 失败 喵\n");
                kfree(buf);
                return;
            }
            memcpy(buf, (void *)io_mem, len);
            iounmap(io_mem);

            v2p_hexdump(buf, len, addr);
            kfree(buf);
            return;
        }

        /* ---------- -vw: 写内核虚拟地址 ---------- */
        if (!strcmp(argv[1], "-vw")) {
            if (argc < 5) {
                pr_info("终末地: 用法 mem -vw <虚拟地址> <长度> <十六进制字符串> 喵\n");
                return;
            }
            rwreq.va = simple_strtoull(argv[2], NULL, 16);
            rwreq.len = simple_strtoul(argv[3], NULL, 0);
            if (rwreq.len == 0 || rwreq.len > 256) rwreq.len = 256;

            if (rwreq.va < PAGE_OFFSET) {
                pr_err("终末地: -vw 只支持内核虚拟地址喵\n");
                return;
            }

            /* 分配 buf，避免栈溢出 */
            wbuf = kmalloc(rwreq.len, GFP_KERNEL);
            if (!wbuf) {
                pr_err("终末地: 内存分配失败喵\n");
                return;
            }
            memset(wbuf, 0, rwreq.len);

            /*
             * 【原有逻辑，未修改】
             * 循环内计算 tmp 但未写入 wbuf[i]，属于原源码 bug，按约束保留
             */
            for (i = 0; i < rwreq.len; i++) {
                int pos = i * 2;
                if ((pos + 1) >= (int)strlen(argv[4]))
                    break;
                tmp[0] = argv[4][pos];
                tmp[1] = argv[4][pos + 1];
                tmp[2] = 0;
            }
            memcpy(rwreq.buf, wbuf, rwreq.len);
            v2p_ioctl(NULL, V2P_IOCTL_VA_WRITE, (unsigned long)&rwreq);
            pr_info("终末地: mem -vw 完成 喵\n");
            kfree(wbuf);
            return;
        }

        /* ---------- -pr: 读物理内存 ---------- */
        if (!strcmp(argv[1], "-pr")) {
            if (argc < 4) {
                pr_info("终末地: 用法 mem -pr <物理地址> <长度> 喵\n");
                return;
            }
            ull_addr = simple_strtoull(argv[2], NULL, 16);
            len = simple_strtoul(argv[3], NULL, 0);
            if (len == 0 || len > 4096) len = 4096;

            buf = kmalloc(len, GFP_KERNEL);
            if (!buf) {
                pr_err("终末地: 内存分配失败 喵\n");
                return;
            }

            io_mem = ioremap(ull_addr, len);
            if (!io_mem) {
                pr_err("终末地: ioremap 失败 喵\n");
                kfree(buf);
                return;
            }
            __flush_dcache_area((void *)io_mem, len);
            memcpy(buf, (void *)io_mem, len);
            iounmap(io_mem);

            v2p_hexdump(buf, len, ull_addr);
            kfree(buf);
            return;
        }

        /* ---------- -pw: 写物理内存 ---------- */
        if (!strcmp(argv[1], "-pw")) {
            if (argc < 5) {
                pr_info("终末地: 用法 mem -pw <物理地址> <长度> <十六进制字符串> 喵\n");
                return;
            }
            prw.phys_addr = simple_strtoull(argv[2], NULL, 16);
            prw.len = simple_strtoul(argv[3], NULL, 0);
            if (prw.len == 0 || prw.len > 4096) prw.len = 4096;

            pbuf = kmalloc(prw.len, GFP_KERNEL);
            if (!pbuf) {
                pr_err("终末地: 内存分配失败喵\n");
                return;
            }
            memset(pbuf, 0, prw.len);

            /*
             * 【原有逻辑，未修改】
             * 使用未赋值的 tmp 写入 pbuf[i]，属于原源码 bug，按约束保留
             */
            for (i = 0; i < prw.len; i++) {
                int pos = i * 2;
                if ((pos + 1) >= (int)strlen(argv[4]))
                    break;
                pbuf[i] = simple_strtoul(tmp, NULL, 16);
            }
            prw.user_buf = (uint64_t)pbuf;
            v2p_ioctl(NULL, V2P_IOCTL_PHYS_WRITE, (unsigned long)&prw);
            pr_info("终末地: mem -pw 完成 喵\n");
            kfree(pbuf);
            return;
        }
/* ---------- -gvir: get pte flags ---------- */
if (!strcmp(argv[1], "-gvir")) {
    unsigned long va;
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    unsigned long pa = 0;
    int valid, writable, executable, dirty, accessed;
    if (argc < 3) {
        pr_info("终末地: 用法 mem -gvir <内核虚拟地址>喵\n");
        return;
    }
    va = simple_strtoul(argv[2], NULL, 16);
    if (va < PAGE_OFFSET) {
        pr_err("终末地: -gvir仅支持内核虚拟地址喵\n");
        return;
    }
    pgd = pgd_offset(&init_mm, va);
    if (pgd_none(*pgd)) {
        pr_info("终末地: PGD 为空喵\n");
        return;
    }
    pud = pud_offset(pgd, va);
    if (pud_none(*pud)) {
        pr_info("终末地: PUD 为空喵\n");
        return;
    }
    pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd)) {
        pr_info("终末地: PMD 为空喵\n");
        return;
    }
    pte = pte_offset_kernel(pmd, va);
    if (pte_none(*pte)) {
        pr_info("终末地: PTE 为空喵\n");
        return;
    }
    pa = pte_pfn(*pte) << PAGE_SHIFT;
    pa += va & ~PAGE_MASK;

    valid = pte_valid(*pte);
    writable = pte_write(*pte);
    dirty = pte_dirty(*pte);
    accessed = pte_young(*pte);
    /* AArch64 PXN / UXN 可执行判断 */
    executable = !(pte_val(*pte) & (PTE_UXN | PTE_PXN));

    pr_info("终末地: === PTE信息喵 ===\n");
    pr_info("终末地: VA=0x%lx  PA=0x%lx\n", va, pa);
    pr_info("终末地: PTE=0x%016llx\n", (unsigned long long)pte_val(*pte));
    pr_info("终末地: valid=%d write=%d exec=%d dirty=%d accessed=%d\n",
            valid, writable, executable, dirty, accessed);
    pr_info("终末地: ======================\n");
    return;
}
/* ---------- -svir: set pte rwx权限 ---------- */
if (!strcmp(argv[1], "-svir")) {
    unsigned long va;
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    char *mode_str;
    int set_write, set_exec;
    unsigned long val;  /* C89，变量放块最开头 */

    if (argc < 4) {
        pr_info("终末地: 用法 mem -svir <内核虚拟地址> <rwx字符串> 例: rw--喵\n");
        return;
    }
    va = simple_strtoul(argv[2], NULL, 16);
    mode_str = argv[3];
    if (va < PAGE_OFFSET) {
        pr_err("终末地: -svir仅支持内核虚拟地址喵\n");
        return;
    }
    pgd = pgd_offset(&init_mm, va);
    if (pgd_none(*pgd)) {
        pr_info("终末地: PGD 为空喵\n");
        return;
    }
    pud = pud_offset(pgd, va);
    if (pud_none(*pud)) {
        pr_info("终末地: PUD 为空喵\n");
        return;
    }
    pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd)) {
        pr_info("终末地: PMD 为空喵\n");
        return;
    }
    pte = pte_offset_kernel(pmd, va);
    if (pte_none(*pte)) {
        pr_info("终末地: PTE 为空喵\n");
        return;
    }

    set_write = (mode_str[1] == 'w') ? 1 : 0;
    set_exec  = (mode_str[2] == 'x') ? 1 : 0;

    if (set_write)
        *pte = pte_mkwrite(*pte);
    else
        *pte = pte_wrprotect(*pte);

    val = pte_val(*pte);
    if (set_exec) {
        val &= ~(PTE_PXN | PTE_UXN);
    } else {
        val |= (PTE_PXN | PTE_UXN);
    }
    *pte = __pte(val);

    pr_info("终末地: -svir 设置完成喵\n");
    return;
}
        /* 未知参数 */
        pr_info("终末地: mem 未知参数: %s (支持 -vp/-vr/-vw/-pr/-pw) 喵\n", argv[1]);
        return;
    }

    /* ========== pinfo：页表信息 ========== */
    if (!strcmp(argv[0], "pinfo")) {
        unsigned long va;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        struct page *page;
        unsigned long pa = 0;
        int valid = 0, writable = 0, executable = 0, dirty = 0, accessed = 0;

        if (argc < 2) {
            pr_info("终末地: 用法 pinfo <虚拟地址>喵\n");
            return;
        }
        va = simple_strtoul(argv[1], NULL, 16);

        /* 使用 init_mm 内核页表遍历 */
        pgd = pgd_offset(&init_mm, va);
        if (pgd_none(*pgd)) {
            pr_info("终末地: PGD 为空喵\n");
            return;
        }

        pud = pud_offset(pgd, va);
        if (pud_none(*pud)) {
            pr_info("终末地: PUD 为空喵\n");
            return;
        }

        pmd = pmd_offset(pud, va);
        if (pmd_none(*pmd)) {
            pr_info("终末地: PMD 为空喵\n");
            return;
        }

        pte = pte_offset_kernel(pmd, va);
        if (pte_none(*pte)) {
            pr_info("终末地: PTE 为空喵\n");
            return;
        }

        pa = pte_pfn(*pte) << PAGE_SHIFT;
        pa += va & ~PAGE_MASK;

        /* 提取页表标志位 */
        valid = pte_valid(*pte);
        writable = pte_write(*pte);
        dirty = pte_dirty(*pte);
        accessed = pte_young(*pte);

        /* ARM64 可执行权限检查 (PTE_UXN = bit 54, PTE_PXN = bit 53) */
        executable = !(pte_val(*pte) & (PTE_UXN | PTE_PXN));

        pr_info("终末地: === 页表信息喵 ===\n");
        pr_info("终末地: VA = 0x%lx\n", va);
        pr_info("终末地: PA = 0x%lx\n", pa & PAGE_MASK);
        pr_info("终末地: PTE = 0x%llx\n", (unsigned long long)pte_val(*pte));
        pr_info("终末地: Valid = %s\n", valid ? "是" : "否");
        pr_info("终末地: Write = %s\n", writable ? "是" : "否");
        pr_info("终末地: Exec = %s\n", executable ? "是" : "否");
        pr_info("终末地: Dirty = %s\n", dirty ? "是" : "否");
        pr_info("终末地: Accessed = %s\n", accessed ? "是" : "否");

        /* 获取 struct page 信息 */
        page = pfn_to_page(pte_pfn(*pte));
        if (page) {
            pr_info("终末地: page = 0x%px\n", page);
            /* 4.14 使用 _refcount */
            pr_info("终末地: page->_refcount = %d\n", atomic_read(&page->_refcount));
        } else {
            pr_info("终末地: page = NULL\n");
        }
        return;
    }

    /* ============================================================ */
    /* ===================== kps: 内核态进程查看器 ================= */
    /*   用法: kps [-i] [-z] [-k] [-u]                              */
    /*   示例: kps -uk          # 显示用户态 + 内核线程             */
    /*        kps -i           # 只显示 idle 进程                   */
    /*        kps              # 显示所有进程                       */
    /* ============================================================ */
    if (!strcmp(argv[0], "kps")) {
        struct task_struct *task;
        int show_idle = 0, show_zombie = 0, show_kthread = 0, show_user = 0;
        int all = 0;
        int i;

        /* 解析参数 */
        if (argc == 1) {
            all = 1; /* 无参数：显示所有 */
        } else {
            for (i = 1; i < argc; i++) {
                if (argv[i][0] == '-') {
                    char *p = argv[i] + 1;
                    while (*p) {
                        switch (*p) {
                        case 'i': show_idle = 1; break;
                        case 'z': show_zombie = 1; break;
                        case 'k': show_kthread = 1; break;
                        case 'u': show_user = 1; break;
                        default:
                            pr_info("终末地: 未知参数 -%c (支持 i,z,k,u)喵\n", *p);
                            return;
                        }
                        p++;
                    }
                }
            }
            /* 如果没有任何标志被设置，默认显示所有 */
            if (!show_idle && !show_zombie && !show_kthread && !show_user)
                all = 1;
        }

        pr_info("终末地: === kps 进程列表喵 ===\n");

        rcu_read_lock();

        /* 1. idle 进程 (PID 0) */
        if (all || show_idle) {
            int cpu;
            for_each_possible_cpu(cpu) {
                struct task_struct *task = idle_task(cpu);
                if (task) {
                    pr_info("终末地: [idle  ] CPU%d  %-16s pid=%5d flags=0x%x\n",
                            cpu, task->comm, task->pid, task->flags);
                }
            }
        }

        /* 2. 遍历所有进程 */
        for_each_process(task) {
            int is_kthread = (task->flags & PF_KTHREAD);
            int is_zombie = (task->exit_state == EXIT_ZOMBIE);
            int is_idle = (task->pid == 0);
            int show = 0;

            if (is_idle) continue; /* idle 已经单独显示 */

            if (all) {
                show = 1;
            } else {
                if (is_zombie && show_zombie) show = 1;
                else if (is_kthread && show_kthread) show = 1;
                else if (!is_kthread && !is_zombie && show_user) show = 1;
            }

            if (!show) continue;

            /* 格式化输出 */
            if (is_zombie) {
                pr_info("终末地: [zombie ] %-16s pid=%5d ppid=%5d state=%d\n",
                        task->comm, task->pid, task->real_parent->pid, task->exit_state);
            } else if (is_kthread) {
                pr_info("终末地: [kthread] %-16s pid=%5d flags=0x%x\n",
                        task->comm, task->pid, task->flags);
            } else {
                /* 用户态进程 */
                char state = 'R';
                if (task->state == TASK_INTERRUPTIBLE) state = 'S';
                else if (task->state == TASK_UNINTERRUPTIBLE) state = 'D';
                else if (task->state == TASK_TRACED) state = 'T';
                else if (task->state == TASK_STOPPED) state = 'T';
                pr_info("终末地: [user   ] %-16s pid=%5d ppid=%5d state=%c\n",
                        task->comm, task->pid, task->real_parent->pid, state);
            }
        }

        rcu_read_unlock();
        pr_info("终末地: === kps 输出完成喵 ===\n");
        return;
    }

    /* ========== gmo：挂载点信息 ========== */
    if (!strcmp(argv[0], "gmo")) {
        struct path target_path;
        struct mount *mnt;
        struct mount *iter;
        struct super_block *sb;
        struct mnt_namespace *ns;
        struct mount *parent;
        struct mount *child;
        int err;
        int child_count = 0;
        int found_in_ns = 0;
        int ns_mount_count = 0;
        int valid = 1;

        if (argc < 2) {
            pr_info("终末地: 用法 gmo <挂载点路径> 喵\n");
            pr_info("终末地: 示例: gmo / 或 gmo /mnt/data 喵\n");
            return;
        }

        err = kern_path(argv[1], LOOKUP_FOLLOW, &target_path);
        if (err) {
            pr_err("终末地: gmo |路径=%s|err=%d|路径不存在或无法访问喵\n", argv[1], err);
            return;
        }

        mnt = real_mount(target_path.mnt);
        sb = mnt->mnt.mnt_sb;
        ns = current->nsproxy->mnt_ns;
        parent = mnt->mnt_parent;

        if (mnt->mnt.mnt_root != target_path.dentry) {
            pr_err("终末地: gmo |%s|不是挂载点，普通目录喵\n", argv[1]);
            path_put(&target_path);
            return;
        }

        pr_info("终末地: =============== gmo 挂载点信息 ===============\n");
        pr_info("终末地: base |mnt=0x%px|sb=0x%px|ns=0x%px|mnt_flags=0x%08x|doomed=%s喵\n",
                mnt, sb, ns, mnt->mnt.mnt_flags,
                (mnt->mnt.mnt_flags & MNT_DOOMED) ? "yes" : "no");

        /* 命名空间链表校验 */
        if (!ns) {
            pr_err("终末地: gmo |当前进程无mnt_ns喵\n");
            path_put(&target_path);
            return;
        }
        down_read(&namespace_sem);
        list_for_each_entry(iter, &ns->list, mnt_list) {
            ns_mount_count++;
            if (iter == mnt)
                found_in_ns = 1;
        }
        up_read(&namespace_sem);
        pr_info("终末地: ns_check |ns_total=%d|found_in_ns=%s喵\n",
                ns->mounts, found_in_ns ? "yes" : "no");
        if (!found_in_ns) {
            pr_warn("终末地: warn |mnt=0x%px|不在命名空间链表，僵尸挂载，建议fumo清理喵\n", mnt);
            valid = 0;
        }

        /* VFS hlist/mount 链表指针，合并单行 | 分隔 */
        pr_info("终末地: vfs_ptr |mnt_hash.next=0x%px|mnt_hash.pprev=0x%px|"
                "mnt_list.next=0x%px|mnt_list.prev=0x%px|"
                "mnt_mounts.next=0x%px|mnt_mounts.prev=0x%px|"
                "mnt_child.next=0x%px|mnt_child.prev=0x%px喵\n",
                mnt->mnt_hash.next, mnt->mnt_hash.pprev,
                mnt->mnt_list.next, mnt->mnt_list.prev,
                mnt->mnt_mounts.next, mnt->mnt_mounts.prev,
                mnt->mnt_child.next, mnt->mnt_child.prev);

        /* hlist 反向校验单独警告输出 */
        if (mnt->mnt_hash.pprev) {
            if (*(mnt->mnt_hash.pprev) != &mnt->mnt_hash) {
                pr_warn("终末地: warn |hlist反向校验失败喵\n");
                valid = 0;
            }
        } else {
            pr_info("终末地: info |hlist pprev=NULL未初始化喵\n");
        }

        /* 父子挂载信息合并单行 */
        {
            char *p_name = "(unknown)";
            char *mp_name = "(null)";
            char *root_name = "(null)";
            void *mp_inode = NULL;

            if (parent && parent != mnt && parent->mnt_mountpoint &&
                parent->mnt_mountpoint->d_name.name)
                p_name = (char *)parent->mnt_mountpoint->d_name.name;
            if (mnt->mnt_mountpoint) {
                mp_name = (char *)mnt->mnt_mountpoint->d_name.name;
                mp_inode = mnt->mnt_mountpoint->d_inode;
            }
            if (mnt->mnt.mnt_root && mnt->mnt.mnt_root->d_name.name)
                root_name = (char *)mnt->mnt.mnt_root->d_name.name;

            pr_info("终末地: mount_tree |parent=0x%px|p_name=%s|mpoint=0x%px|mp_name=%s|"
                    "mp_inode=0x%px|mnt_root=0x%px|root_name=%s喵\n",
                    parent, p_name,
                    mnt->mnt_mountpoint, mp_name, mp_inode,
                    mnt->mnt.mnt_root, root_name);
        }

        /* 子挂载列表，保留逐行打印（变长列表，不适合压单行） */
        pr_info("终末地: --- 子挂载点列表 ---\n");
        list_for_each_entry(child, &mnt->mnt_mounts, mnt_child) {
            child_count++;
            if (child->mnt_mountpoint) {
                pr_info("终末地: child[%d] |0x%px|path=%s喵\n",
                        child_count, child, child->mnt_mountpoint->d_name.name);
            } else {
                pr_info("终末地: child[%d] |0x%px|path=(null)喵\n", child_count, child);
            }
        }
        if (child_count == 0)
            pr_info("终末地: child |无子挂载点喵\n");

        /* super_block 全部字段合并一行 */
        if (sb) {
            pr_info("终末地: sb_info |sb=0x%px|fstype=%s|s_magic=0x%lx|s_flags=0x%lx|"
                    "blk_size=%lu|blk_bits=%u|s_maxbytes=0x%llx|"
                    "s_count=%u|s_active=%d|ro_remount=%d|frozen=%d|bdev=0x%px|s_root=0x%px喵\n",
                    sb, sb->s_type ? sb->s_type->name : "(null)",
                    sb->s_magic, sb->s_flags,
                    sb->s_blocksize, sb->s_blocksize_bits, sb->s_maxbytes,
                    sb->s_count, atomic_read(&sb->s_active),
                    sb->s_readonly_remount, sb->s_writers.frozen,
                    sb->s_bdev, sb->s_root);
        } else {
            pr_err("终末地: sb_info |sb=NULL，无效超级块喵\n");
            valid = 0;
        }

        /* mnt_flags 标志位压缩打印，不拆多行 */
        pr_info("终末地: flag_bits |MNT_RDONLY=%s|MNT_NOEXEC=%s|MNT_NOSUID=%s|"
                "MNT_NODEV=%s|MNT_DOOMED=%s|MNT_SYNC_UMOUNT=%s喵\n",
                (mnt->mnt.mnt_flags & MNT_READONLY) ? "Y" : "N",
                (mnt->mnt.mnt_flags & MNT_NOEXEC) ? "Y" : "N",
                (mnt->mnt.mnt_flags & MNT_NOSUID) ? "Y" : "N",
                (mnt->mnt.mnt_flags & MNT_NODEV) ? "Y" : "N",
                (mnt->mnt.mnt_flags & MNT_DOOMED) ? "Y" : "N",
                (mnt->mnt.mnt_flags & MNT_SYNC_UMOUNT) ? "Y" : "N");

        /* 完整性检查，保留全部告警逻辑 */
        if (!mnt->mnt.mnt_sb) {
            pr_warn("终末地: check_warn |mnt_sb=NULL 可能损坏喵\n");
            valid = 0;
        }
        if (!mnt->mnt.mnt_root) {
            pr_warn("终末地: check_warn |mnt_root=NULL 可能损坏喵\n");
            valid = 0;
        }
        if (mnt->mnt.mnt_flags & MNT_DOOMED) {
            pr_warn("终末地: check_warn |MNT_DOOMED置位，即将销毁喵\n");
        }
        if (sb && atomic_read(&sb->s_active) == 0 && sb->s_count == 0) {
            pr_warn("终末地: check_warn |sb_active=0 s_count=0，可能已卸载喵\n");
            valid = 0;
        }
        if (!found_in_ns) {
            pr_warn("终末地: check_warn |不在命名空间链表喵\n");
            valid = 0;
        }

        pr_info("终末地: check_result |valid=%s|gmo完成喵\n", valid ? "OK" : "FAIL");
        pr_info("终末地: ==============================================\n");

        path_put(&target_path);
        return;
    }

    /* ========== lsmnt：列出全部挂载点 ========== */
    if (!strcmp(argv[0], "lsmnt")) {
        struct mnt_namespace *ns;
        struct mount *mnt;
        struct super_block *sb;
        int idx = 0;

        ns = current->nsproxy->mnt_ns;
        if (!ns) {
            pr_err("终末地: lsmnt 获取mnt_ns失败喵\n");
            return;
        }

        pr_info("终末地: ============ lsmnt 全部挂载点喵 ============\n");
        pr_info("终末地: ns=0x%px 总挂载计数:%d\n", ns, ns->mounts);
        pr_info("终末地: %-3s | %-24s | %-12s | mnt_flags | DOOMED | sb-ro | fstype\n",
                "IDX", "挂载点", "根目录");
        pr_info("------------------------------------------------------------------------\n");

        down_read(&namespace_sem);
        list_for_each_entry(mnt, &ns->list, mnt_list) {
            char *mp_name = "(null)";
            char *root_name = "(null)";
            sb = mnt->mnt.mnt_sb;

            /* 获取挂载点目录名 */
            if (mnt->mnt_mountpoint && mnt->mnt_mountpoint->d_name.name)
                mp_name = (char *)mnt->mnt_mountpoint->d_name.name;
            /* 获取该挂载的根目录名 */
            if (mnt->mnt.mnt_root && mnt->mnt.mnt_root->d_name.name)
                root_name = (char *)mnt->mnt.mnt_root->d_name.name;

            pr_info("终末地: %-3d | %-24s | %-12s | 0x%08x | %-6s | %-6s | %s\n",
                    idx,
                    mp_name,
                    root_name,
                    mnt->mnt.mnt_flags,
                    (mnt->mnt.mnt_flags & MNT_DOOMED) ? "YES" : "NO",
                    (sb && (sb->s_flags & SB_RDONLY)) ? "YES" : "NO",
                    sb ? sb->s_type->name : "null");
            idx++;
        }
        up_read(&namespace_sem);
        pr_info("终末地: ============ lsmnt 输出完成，共 %d 个挂载喵 ============\n", idx);
        return;
    }

    /* ========== dpanic 命令（开/关 panic 拦截） ========== */
    if (strstr(cmdstr, "dpanic")) {
        if (!strcmp(cmdstr, "dpanic1")) {
            panic_block = true;
            pr_info("[终末地 dpanic]  开启panic拦截\n");
        } else if (!strcmp(cmdstr, "dpanic0")) {
            panic_block = false;
            pr_info("[终末地 dpanic]  关闭panic拦截\n");
        } else {
            pr_info("[终末地] dpanic 命令：仅支持 dpanic0 / dpanic1\n");
        }
        return;
    }
    
/* ========== mkneg：创建负PID常驻内核线程 ========== */
if (!strcmp(argv[0], "mkneg")) {
    pid_t want_pid;
    int ret;
    struct pid *neg_pid;
    struct task_struct *tsk;
    struct pid_namespace *ns;

    if (argc < 2) {
        pr_info("终末地: 用法 mkneg <负pid> 例 mkneg -1喵\n");
        return;
    }
    ret = kstrtoint(argv[1], 0, &want_pid);
    if (ret != 0) {
        pr_err("终末地: mkneg 解析pid失败喵, arg='%s'\n", argv[1]);
        return;
    }
    if (want_pid >= 0) {
        pr_err("终末地: mkneg 只允许传入负数pid喵\n");
        return;
    }

    ns = task_active_pid_ns(current);
    neg_pid = alloc_neg_pid(ns);
    if (!neg_pid) {
        pr_err("终末地: mkneg alloc_neg_pid 分配负pid失败喵\n");
        return;
    }

    tsk = kthread_create(mkneg_worker, NULL, "mkneg_task");
    if (IS_ERR(tsk)) {
        pr_err("终末地: mkneg kthread_create失败喵\n");
        put_pid(neg_pid);
        return;
    }

    write_lock_irq(&tasklist_lock);
    change_pid(tsk, PIDTYPE_PID, neg_pid);
    write_unlock_irq(&tasklist_lock);

    wake_up_process(tsk);
    pr_info("终末地: mkneg 创建完成，task=%p 分配得到vpid=%d comm=mkneg_task喵\n",
             tsk, pid_nr_ns(neg_pid, ns));
    put_pid(neg_pid);
    return;
}
    /* ========== panicnow：手动触发 panic ========== */
    /* ✅ panicnow 必须放在 parse_dev_cmd 的大括号内部！！！ */
    if (!strcmp(cmdstr, "panicnow")) {
        panic("终末地：手动触发panic测试");
        return;
    }
}

/* ========================== file_operations ========================== */

static ssize_t v2p_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    static const char help[] =
        "当前工具版本:3.6 | Image内嵌版本 | 当前能用的命令如下\n"
        "update_log           输出功能更新说明\n"
        "gcap                  # 查看当前进程的 Capabilities 喵\n"
        "guid                  # 查看当前进程的 UID/GID 喵\n"
        "gmo <挂载点>          # 查看挂载点完整内核状态喵\n"
        "lsmnt                  # 列出命名空间下全部挂载点喵\n"
        "pinfo <虚拟地址>      # 解析页表，显示物理地址、权限、page结构喵\n"
        "kps [-i] [-z] [-k] [-u]  # 内核态进程查看器 (i=idle, z=zombie, k=kthread, u=user)喵\n"
        "sgd <uid> <gid> <euid> <egid>              # 修改当前进程 uid 等喵\n"
        "sud_pid <pid> <uid> <gid> <euid> <egid>    # 修改指定进程 uid 等喵\n"
        "scap <perm> <inh> <eff> <bset> <amb>       # 修改当前进程 cap 喵\n"
        "scap_pid <pid> <perm> <inh> <eff> <bset> <amb>  # 修改指定进程 cap 喵\n"
        "终末地: mem 内存操作命令喵\n"
        "mem -vp <va>        # 虚拟地址转物理地址\n"
        "      -vr <va> <len>  # 读取内核虚拟地址\n"
        "      -vw <va> <len> <hex> # 写入内核虚拟地址\n"
        "      -pr <pa> <len>  # 读取物理内存\n"
        "      -pw <pa> <len> <hex> # 写入物理内存\n"
        "      -gvir <va>      # 读取PTE页表权限标志\n"
        "      -svir <va> <rwx># 修改PTE权限，例 rw-- / r‑x‑\n"
        "mflg <rw|ro> <exec|noexec>   # 修改挂载标志喵\n"
        "fumo <挂载点>            # 强制卸载文件系统喵\n"
        "     ├── -k 杀死占用进程后销毁喵\n"
        "     └── -f 无视进程强制销毁super_block喵\n"
        "kc <文件路径>            # 强行创建文件喵\n"
        "kpid <pid>           # 强制抹杀进程喵\n"
        "fpid <pid>           # 冻结进程喵（暂停执行）\n"
        "tpid <pid>           # 解冻进程喵（恢复执行）\n"
        "scom <pid> <名称>    # 修改进程名喵（ps/top 显示假名）\n"
        "gpi <pid>            # 获取进程完整内核信息喵（详细喵）\n"
        "fsb <目录>           # 强制同步文件系统状态喵\n"
        "mkneg <负pid>         # 创建负PID常驻内核线程 例 mkneg -1喵\n"
        "call <func_name> [arg0 arg1 ... argN] # 调用内核导出函数喵\n"
        "dpanic1 / dpanic0 # 开/关阻止panic模式\n"
        "panicnow # 可以使系统立刻panic崩溃\n"

    size_t len = strlen(help);
    if (*ppos >= len)
        return 0;
    if (count > len - *ppos)
        count = len - *ppos;
    if (copy_to_user(buf, help + *ppos, count))
        return -EFAULT;
    *ppos += count;
    return count;
}

static ssize_t v2p_dev_write(struct file *file, const char __user *buf,
                             size_t count, loff_t *ppos)
{
    char *kbuf;

    pr_info("终末地: 写入命令触发喵\n");
    if (count >= DEV_BUF_SZ)
        return -EINVAL;

    kbuf = kmalloc(count + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    parse_dev_cmd(kbuf);
    kfree(kbuf);
    *ppos += count;
    return count;
}

/* ========================== ioctl ========================== */

static long v2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct v2p_req req;
    struct v2p_rw_req rwreq;
    struct cap_req creq;
    struct uidgid_req ugreq;
    const struct cred *old_cred;
    struct cred *new_cred;
    struct page *page;
    struct v2p_phys_rw prw;
    void __iomem *io_mem;

    switch (cmd) {
    case V2P_IOCTL_VA2PA:
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        page = virt_to_page((void *)req.va);
        req.pa = page_to_phys(page);
        pr_info("ioctl va2pa 虚拟=0x%llx 物理=0x%llx 喵\n", req.va, req.pa);
        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        break;

    case V2P_IOCTL_VA_READ:
        if (copy_from_user(&rwreq, (void __user *)arg, sizeof(rwreq)))
            return -EFAULT;
        if (rwreq.len > sizeof(rwreq.buf))
            return -EINVAL;
        memcpy(rwreq.buf, (void *)rwreq.va, rwreq.len);
        pr_info("[虚拟地址读取] 0x%llx len=%u 喵]\n", rwreq.va, rwreq.len);
        if (copy_to_user((void __user *)arg, &rwreq, sizeof(rwreq)))
            return -EFAULT;
        break;

    case V2P_IOCTL_VA_WRITE:
        if (copy_from_user(&rwreq, (void __user *)arg, sizeof(rwreq)))
            return -EFAULT;
        if (rwreq.len > sizeof(rwreq.buf))
            return -EINVAL;
        memcpy((void *)rwreq.va, rwreq.buf, rwreq.len);
        pr_info("[虚拟地址写入] 0x%llx len=%u 写入完成喵]\n", rwreq.va, rwreq.len);
        break;

    case V2P_IOCTL_GET_CAP:
        old_cred = current_cred();
        creq.cap_perm = cap_to_u64(&old_cred->cap_permitted);
        creq.cap_inh  = cap_to_u64(&old_cred->cap_inheritable);
        if (copy_to_user((void __user *)arg, &creq, sizeof(creq)))
            return -EFAULT;
        break;

    case V2P_IOCTL_SET_CAP:
        if (copy_from_user(&creq, (void __user *)arg, sizeof(creq)))
            return -EFAULT;
        new_cred = prepare_creds();
        if (!new_cred)
            return -ENOMEM;
        cap_from_u64(&new_cred->cap_permitted, creq.cap_perm);
        cap_from_u64(&new_cred->cap_inheritable, creq.cap_inh);
        commit_creds(new_cred);
        break;

    case V2P_IOCTL_GET_UIDGID:
        old_cred = current_cred();
        ugreq.uid  = old_cred->uid.val;
        ugreq.gid  = old_cred->gid.val;
        ugreq.euid = old_cred->euid.val;
        ugreq.egid = old_cred->egid.val;
        if (copy_to_user((void __user *)arg, &ugreq, sizeof(ugreq)))
            return -EFAULT;
        break;

    case V2P_IOCTL_SET_UIDGID:
        if (copy_from_user(&ugreq, (void __user *)arg, sizeof(ugreq)))
            return -EFAULT;
        new_cred = prepare_creds();
        if (!new_cred)
            return -ENOMEM;
        new_cred->uid  = make_kuid(current_user_ns(), ugreq.uid);
        new_cred->gid  = make_kgid(current_user_ns(), ugreq.gid);
        new_cred->euid = make_kuid(current_user_ns(), ugreq.euid);
        new_cred->egid = make_kgid(current_user_ns(), ugreq.egid);
        commit_creds(new_cred);
        break;

    case V2P_IOCTL_SET_MNTFLG:
        v2p_mnt_flags_op(MNT_NOEXEC | MNT_NOSUID | MNT_NODEV, 0);
        break;

    case V2P_IOCTL_PHYS_READ:
        if (copy_from_user(&prw, (void __user *)arg, sizeof(prw)))
            return -EFAULT;
        if (prw.len > 4096)
            return -EINVAL;
        io_mem = ioremap(prw.phys_addr, prw.len);
        if (!io_mem)
            return -EINVAL;
        if (copy_to_user((void __user *)prw.user_buf, io_mem, prw.len)) {
            iounmap(io_mem);
            return -EFAULT;
        }
        iounmap(io_mem);
        break;

    case V2P_IOCTL_PHYS_WRITE:
        if (copy_from_user(&prw, (void __user *)arg, sizeof(prw)))
            return -EFAULT;
        if (prw.len > 4096)
            return -EINVAL;
        io_mem = ioremap(prw.phys_addr, prw.len);
        if (!io_mem)
            return -EINVAL;
        if (copy_from_user(io_mem, (void __user *)prw.user_buf, prw.len)) {
            iounmap(io_mem);
            return -EFAULT;
        }
        iounmap(io_mem);
        break;

    default:
        return -ENOTTY;
    }
    return 0;
}

/* ========================== 标准字符设备驱动 ========================== */

static int v2p_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int v2p_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations v2p_fops = {
    .owner          = THIS_MODULE,
    .open           = v2p_open,
    .release        = v2p_release,
    .read           = v2p_dev_read,
    .write          = v2p_dev_write,
    .unlocked_ioctl = v2p_ioctl,
};

/* ========================== 内置初始化 ========================== */

int __init end_builtin_init(void)
{
    struct path rootfs_parent;
    struct filename *fname;
    struct dentry *end_dir_dentry;
    struct path mnt_point_path; /* 给 do_new_mount 用 */
    int ret;

    if (g_end_builtin_done)
        return 0;
    g_end_builtin_done = 1;

    rootfs_parent = init_task.fs->root;
    fname = getname_kernel("/end");
    end_dir_dentry = kern_path_create(AT_FDCWD, fname->name, &rootfs_parent,
                                      LOOKUP_CREATE | LOOKUP_DIRECTORY);
    if (IS_ERR(end_dir_dentry)) {
        ret = PTR_ERR(end_dir_dentry);
        pr_err("终末地: 创建 /end 目录失败 ret=%d\n", ret);
        putname(fname);
        return ret;
    }
    done_path_create(&rootfs_parent, end_dir_dentry);
    putname(fname);

    ret = kern_path("/end", LOOKUP_FOLLOW, &mnt_point_path);
    if (ret) {
        pr_err("终末地: kern_path /end 失败 %d\n", ret);
        return ret;
    }

    /* 现在 mnt_point_path 是有效的，传给 do_new_mount */
    ret = do_new_mount(&mnt_point_path, "ramfs", MS_NOATIME, 0, "ramfs", NULL);
    path_put(&mnt_point_path); /* 使用完释放引用！ */

    if (ret != 0) {
        pr_err("终末地: do_new_mount /end ramfs 失败 ret=%d\n", ret);
        return ret;
    }
    pr_info("终末地: ramfs挂载到/end成功\n");

    /* 再继续创建 /end/ctrl */
    /* ...后面创建ctrl节点逻辑... */
    return 0;
}

/* ========================== 模块加载/卸载 ========================== */

static int __init v2p_init(void)
{
    int err;
    int kp_ret;

    pr_info("终末地: 初始化状态...\n");

    err = alloc_chrdev_region(&v2p_devno, 0, 1, "v2p");
    if (err < 0) {
        pr_err("终末地: 分配字符设备失败 %d 喵\n", err);
        return err;
    }
    pr_info("终末地: 分配字符设备成功 major=%d minor=%d 喵\n",
            MAJOR(v2p_devno), MINOR(v2p_devno));

    cdev_init(&v2p_cdev, &v2p_fops);
    v2p_cdev.owner = THIS_MODULE;
    err = cdev_add(&v2p_cdev, v2p_devno, 1);
    if (err) {
        pr_err("终末地: cdev添加失败喵 %d\n", err);
        goto err_chrdev;
    }
    pr_info("终末地: cdev成功添加喵\n");

    v2p_class = class_create(THIS_MODULE, "v2p");
    if (IS_ERR(v2p_class)) {
        err = PTR_ERR(v2p_class);
        pr_err("终末地: class_create失败，错误代码=%d 喵\n", err);
        goto err_cdev;
    }
    pr_info("终末地: class_create完成喵\n");

    v2p_dev = device_create(v2p_class, NULL, v2p_devno, NULL, "end");
    if (IS_ERR(v2p_dev)) {
        err = PTR_ERR(v2p_dev);
        pr_err("v2p: 设备节点创建失败，错误代码=%d 喵\n", err);
        goto err_class;
    }
    pr_info("终末地: /dev/end设备节点创建成功喵\n");

    create_both_v2p_node();

    kp_ret = end_dpanic_kprobe_init();
    if (kp_ret == 0) {
        pr_info("[终末地 dpanic]  kprobe 注册成功，挂钩panic函数\n");
    } else {
        pr_err("[终末地 dpanic]  kprobe注册失败！ret=%d\n", kp_ret);
    }
    pr_info("终末地: 双挂载状态节点创建完成喵\n");

    return 0;

err_class:
    class_destroy(v2p_class);
err_cdev:
    cdev_del(&v2p_cdev);
err_chrdev:
    unregister_chrdev_region(v2p_devno, 1);
    return err;
}

/* 内置编译：替换 module_init(v2p_init) */
fs_initcall(v2p_init);

late_initcall(end_builtin_init);