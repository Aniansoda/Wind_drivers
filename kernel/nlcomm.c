/*
 * nlcomm.c — netlink 私有通道
 *
 * 无设备节点, 无 inode/fops 改写, 无 kprobe hook。内核侧只做三件事:
 *   netlink_kernel_create   注册私有协议号
 *   input 回调              在发送进程上下文里执行 wind_dispatch_ioctl
 *   netlink_unicast         把 result 回给发送方
 *
 * 因为内核 socket 的 input 回调是在用户 sendmsg 的 syscall 上下文里同步调用的,
 * current 就是调用者, 所以 arg 里的用户指针可以直接 copy_from_user,
 * 语义与 ioctl 第三参数完全一致, 上层 dispatch 一行都不用改。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netlink.h>
#include <net/netlink.h>
#include <linux/skbuff.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <net/sock.h>
#include <net/net_namespace.h>

#include "comm.h"
#include "sym.h"
#include "nlcomm.h"

/* 候选协议号: 从高往低试, 23..31 在内核里没有固定用途。
 * NETLINK_GENERIC(16)/KOBJECT_UEVENT(15)/SELINUX(7) 等全部避开。 */
#define WIND_NL_UNIT_MIN 23
#define WIND_NL_UNIT_MAX 31

static struct sock *g_nl_sk;
static int g_nl_unit = -1;

/* netlink_kernel_cfg.cb_mutex 不填会在 unicast 里出现空锁, 必须给一个 */
static DEFINE_MUTEX(g_nl_cb_mutex);

/* 6.6 起 netlink_kernel_cfg.input 返回 void */
static void wind_nl_input(struct sk_buff *skb)
{
	const struct nlmsghdr *nlh;
	const WIND_NL_MSG *req;
	WIND_NL_MSG *reply_msg;
	struct nlmsghdr *rnlh;
	struct sk_buff *rskb;
	u32 portid;
	long ret;

	if (unlikely(!skb || skb->len < NLMSG_HDRLEN + sizeof(WIND_NL_MSG)))
		return;

	nlh = nlmsg_hdr(skb);
	if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(WIND_NL_MSG))
		return;

	req = (const WIND_NL_MSG *)NLMSG_DATA(nlh);
	if (req->magic != WIND_NL_MAGIC)
		return;

	portid = NETLINK_CB(skb).portid;

	/* 在发送进程上下文里跑, arg 是用户指针, dispatch 里自己 copy_from/to_user */
	ret = wind_dispatch_ioctl(req->op, (unsigned long)req->arg);

	rskb = wind_alloc_skb(NLMSG_SPACE(sizeof(WIND_NL_MSG)), GFP_ATOMIC);
	if (!rskb)
		return;

	rnlh = nlmsg_put(rskb, 0, 0, 0, sizeof(WIND_NL_MSG), 0);
	if (!rnlh) /* alloc 尺寸已算够, 实际不会发生; 不为此引入 kfree_skb 依赖 */
		return;

	reply_msg = (WIND_NL_MSG *)NLMSG_DATA(rnlh);
	reply_msg->magic = WIND_NL_MAGIC;
	reply_msg->op = req->op;
	reply_msg->arg = req->arg;
	reply_msg->result = ret;

	/* 发送方已退出时这里返回 -ECONNREFUSED, 忽略即可 */
	(void)wind_netlink_unicast(g_nl_sk, rskb, portid, MSG_DONTWAIT);
}

int wind_nl_install(void)
{
	struct netlink_kernel_cfg cfg;
	int unit, ret = -EOPNOTSUPP;

	if (g_nl_sk)
		return 0;

	if (!wind_netlink_available()) {
		pr_err("wind: netlink 符号未解析到, 无法建立通道\n");
		return -EOPNOTSUPP;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.input = wind_nl_input;
	cfg.cb_mutex = &g_nl_cb_mutex;

	for (unit = WIND_NL_UNIT_MAX; unit >= WIND_NL_UNIT_MIN; unit--) {
		g_nl_sk = wind_netlink_kernel_create(&init_net, unit, &cfg);
		if (IS_ERR(g_nl_sk)) {
			ret = (int)PTR_ERR(g_nl_sk);
			g_nl_sk = NULL;
			continue;
		}
		if (g_nl_sk) {
			g_nl_unit = unit;
			pr_info("wind: netlink 通道就绪 unit=%d (无设备节点) by 川上富江Tomie\n", unit);
			return 0;
		}
		ret = -EBUSY;
	}

	pr_err("wind: netlink 协议号 %d..%d 全被占用\n",
	       WIND_NL_UNIT_MIN, WIND_NL_UNIT_MAX);
	g_nl_sk = NULL;
	return ret;
}

void wind_nl_remove(void)
{
	if (!g_nl_sk)
		return;

	wind_netlink_kernel_release(g_nl_sk);
	g_nl_sk = NULL;
	pr_info("wind: netlink 通道已关闭 (unit=%d)\n", g_nl_unit);
	g_nl_unit = -1;
}

int wind_nl_unit(void)
{
	return g_nl_unit;
}
