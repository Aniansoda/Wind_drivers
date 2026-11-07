#ifndef _WIND_NLCOMM_H_
#define _WIND_NLCOMM_H_

/*
 * nlcomm.h — netlink 私有通道 (无节点, 无 hook)
 *
 * 内核注册一个私有 AF_NETLINK 协议号; 用户态 socket(AF_NETLINK, SOCK_RAW, unit)
 * 发一条 WIND_NL_MSG, 内核在同一个发送进程上下文里执行 dispatch, 再回一条
 * 同结构消息填 result。
 *
 * 相比改 /dev/null 的 inode->i_fop:
 *   - 不写任何内核对象 (inode / fops / 页表都不碰)
 *   - /dev 与 /proc 下都没有节点
 *   - 不需要 chrdev_open 语义, 不依赖 devtmpfs
 * 唯一暴露点是 /proc/net/netlink 里多一条 proto 记录 (需 root 才能读)。
 */

#include <linux/types.h>

/* 由 entry.c 提供: 与 ioctl 完全相同的分发入口 */
long wind_dispatch_ioctl(unsigned int cmd, unsigned long arg);

int wind_nl_install(void);
void wind_nl_remove(void);
int wind_nl_unit(void); /* 当前使用的 netlink 协议号, 未装为 -1 */

#endif /* _WIND_NLCOMM_H_ */
