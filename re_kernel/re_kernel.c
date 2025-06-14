/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 bmax121. All Rights Reserved.
 */

/*   SPDX-License-Identifier: GPL-3.0-only   */
/*
 * Copyright (C) 2024 Nep-Timeline. All Rights Reserved.
 * Copyright (C) 2024 lzghzr. All Rights Reserved.
 */

#include <compiler.h>
#include <hook.h>
#include <kpmodule.h>
#include <kputils.h>
#include <taskext.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/atomic.h>

#ifdef CONFIG_DEBUG
#include <uapi/linux/limits.h>
#endif /* CONFIG_DEBUG */

#include "re_kernel.h"
#include "re_utils.h"

KPM_NAME("re_kernel");
KPM_VERSION(MYKPM_VERSION);
KPM_LICENSE("GPL v3");
KPM_AUTHOR("Nep-Timeline, lzghzr");
KPM_DESCRIPTION("Re:Kernel, support 4.4 ~ 6.1");

#define NETLINK_REKERNEL_MAX 26
#define NETLINK_REKERNEL_MIN 22
#define USER_PORT 100
#define PACKET_SIZE 256
#define MIN_USERAPP_UID 10000
#define MAX_SYSTEM_UID 2000
#define PARCEL_OFFSET 16
#define INTERFACETOKEN_BUFF_SIZE 140

enum report_type {
  BINDER,
  SIGNAL,
#ifdef CONFIG_NETWORK
  NETWORK,
#endif /* CONFIG_NETWORK */
};
enum binder_type {
  REPLY,
  TRANSACTION,
  OVERFLOW,
};
static const char* binder_type[] = {
    "reply",
    "transaction",
    "free_buffer_full",
};

#define IZERO (1UL << 0x10)
#define UZERO (1UL << 0x20)

// cgroup_freezing, cgroupv1_freeze
static bool (*cgroup_freezing)(struct task_struct* task);
// send_netlink_message
struct sk_buff* kfunc_def(__alloc_skb)(unsigned int size, gfp_t gfp_mask, int flags, int node);
struct nlmsghdr* kfunc_def(__nlmsg_put)(struct sk_buff* skb, u32 portid, u32 seq, int type, int len, int flags);
void kfunc_def(kfree_skb)(struct sk_buff* skb);
int kfunc_def(netlink_unicast)(struct sock* ssk, struct sk_buff* skb, u32 portid, int nonblock);
// netlink_rcv
int kfunc_def(netlink_rcv_skb)(struct sk_buff* skb, int (*cb)(struct sk_buff*, struct nlmsghdr*, struct netlink_ext_ack*));
// start_rekernel_server
static struct net kvar_def(init_net);
struct sock* kfunc_def(__netlink_kernel_create)(struct net* net, int unit, struct module* module, struct netlink_kernel_cfg* cfg);
void kfunc_def(netlink_kernel_release)(struct sock* sk);
// prco
struct proc_dir_entry* kfunc_def(proc_mkdir)(const char* name, struct proc_dir_entry* parent);
struct proc_dir_entry* kfunc_def(proc_create_data)(const char* name, umode_t mode, struct proc_dir_entry* parent, const struct file_operations* proc_fops, void* data);
void kfunc_def(proc_remove)(struct proc_dir_entry* de);
// hook binder_proc_transaction
static int (*binder_proc_transaction)(struct binder_transaction* t, struct binder_proc* proc, struct binder_thread* thread);
// free the outdated transaction and buffer
static void (*binder_transaction_buffer_release)(struct binder_proc* proc, struct binder_thread* thread, struct binder_buffer* buffer, binder_size_t off_end_offset, bool is_failure);
static void (*binder_transaction_buffer_release_v6)(struct binder_proc* proc, struct binder_thread* thread, struct binder_buffer* buffer, binder_size_t failed_at, bool is_failure);
static void (*binder_transaction_buffer_release_v4)(struct binder_proc* proc, struct binder_buffer* buffer, binder_size_t failed_at, bool is_failure);
static void (*binder_transaction_buffer_release_v3)(struct binder_proc* proc, struct binder_buffer* buffer, binder_size_t* failed_at);
static void (*binder_alloc_free_buf)(struct binder_alloc* alloc, struct binder_buffer* buffer);
void kfunc_def(kfree)(const void* objp);
// hook do_send_sig_info
static int (*do_send_sig_info)(int sig, struct siginfo* info, struct task_struct* p, enum pid_type type);
// hook binder_transaction
static void (*binder_transaction)(struct binder_proc* proc, struct binder_thread* thread, struct binder_transaction_data* tr, int reply, binder_size_t extra_buffers_size);
// copy_from_user
void* kfunc_def(memdup_user)(const void __user* src, size_t len);
void kfunc_def(kvfree)(const void* addr);

#ifdef CONFIG_NETWORK
// netfilter
kuid_t kfunc_def(sock_i_uid)(struct sock* sk);
// hook tcp_rcv
static int (*tcp_v4_rcv)(struct sk_buff* skb);
static int (*tcp_v6_rcv)(struct sk_buff* skb);
#endif /* CONFIG_NETWORK */

// _raw_spin_lock && _raw_spin_unlock
void kfunc_def(_raw_spin_lock)(raw_spinlock_t* lock);
void kfunc_def(_raw_spin_unlock)(raw_spinlock_t* lock);
// trace
int kfunc_def(tracepoint_probe_register)(struct tracepoint* tp, void* probe, void* data);
int kfunc_def(tracepoint_probe_unregister)(struct tracepoint* tp, void* probe, void* data);
// trace_binder_transaction
struct tracepoint kvar_def(__tracepoint_binder_transaction);
#ifdef CONFIG_DEBUG_CMDLINE
int kfunc_def(get_cmdline)(struct task_struct* task, char* buffer, int buflen);
#endif /* CONFIG_DEBUG_CMDLINE */

