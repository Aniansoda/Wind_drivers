#ifndef _LOADER_H_
#define _LOADER_H_

/* 从内嵌载荷里挑出与当前内核匹配的一条, 解密后写到 dest。
 * 返回 0 成功; 负数见下面的错误码。 */
int wind_payload_extract(const char *dest);

#define WPE_OK          0
#define WPE_NO_PAYLOAD  (-1)   /* 编译进来的是空表 */
#define WPE_NO_MATCH    (-2)   /* 没有匹配当前内核的 payload */
#define WPE_BAD_B64     (-3)
#define WPE_IO          (-4)
#define WPE_BAD_SIZE    (-5)

#endif /* _LOADER_H_ */
