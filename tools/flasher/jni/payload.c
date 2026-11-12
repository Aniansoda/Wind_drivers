/* 占位: 仓库里这份只有空表, 让 AIDE 不开 CI 也能编译。
 * CI 会用 tools/mkpayload.py 覆盖成真实数据。 */
#include "payload.h"

const struct wind_payload wind_payloads[] = {
	{ 0, 0, 0, { 0 }, 0 }
};

const int wind_payload_cnt = 0;