struct struct_offset struct_offset = {};
// 最好初始化一个大于 0xFFFFFFFF 的值, 否则编译器优化后, 全局变量可能出错
static uint64_t binder_stats_deleted_addr = UZERO,
// 实际上会被编译器优化为 bool
binder_transaction_buffer_release_ver6 = UZERO, binder_transaction_buffer_release_ver5 = UZERO, binder_transaction_buffer_release_ver4 = UZERO;

static unsigned long trace = UZERO, ext_tr_offset = UZERO;
#include "re_offsets.c"

// binder_node_lock
static inline void binder_node_lock(struct binder_node* node) {
  spinlock_t* node_lock = binder_node_lock_ptr(node);
  spin_lock(node_lock);
}
// binder_node_unlock
static inline void binder_node_unlock(struct binder_node* node) {
  spinlock_t* node_lock = binder_node_lock_ptr(node);
  spin_unlock(node_lock);
}
// binder_inner_proc_lock
static inline void binder_inner_proc_lock(struct binder_proc* proc) {
  spinlock_t* inner_lock = binder_proc_inner_lock(proc);
  spin_lock(inner_lock);
}
// binder_inner_proc_unlock
static inline void binder_inner_proc_unlock(struct binder_proc* proc) {
  spinlock_t* inner_lock = binder_proc_inner_lock(proc);
  spin_unlock(inner_lock);
}

// binder_is_frozen
static inline bool binder_is_frozen(struct binder_proc* proc) {
  bool is_frozen = false;
  if (struct_offset.binder_proc_is_frozen) {
    is_frozen = binder_proc_is_frozen(proc);
  }
  return is_frozen;
}

// cgroupv2_freeze
static inline bool jobctl_frozen(struct task_struct* task) {
  unsigned long jobctl = task_jobctl(task);
  return ((jobctl & JOBCTL_TRAP_FREEZE) != 0);
}
// 判断线程是否进入 frozen 状态
static inline bool frozen_task_group(struct task_struct* task) {
  return (jobctl_frozen(task) || cgroup_freezing(task));
}

// netlink
int netlink_count = 0;
static struct sock* rekernel_netlink;
static unsigned long rekernel_netlink_unit = UZERO;
static struct proc_dir_entry* rekernel_dir, * rekernel_unit_entry;
static const struct file_operations rekernel_unit_fops = {};
// 发送 netlink 消息
static int send_netlink_message(char* msg) {
  int len = strlen(msg);
  struct sk_buff* skbuffer;
  struct nlmsghdr* nlhdr;

  skbuffer = nlmsg_new(len, GFP_ATOMIC);
  if (!skbuffer) {
    logkm("netlink alloc failure.\n");
    return -ENOMEM;
  }

  nlhdr = nlmsg_put(skbuffer, 0, 0, rekernel_netlink_unit, len, 0);
  if (!nlhdr) {
    logkm("nlmsg_put failaure.\n");
    nlmsg_free(skbuffer);
    return -EMSGSIZE;
  }

  memcpy(nlmsg_data(nlhdr), msg, len);
  return netlink_unicast(rekernel_netlink, skbuffer, USER_PORT, MSG_DONTWAIT);
}
// 接收 netlink 消息
static int netlink_rcv_msg(struct sk_buff* skb, struct nlmsghdr* nlh, struct netlink_ext_ack* extack) {
  char* umsg = nlmsg_data(nlh);
  if (!umsg)
    return -EINVAL;

  netlink_count++;
  char netlink_kmsg[PACKET_SIZE];
  snprintf(netlink_kmsg, sizeof(netlink_kmsg), "Successfully received data packet! %d", netlink_count);
  logkm("kernel recv packet from user: %s\n", umsg);
  return send_netlink_message(netlink_kmsg);
}
static void netlink_rcv(struct sk_buff* skb) {
  netlink_rcv_skb(skb, &netlink_rcv_msg);
}
// 创建 netlink 服务
static int start_rekernel_server(void) {
  if (rekernel_netlink_unit != UZERO)
    return 0;
  struct netlink_kernel_cfg rekernel_cfg = {
    .input = netlink_rcv,
  };

  for (rekernel_netlink_unit = NETLINK_REKERNEL_MAX; rekernel_netlink_unit >= NETLINK_REKERNEL_MIN; rekernel_netlink_unit--) {
    rekernel_netlink = netlink_kernel_create(kvar(init_net), rekernel_netlink_unit, &rekernel_cfg);
    if (rekernel_netlink != NULL)
      break;
  }
  if (rekernel_netlink == NULL) {
    logkm("Failed to create Re:Kernel server!\n");
    return -ENOBUFS;
  }
  logkm("Created Re:Kernel server! NETLINK UNIT: %d\n", rekernel_netlink_unit);

  rekernel_dir = proc_mkdir("rekernel", NULL);
  if (!rekernel_dir) {
    logkm("create /proc/rekernel failed!\n");
  } else {
    char buff[32];
    sprintf(buff, "%d", rekernel_netlink_unit);
    rekernel_unit_entry = proc_create(buff, 0400, rekernel_dir, &rekernel_unit_fops);
    if (!rekernel_unit_entry) {
      logkm("create rekernel unit failed!\n");
    }
  }

  return 0;
}

