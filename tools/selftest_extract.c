/* CI 用: 在主机上验一遍 base64 -> ChaCha20 -> 落盘 是否还原得回去。
 * 配合 WIND_KVER_FORCE 指定内核版本, 因为主机 /proc/version 跟目标无关。 */
#include <stdio.h>
#include "loader.h"

int main(int argc, char **argv)
{
	const char *dest = (argc > 1) ? argv[1] : "/tmp/wind-selftest.ko";
	int rc = wind_payload_extract(dest);

	printf("extract rc=%d -> %s\n", rc, dest);
	return rc != 0;
}