static void rekernel_report(int reporttype, int type, pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (start_rekernel_server() != 0)
    return;

#ifdef CONFIG_NETWORK
  if (reporttype == NETWORK) {
    char binder_kmsg[PACKET_SIZE];
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Network,target=%d;", dst_pid);
#ifdef CONFIG_DEBUG
    logkm("%s\n", binder_kmsg);
#endif /* CONFIG_DEBUG */
    send_netlink_message(binder_kmsg);
    return;
  }
#endif /* CONFIG_NETWORK */

  if (!frozen_task_group(dst))
    return;

  if (task_uid(src).val == task_uid(dst).val)
    return;

  char binder_kmsg[PACKET_SIZE];
  switch (reporttype) {
  case BINDER:
    if (oneway && type == TRANSACTION) {
      if (ext_tr_offset == UZERO)
        return;
      struct task_ext* ext = get_task_ext(current);
      struct binder_transaction_data* tr = *(void**)task_local_ptr(ext, ext_tr_offset);
      if (!tr)
        return;
      // 减少异步消息
      if (tr->code < 29 || tr->code > 32)
        return;

      size_t buf_data_size = tr->data_size > INTERFACETOKEN_BUFF_SIZE ? INTERFACETOKEN_BUFF_SIZE : tr->data_size;
      char* buf_data = memdup_user((char*)tr->data.ptr.buffer, buf_data_size);
      if (IS_ERR(buf_data))
        return;
      char buf[INTERFACETOKEN_BUFF_SIZE] = { 0 };
      int i = 0;
      int j = PARCEL_OFFSET + 1;
      char* p = buf_data + PARCEL_OFFSET;
      while (i < INTERFACETOKEN_BUFF_SIZE && j < buf_data_size && *p != '\0') {
        buf[i++] = *p;
        j += 2;
        p += 2;
      }
      kvfree(buf_data);
      if (i == INTERFACETOKEN_BUFF_SIZE) {
        buf[i - 1] = '\0';
      }
      snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d,rpc_name=%s,code=%d;", binder_type[type], oneway, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val, buf, tr->code);
    } else {
      snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d;", binder_type[type], oneway, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
    }
    break;
  case SIGNAL:
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Signal,signal=%d,killer_pid=%d,killer=%d,dst_pid=%d,dst=%d;", type, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
    break;
  default:
    return;
  }
#ifdef CONFIG_DEBUG
  logkm("%s\n", binder_kmsg);
  logkm("src_comm=%s,dst_comm=%s\n", get_task_comm(src), get_task_comm(dst));
#endif /* CONFIG_DEBUG */
#ifdef CONFIG_DEBUG_CMDLINE
  char src_cmdline[PATH_MAX], dst_cmdline[PATH_MAX];
  memset(&src_cmdline, 0, PATH_MAX);
  memset(&dst_cmdline, 0, PATH_MAX);
  int res = 0;
  res = get_cmdline(src, src_cmdline, PATH_MAX - 1);
  src_cmdline[res] = '\0';
  res = get_cmdline(dst, dst_cmdline, PATH_MAX - 1);
  dst_cmdline[res] = '\0';
  logkm("src_cmdline=%s,dst_cmdline=%s\n", src_cmdline, dst_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */
  send_netlink_message(binder_kmsg);
}

static void binder_reply_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;
  if (task_uid(dst).val > MAX_SYSTEM_UID || src_pid == dst_pid)
    return;

  // oneway=0
  rekernel_report(BINDER, REPLY, src_pid, src, dst_pid, dst, oneway);
}

static void binder_trans_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;
  if ((task_uid(dst).val <= MIN_USERAPP_UID) || src_pid == dst_pid)
    return;

  rekernel_report(BINDER, TRANSACTION, src_pid, src, dst_pid, dst, oneway);
}

static void binder_overflow_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;

  // oneway=1
  rekernel_report(BINDER, OVERFLOW, src_pid, src, dst_pid, dst, oneway);
}

static void rekernel_binder_transaction(void* data, bool reply, struct binder_transaction* t, struct binder_node* target_node) {
  struct binder_proc* to_proc = binder_transaction_to_proc(t);
  if (!to_proc)
    return;
  struct binder_thread* from = binder_transaction_from(t);

  if (reply) {
    binder_reply_handler(task_pid(current), current, to_proc->pid, to_proc->tsk, false);
  } else if (from) {
    if (from->proc) {
      binder_trans_handler(from->proc->pid, from->proc->tsk, to_proc->pid, to_proc->tsk, false);
    }
  } else { // oneway=1
    binder_trans_handler(task_pid(current), current, to_proc->pid, to_proc->tsk, true);

    struct binder_alloc* target_alloc = binder_proc_alloc(to_proc);
    size_t free_async_space = binder_alloc_free_async_space(target_alloc);
    size_t buffer_size = binder_alloc_buffer_size(target_alloc);
    if (free_async_space < (buffer_size / 10 + 0x300)) {
      binder_overflow_handler(task_pid(current), current, to_proc->pid, to_proc->tsk, true);
    }
  }
}

static bool binder_can_update_transaction(struct binder_transaction* t1, struct binder_transaction* t2) {
  struct binder_proc* t1_to_proc = binder_transaction_to_proc(t1);
  struct binder_buffer* t1_buffer = binder_transaction_buffer(t1);
  unsigned int t1_code = binder_transaction_code(t1);
  unsigned int t1_flags = binder_transaction_flags(t1);
  binder_uintptr_t t1_ptr = binder_node_ptr(t1_buffer->target_node);
  binder_uintptr_t t1_cookie = binder_node_cookie(t1_buffer->target_node);

  struct binder_proc* t2_to_proc = binder_transaction_to_proc(t2);
  struct binder_buffer* t2_buffer = binder_transaction_buffer(t2);
  unsigned int t2_code = binder_transaction_code(t2);
  unsigned int t2_flags = binder_transaction_flags(t2);
  binder_uintptr_t t2_ptr = binder_node_ptr(t2_buffer->target_node);
  binder_uintptr_t t2_cookie = binder_node_cookie(t2_buffer->target_node);

  if ((t1_flags & t2_flags & TF_ONE_WAY) != TF_ONE_WAY || !t1_to_proc || !t2_to_proc)
    return false;
  if (t1_to_proc->tsk == t2_to_proc->tsk
    && t1_code == t2_code
    && t1_flags == t2_flags
    && (struct_offset.binder_proc_is_frozen ? t1_buffer->pid == t2_buffer->pid : true) // 4.19 以下无此数据
    && t1_ptr == t2_ptr
    && t1_cookie == t2_cookie)
    return true;
  return false;
}

static struct binder_transaction* binder_find_outdated_transaction_ilocked(struct binder_transaction* t, struct list_head* target_list) {
  struct binder_work* w;
  bool second = false;

  list_for_each_entry(w, target_list, entry) {
    if (w->type != BINDER_WORK_TRANSACTION)
      continue;
    struct binder_transaction* t_queued = container_of(w, struct binder_transaction, work);
    if (binder_can_update_transaction(t_queued, t)) {
      if (second)
        return t_queued;
      else {
        second = true;
      }
    }
  }
  return NULL;
}

static inline void outstanding_txns_dec(struct binder_proc* proc) {
  if (struct_offset.binder_proc_outstanding_txns) {
    int* outstanding_txns = binder_proc_outstanding_txns(proc);
    (*outstanding_txns)--;
  }
}

static inline void binder_release_entire_buffer(struct binder_proc* proc, struct binder_thread* thread, struct binder_buffer* buffer, bool is_failure) {
  if (binder_transaction_buffer_release_ver6 == IZERO) {
    binder_transaction_buffer_release_v6(proc, thread, buffer, 0, is_failure);
  } else if (binder_transaction_buffer_release_ver5 == IZERO) {
    binder_size_t off_end_offset = ALIGN(buffer->data_size, sizeof(void*));
    off_end_offset += buffer->offsets_size;

    binder_transaction_buffer_release(proc, thread, buffer, off_end_offset, is_failure);
  } else if (binder_transaction_buffer_release_ver4 == IZERO) {
    binder_transaction_buffer_release_v4(proc, buffer, 0, is_failure);
  } else {
    binder_transaction_buffer_release_v3(proc, buffer, NULL);
  }
}

static inline void binder_stats_deleted(enum binder_stat_types type) {
  atomic_inc((atomic_t*)binder_stats_deleted_addr);
}

static void binder_proc_transaction_before(hook_fargs3_t* args, void* udata) {
  struct binder_transaction* t = (struct binder_transaction*)args->arg0;
  struct binder_proc* proc = (struct binder_proc*)args->arg1;

  struct binder_buffer* buffer = binder_transaction_buffer(t);
  struct binder_node* node = buffer->target_node;
  // 兼容不支持 trace 的内核
  if (trace == UZERO) {
    rekernel_binder_transaction(NULL, false, t, NULL);
  }
  unsigned int flags = binder_transaction_flags(t);
  if (!node || !(flags & TF_ONE_WAY))
    return;

  // binder 冻结时不再清理过时消息
  if (binder_is_frozen(proc) || !frozen_task_group(proc->tsk))
    return;

  binder_node_lock(node);
  bool has_async_transaction = binder_node_has_async_transaction(node);
  if (!has_async_transaction) {
    binder_node_unlock(node);
    return;
  }
  binder_inner_proc_lock(proc);

  struct list_head* async_todo = binder_node_async_todo(node);
  struct binder_transaction* t_outdated = binder_find_outdated_transaction_ilocked(t, async_todo);
  if (t_outdated) {
    list_del_init(&t_outdated->work.entry);
    outstanding_txns_dec(proc);
  }

  binder_inner_proc_unlock(proc);
  binder_node_unlock(node);

  if (t_outdated) {
    struct binder_alloc* target_alloc = binder_proc_alloc(proc);
    struct binder_buffer* buffer = binder_transaction_buffer(t_outdated);
#ifdef CONFIG_DEBUG
    logkm("free_outdated pid=%d,uid=%d,data_size=%d\n", proc->pid, task_uid(proc->tsk).val, buffer->data_size);
#endif /* CONFIG_DEBUG */

    * (struct binder_buffer**)((uintptr_t)t_outdated + struct_offset.binder_transaction_buffer) = NULL;
    buffer->transaction = NULL;
    binder_release_entire_buffer(proc, NULL, buffer, false);
    binder_alloc_free_buf(target_alloc, buffer);
    kfree(t_outdated);
    binder_stats_deleted(BINDER_STAT_TRANSACTION);
  }
}

static void binder_transaction_before(hook_fargs5_t* args, void* udata) {
  struct task_ext* ext = get_task_ext(current);
  if (!task_ext_valid(ext))
    return;
  if (ext_tr_offset == UZERO) {
    // reg_task_local 似乎有bug
    // ext_tr_offset = reg_task_local(sizeof(uint64_t));
    ext_tr_offset = task_ext_size + sizeof(uintptr_t);
    // 随缘兼容其他模块
    while (*(uintptr_t**)task_local_ptr(ext, ext_tr_offset)) {
      ext_tr_offset += sizeof(uintptr_t);
    }
  }
  *(uintptr_t*)task_local_ptr(ext, ext_tr_offset) = args->arg2;
}

static void do_send_sig_info_before(hook_fargs4_t* args, void* udata) {
  int sig = (int)args->arg0;
  struct task_struct* dst = (struct task_struct*)args->arg2;

  if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGQUIT) {
    rekernel_report(SIGNAL, sig, task_tgid(current), current, task_tgid(dst), dst, false);
  }
}

#ifdef CONFIG_NETWORK
static inline bool sk_fullsock(const struct sock* sk) {
  return (1 << sk->sk_state) & ~(TCPF_TIME_WAIT | TCPF_NEW_SYN_RECV);
}

static void tcp_rcv_before(hook_fargs1_t* args, void* udata) {
  struct sk_buff* skb = (struct sk_buff*)args->arg0;
  struct sock* sk = skb->sk;;
  if (sk == NULL || !sk_fullsock(sk))
    return;

  uid_t uid = sock_i_uid(sk).val;
  if (uid < MIN_USERAPP_UID)
    return;

  rekernel_report(NETWORK, NULL, NULL, NULL, uid, NULL, true);
}
#endif /* CONFIG_NETWORK */

static uint64_t calculate_imm(uint32_t inst, enum inst_type inst_type, uint64_t inst_addr) {
  if (inst_addr && inst_type == ARM64_ADRP) {
    uint64_t immlo = bits32(inst, 30, 29);
    uint64_t immhi = bits32(inst, 23, 5);
    return (inst_addr + sign64_extend((immhi << 14u) | (immlo << 12u), 33u)) & 0xFFFFFFFFFFFFF000;
  }
  uint64_t imm12 = bits32(inst, 21, 10);
  switch (inst_type) {
  case ARM64_ADD_64:
    if (bit(inst, 22))
      return sign64_extend((imm12 << 12u), 16u);
    else
      return sign64_extend((imm12), 16u);
  case ARM64_STRB:
    return sign64_extend((imm12), 16u);
  case ARM64_STR_32:
  case ARM64_LDR_32:
    return sign64_extend((imm12 << 0b10u), 16u);
  case ARM64_STR_64:
  case ARM64_LDR_64:
    return sign64_extend((imm12 << 0b11u), 16u);
  default:
    return UZERO;
  }
}

static long calculate_offsets() {
  // 获取 binder_transaction_buffer_release 版本, 以参数数量做判断
  uint32_t* binder_transaction_buffer_release_src = (uint32_t*)binder_transaction_buffer_release;
  for (u32 i = 0; i < 0x100; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_transaction_buffer_release %x %llx\n", i, binder_transaction_buffer_release_src[i]);
#endif /* CONFIG_DEBUG */
    if (i < 0x10) {
      if ((binder_transaction_buffer_release_src[i] & MASK_STR_Rt_4) == INST_STR_Rt_4
        || (binder_transaction_buffer_release_src[i] & MASK_MOV_Rm_4_Rn_WZR) == INST_MOV_Rm_4_Rn_WZR
        || (binder_transaction_buffer_release_src[i] & MASK_UXTB_Rn_4) == INST_UXTB_Rn_4) {
        binder_transaction_buffer_release_ver5 = IZERO;
      } else if ((binder_transaction_buffer_release_src[i] & MASK_STR_Rt_3) == INST_STR_Rt_3
        || (binder_transaction_buffer_release_src[i] & MASK_MOV_Rm_3_Rn_WZR) == INST_MOV_Rm_3_Rn_WZR
        || (binder_transaction_buffer_release_src[i] & MASK_UXTB_Rn_3) == INST_UXTB_Rn_3) {
        binder_transaction_buffer_release_ver4 = IZERO;
      }
    } else if (binder_transaction_buffer_release_ver5 == UZERO) {
      break;
    } else if ((binder_transaction_buffer_release_src[i] & MASK_AND_64_imm_0XFFFFFFFFFFFFFFF8) == INST_AND_64_imm_0XFFFFFFFFFFFFFFF8) {
      for (u32 j = 1; j < 0x3; j++) {
        if ((binder_transaction_buffer_release_src[i + j] & MASK_CBZ) == INST_CBZ
          || (binder_transaction_buffer_release_src[i + j] & MASK_TBNZ) == INST_TBNZ) {
          binder_transaction_buffer_release_ver6 = IZERO;
          break;
        }
      }
      break;
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_transaction_buffer_release_ver6=0x%llx\n", binder_transaction_buffer_release_ver6);
  logkm("binder_transaction_buffer_release_ver5=0x%llx\n", binder_transaction_buffer_release_ver5);
  logkm("binder_transaction_buffer_release_ver4=0x%llx\n", binder_transaction_buffer_release_ver4);
#endif /* CONFIG_DEBUG */
  // 获取 binder_proc->is_frozen, 没有就是不支持
  uint32_t* binder_proc_transaction_src = (uint32_t*)binder_proc_transaction;
  for (u32 i = 0; i < 0x70; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_proc_transaction %x %llx\n", i, binder_proc_transaction_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_proc_transaction_src[i] == ARM64_RET) {
      break;
    } else if (!struct_offset.binder_node_has_async_transaction && (binder_proc_transaction_src[i] & MASK_STRB) == INST_STRB) {
      uint64_t offset = calculate_imm(binder_proc_transaction_src[i], ARM64_STRB, NULL);
      if (offset < 0x6B || offset > 0x7B)
        continue;
      struct_offset.binder_node_has_async_transaction = offset;
      struct_offset.binder_node_ptr = offset - 0x13;
      struct_offset.binder_node_cookie = offset - 0xB;
      struct_offset.binder_node_async_todo = offset + 0x5;
      // 目前只有 harmony 内核需要特殊设置
      if (offset == 0x7B) {
        struct_offset.binder_node_lock = 0x8;
        struct_offset.binder_transaction_from = 0x28;
      } else {
        struct_offset.binder_node_lock = 0x4;
        struct_offset.binder_transaction_from = 0x20;
      }
    } else if (!struct_offset.binder_transaction_buffer && (binder_proc_transaction_src[i] & MASK_LDR_64_Rn_X0) == INST_LDR_64_Rn_X0) {
      struct_offset.binder_transaction_buffer = calculate_imm(binder_proc_transaction_src[i], ARM64_LDR_64, NULL);
      struct_offset.binder_transaction_to_proc = struct_offset.binder_transaction_buffer - 0x20;
      struct_offset.binder_transaction_code = struct_offset.binder_transaction_buffer + 0x8;
      struct_offset.binder_transaction_flags = struct_offset.binder_transaction_buffer + 0xC;
    } else if ((binder_proc_transaction_src[i] & MASK_ORR) == INST_ORR && (binder_proc_transaction_src[i + 1] & MASK_STRB) == INST_STRB) {
      uint64_t binder_proc_sync_recv_offset = calculate_imm(binder_proc_transaction_src[i + 1], ARM64_STRB, NULL);
      struct_offset.binder_proc_is_frozen = binder_proc_sync_recv_offset - 1;
      struct_offset.binder_proc_outstanding_txns = binder_proc_sync_recv_offset - 0x6;
      break;
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_transaction_from=0x%x\n", struct_offset.binder_transaction_from); // 0x20
  logkm("binder_transaction_to_proc=0x%x\n", struct_offset.binder_transaction_to_proc); // 0x30
  logkm("binder_transaction_buffer=0x%x\n", struct_offset.binder_transaction_buffer); // 0x50
  logkm("binder_transaction_code=0x%x\n", struct_offset.binder_transaction_code); // 0x58
  logkm("binder_transaction_flags=0x%x\n", struct_offset.binder_transaction_flags); // 0x5C
  logkm("binder_node_lock=0x%x\n", struct_offset.binder_node_lock); // 0x4
  logkm("binder_node_ptr=0x%x\n", struct_offset.binder_node_ptr); // 0x58
  logkm("binder_node_cookie=0x%x\n", struct_offset.binder_node_cookie); // 0x60
  logkm("binder_node_has_async_transaction=0x%x\n", struct_offset.binder_node_has_async_transaction); // 0x6B
  logkm("binder_node_async_todo=0x%x\n", struct_offset.binder_node_async_todo); // 0x70
  logkm("binder_proc_outstanding_txns=0x%x\n", struct_offset.binder_proc_outstanding_txns); // 0x6C
  logkm("binder_proc_is_frozen=0x%x\n", struct_offset.binder_proc_is_frozen); // 0x71
#endif /* CONFIG_DEBUG */
  if (!struct_offset.binder_node_lock || !struct_offset.binder_node_has_async_transaction || !struct_offset.binder_transaction_buffer)
    return -11;

  // 获取 task_struct->jobctl
  void (*task_clear_jobctl_trapping)(struct task_struct* t);
  lookup_name(task_clear_jobctl_trapping);

  uint32_t* task_clear_jobctl_trapping_src = (uint32_t*)task_clear_jobctl_trapping;
  for (u32 i = 0; i < 0x10; i++) {
#ifdef CONFIG_DEBUG
    logkm("task_clear_jobctl_trapping %x %llx\n", i, task_clear_jobctl_trapping_src[i]);
#endif /* CONFIG_DEBUG */
    if (task_clear_jobctl_trapping_src[i] == ARM64_RET) {
      break;
    } else if ((task_clear_jobctl_trapping_src[i] & MASK_LDR_64_Rn_X0) == INST_LDR_64_Rn_X0) {
      struct_offset.task_struct_jobctl = calculate_imm(task_clear_jobctl_trapping_src[i], ARM64_LDR_64, NULL);
      break;
    }
  }
#ifdef CONFIG_DEBUG
  logkm("task_struct_jobctl=0x%x\n", struct_offset.task_struct_jobctl); // 0x580
#endif /* CONFIG_DEBUG */
  if (!struct_offset.task_struct_jobctl)
    return -11;

  // 获取 binder_proc->context, binder_proc->inner_lock, binder_proc->outer_lock
  uint32_t* binder_transaction_src = (uint32_t*)binder_transaction;
  for (u32 i = 0; i < 0x20; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_transaction %x %llx\n", i, binder_transaction_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_transaction_src[i] == ARM64_RET) {
      break;
    } else if (((binder_transaction_src[i] & MASK_LDR_64_) == INST_LDR_64_)) {
      uint64_t offset = calculate_imm(binder_transaction_src[i], ARM64_LDR_64, NULL);
      if (offset < 0x200 || offset > 0x300)
        continue;
      struct_offset.binder_proc_context = offset;
      struct_offset.binder_proc_inner_lock = offset + 0x8;
      struct_offset.binder_proc_outer_lock = offset + 0xC;
      break;
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_proc_context=0x%x\n", struct_offset.binder_proc_context); // 0x240
  logkm("binder_proc_inner_lock=0x%x\n", struct_offset.binder_proc_inner_lock); // 0x248
  logkm("binder_proc_outer_lock=0x%x\n", struct_offset.binder_proc_outer_lock); // 0x24C
#endif /* CONFIG_DEBUG */
  if (!struct_offset.binder_proc_context)
    return -11;

  // 获取 binder_proc->alloc
  void (*binder_free_proc)(struct binder_proc* proc);
  binder_free_proc = (typeof(binder_free_proc))kallsyms_lookup_name("binder_free_proc");
  if (!binder_free_proc) {
    binder_free_proc = (typeof(binder_free_proc))kallsyms_lookup_name("binder_proc_dec_tmpref");
  }
  pr_info("kernel function binder_free_proc addr: %llx\n", binder_free_proc);
  if (!binder_free_proc)
    return -21;

  uint32_t* binder_free_proc_src = (uint32_t*)binder_free_proc;
  for (u32 i = 0x10; i < 0x100; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_free_proc %x %llx\n", i, binder_free_proc_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_free_proc_src[i] == ARM64_MOV_x29_SP) {
      break;
    } else if ((binder_free_proc_src[i] & MASK_ADD_64_Rd_X0_Rn_X19) == INST_ADD_64_Rd_X0_Rn_X19 && (binder_free_proc_src[i + 1] & MASK_BL) == INST_BL) {
      struct_offset.binder_proc_alloc = calculate_imm(binder_free_proc_src[i], ARM64_ADD_64, NULL);
      if (struct_offset.binder_proc_alloc > struct_offset.binder_proc_context) {
        continue;
      }
      break;
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_proc_alloc=0x%x\n", struct_offset.binder_proc_alloc); // 0x1A8
#endif /* CONFIG_DEBUG */
  if (!struct_offset.binder_proc_alloc)
    return -11;

  // 获取 binder_alloc->pid, task_struct->pid, task_struct->group_leader
  void (*binder_alloc_init)(struct task_struct* t);
  lookup_name(binder_alloc_init);

  uint32_t* binder_alloc_init_src = (uint32_t*)binder_alloc_init;
  for (u32 i = 0; i < 0x20; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_alloc_init %x %llx\n", i, binder_alloc_init_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_alloc_init_src[i] == ARM64_RET) {
      for (u32 j = 1; j < 0x10; j++) {
        if ((binder_alloc_init_src[i - j] & MASK_ADD_64) == INST_ADD_64) {
          uint64_t binder_alloc_buffers_offset = calculate_imm(binder_alloc_init_src[i - j], ARM64_ADD_64, NULL);
          struct_offset.binder_alloc_buffer = binder_alloc_buffers_offset - 0x8;
          struct_offset.binder_alloc_free_async_space = binder_alloc_buffers_offset + 0x20;
          struct_offset.binder_alloc_buffer_size = binder_alloc_buffers_offset + 0x30;
          break;
        }
      }
      break;
    } else if (!struct_offset.binder_alloc_pid && (binder_alloc_init_src[i] & MASK_STR_32_x0) == INST_STR_32_x0) {
      struct_offset.binder_alloc_pid = calculate_imm(binder_alloc_init_src[i], ARM64_STR_32, NULL);
    } else if (!struct_offset.binder_alloc_pid && (binder_alloc_init_src[i] & MASK_LDR_32_) == INST_LDR_32_) {
      struct_offset.task_struct_pid = calculate_imm(binder_alloc_init_src[i], ARM64_LDR_32, NULL);
      struct_offset.task_struct_tgid = struct_offset.task_struct_pid + 0x4;
    } else if (!struct_offset.binder_alloc_pid && (binder_alloc_init_src[i] & MASK_LDR_64_) == INST_LDR_64_) {
      struct_offset.task_struct_group_leader = calculate_imm(binder_alloc_init_src[i], ARM64_LDR_64, NULL);
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_alloc_pid=0x%x\n", struct_offset.binder_alloc_pid); // 0x84
  logkm("binder_alloc_buffer_size=0x%x\n", struct_offset.binder_alloc_buffer_size); // 0x78
  logkm("binder_alloc_free_async_space=0x%x\n", struct_offset.binder_alloc_free_async_space); // 0x68
  logkm("binder_alloc_buffer=0x%x\n", struct_offset.binder_alloc_buffer); // 0x40
  logkm("task_struct_pid=0x%x\n", struct_offset.task_struct_pid); // 0x5D8
  logkm("task_struct_tgid=0x%x\n", struct_offset.task_struct_tgid); // 0x5DC
  logkm("task_struct_group_leader=0x%x\n", struct_offset.task_struct_group_leader); // 0x618
#endif /* CONFIG_DEBUG */
  if (!struct_offset.binder_alloc_pid || !struct_offset.task_struct_pid || !struct_offset.task_struct_group_leader)
    return -11;

  // 获取 binder_stats_deleted_addr
  struct binder_stats kvar_def(binder_stats);
  kvar_lookup_name(binder_stats);
  void (*binder_free_transaction)(struct binder_transaction* t);
  binder_free_transaction = (typeof(binder_free_transaction))kallsyms_lookup_name("binder_free_transaction");
  if (!binder_free_transaction) {
    binder_free_transaction = (typeof(binder_free_transaction))kallsyms_lookup_name("binder_send_failed_reply");
  }
  pr_info("kernel function binder_free_transaction addr: %llx\n", binder_free_transaction);
  if (!binder_free_transaction)
    return -21;

  uint32_t* binder_free_transaction_src = (uint32_t*)binder_free_transaction;
  for (u32 i = 0; i < 0x100; i++) {
#ifdef CONFIG_DEBUG
    logkm("binder_free_transaction %x %llx\n", i, binder_free_transaction_src[i]);
#endif /* CONFIG_DEBUG */
    if ((binder_free_transaction_src[i] & MASK_ADRP) == INST_ADRP) {
      uint64_t inst_addr = (uint64_t)binder_free_transaction + i * 4;
      uint64_t adrp_addr = calculate_imm(binder_free_transaction_src[i], ARM64_ADRP, inst_addr);
      if (adrp_addr - ((uint64_t)kvar(binder_stats) & 0xFFFFFFFFFFFFF000) <= 0x1000) {
        uint64_t binder_stats_addr = (uint64_t)kvar(binder_stats) & 0xFFF;
        for (u32 j = 0; j < 0x10; j++) {
          if ((binder_free_transaction_src[i + j] & MASK_ADD_64) == INST_ADD_64) {
            uint64_t adrl_addr = calculate_imm(binder_free_transaction_src[i + j], ARM64_ADD_64, NULL);
            uint64_t deleted_offset = (adrl_addr - binder_stats_addr) & 0xFFF;
            if (deleted_offset == 0) {
              for (u32 k = 0; k < 0x10; k++) {
                if ((binder_free_transaction_src[i + j + k] & MASK_ADD_64) == INST_ADD_64) {
                  uint64_t offset = calculate_imm(binder_free_transaction_src[i + j + k], ARM64_ADD_64, NULL);
                  if (offset > 0xC0 && offset < 0xE0) {
                    binder_stats_deleted_addr = adrp_addr + adrl_addr + offset;
                    break;
                  }
                }
              }
            } else if (deleted_offset > 0xC0 && deleted_offset < 0xE0) {
              binder_stats_deleted_addr = adrp_addr + adrl_addr;
              break;
            }
          }
        }
        break;
      }
    }
  }
#ifdef CONFIG_DEBUG
  logkm("binder_stats=0x%llx\n", kvar(binder_stats));
  logkm("binder_stats_deleted_addr=0x%llx\n", binder_stats_deleted_addr); // binder_stats + 0xCC
#endif /* CONFIG_DEBUG */
  if (binder_stats_deleted_addr == UZERO)
    return -11;

  return 0;
}

static long inline_hook_init(const char* args, const char* event, void* __user reserved) {
  lookup_name(cgroup_freezing);

  kfunc_lookup_name(__alloc_skb);
  kfunc_lookup_name(__nlmsg_put);
  kfunc_lookup_name(kfree_skb);
  kfunc_lookup_name(netlink_unicast);
  kfunc_lookup_name(netlink_rcv_skb);

  kvar_lookup_name(init_net);
  kfunc_lookup_name(__netlink_kernel_create);
  kfunc_lookup_name(netlink_kernel_release);

  kfunc_lookup_name(proc_mkdir);
  kfunc_lookup_name(proc_create_data);
  kfunc_lookup_name(proc_remove);

  kfunc_lookup_name(tracepoint_probe_register);
  kfunc_lookup_name(tracepoint_probe_unregister);

  kfunc_lookup_name(_raw_spin_lock);
  kfunc_lookup_name(_raw_spin_unlock);
  kvar_lookup_name(__tracepoint_binder_transaction);

  lookup_name(binder_transaction_buffer_release);
  binder_transaction_buffer_release_v6 = (typeof(binder_transaction_buffer_release_v6))binder_transaction_buffer_release;
  binder_transaction_buffer_release_v4 = (typeof(binder_transaction_buffer_release_v4))binder_transaction_buffer_release;
  binder_transaction_buffer_release_v3 = (typeof(binder_transaction_buffer_release_v3))binder_transaction_buffer_release;
  lookup_name(binder_alloc_free_buf);
  kfunc_lookup_name(kfree);
  kfunc_lookup_name(kvfree);
  kfunc_lookup_name(memdup_user);

  lookup_name(binder_proc_transaction);
  lookup_name(binder_transaction);
  lookup_name(do_send_sig_info);

#ifdef CONFIG_NETWORK
  kfunc_lookup_name(sock_i_uid);

  lookup_name(tcp_v4_rcv);
  lookup_name(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */
#ifdef CONFIG_DEBUG_CMDLINE
  kfunc_lookup_name(get_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */

  int rc = 0;
  rc = calculate_offsets();
  if (rc < 0)
    return rc;

  rc = tracepoint_probe_register(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);
  if (rc == 0) {
    trace = IZERO;
  }

  hook_func(binder_proc_transaction, 3, binder_proc_transaction_before, NULL, NULL);
  hook_func(binder_transaction, 5, binder_transaction_before, NULL, NULL);
  hook_func(do_send_sig_info, 4, do_send_sig_info_before, NULL, NULL);

#ifdef CONFIG_NETWORK
  hook_func(tcp_v4_rcv, 1, tcp_rcv_before, NULL, NULL);
  hook_func(tcp_v6_rcv, 1, tcp_rcv_before, NULL, NULL);
#endif /* CONFIG_NETWORK */

  return 0;
}

static long inline_hook_control0(const char* ctl_args, char* __user out_msg, int outlen) {
  char msg[64];
  snprintf(msg, sizeof(msg), "_(._.)_");
  compat_copy_to_user(out_msg, msg, sizeof(msg));
  return 0;
}

static long inline_hook_exit(void* __user reserved) {
  if (rekernel_netlink) {
    netlink_kernel_release(rekernel_netlink);
  }
  if (rekernel_dir) {
    proc_remove(rekernel_dir);
  }

  tracepoint_probe_unregister(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);

  unhook_func(binder_proc_transaction);
  unhook_func(binder_transaction);
  unhook_func(do_send_sig_info);

#ifdef CONFIG_NETWORK
  unhook_func(tcp_v4_rcv);
  unhook_func(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */

  return 0;
}

KPM_INIT(inline_hook_init);
KPM_CTL0(inline_hook_control0);
KPM_EXIT(inline_hook_exit);
