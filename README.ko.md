# Wind Driver — Android ARM64 커널 모듈 (wind.ko)

[中文](README.md) | [English](README.en.md) | [日本語](README.ja.md) | [Русский](README.ru.md) | **한국어**

Android / Linux ARM64 커널 공간 메모리 읽기·쓰기 및 디버깅 프레임워크
단일 모듈 `wind.ko`, 11개 서브시스템, 하나의 오퍼코드 체계, 세 가지 통신 경로
제로 CRC 방식, 다섯 브레이크포인트 계열 동시 공존, 화이트리스트 밖 심볼은 전부 실행 시 해석

---

## 저자

| 항목 | 정보 |
|---|---|
| 저자 | 阿念-川上富江Tomie |
| 감사 | 小黑 — JiagNight |
| 연락처 | xuboyuan763@gmail.com |
| 문제 보고 | issue 권장, 커널 버전·기기 모델·`dmesg` 마지막 200줄 첨부 |

저자 명의는 `阿念-川上富江Tomie` 하나뿐
본 프로젝트는 학습·연구 목적으로만 공개하며 상업적 또는 불법 사용을 상정하지 않음

---

## 지원 범위

| 커널 | 빌드 대상 | 상태 |
|---|---|---|
| 5.10.198 | wind.ko | 대응 완료, 제로 CRC 방식 |
| 5.15.149 | wind.ko | 대응 완료, 제로 CRC 방식 |
| 6.1 | wind.ko | 대응 완료, 제로 CRC 방식 |
| 6.6.66 | wind.ko | 실기 회귀 통과, 제로 CRC 방식 |
| 6.12.58 | wind.ko | 실기 config 정렬, 제로 CRC 방식 |
| 6.18.21 | wind.ko | 대응 완료, 제로 CRC 방식 |

아키텍처 aarch64 / arm64
형태 로더블 커널 모듈, `insmod` 로 적재하고 `rmmod` 로 해제
산출물 단일 `wind.ko`, 모든 서브시스템을 한 모듈에 통합, 의존 모듈 없음
소스 `kernel/` 8067줄, `.c` / `.h` 19개 파일

---

## 이번 버전 변경점

| 코드 | 이전 | 현재 |
|---|---|---|
| `0x26..0x2B` | ETM/PTM 분기 추적 | **WRP 데이터 관측점** `wrpbp` |
| `0x38..0x3D` | 예약 | 폐지, 정의하지 않음 |
| `0x33` | — | `OP_STATS` 캐시 통계 |
| `0x34 / 0x37` | — | `HWBP_SET_REGS` / `HWBP_REFRESH` |
| `0x35 / 0x36` | — | `PTEBP_SET_REGS` / `PTEBP_REFRESH` |
| `0x3E / 0x3F` | — | `BP_DIAG` / `BP_ALL_OFF` |
| 자이로 | 실행 브레이크포인트 상주 | 실행 브레이크포인트로 위치 파악 후 **WRP 정상 상태로 전환**, X/Y만 기록, Z는 창에 넣지 않음 |
| 심볼 | 직접 참조 | 화이트리스트 밖 32개를 `register_kprobe` 로 실행 시 해석 |
| 호환 | 코드 곳곳에 `#ifdef` | `compat.h` 로 일원화, 5.10 → 6.18 한 소스로 |

ETM 경로는 삭제했다. Coresight 는 SoC 가 클록과 funnel 을 열어야 하고 기기 커버리지가 낮으며 데이터 자체를 관측하지 못한다
`0x26..0x2B` 여섯 코드는 WRP 에 배정했다. 구조체 크기는 기존 `ETMBP_POINT_CONFIG` 와 같은 24바이트라 구 바이너리가 어긋나지 않는다

---

## 빠른 시작

```bash
# 1 빌드 (GitHub Actions 로 ko 생성)
#    Actions -> build-wind-ko -> 커널 선택 -> artifact 내려받기

# 2 적재
su
insmod wind.ko

# 3 먼저 로그 확인 (심볼 해석 결과가 여기에 출력됨)
dmesg | grep -i wind

# 4 해제
rmmod wind
```

산출물 경로 `kernel-src/drivers/wind/wind.ko`

적재 직후 기대되는 로그

```
wind: sym use_mm=ffffffffxxxxxxxx/ffffffffxxxxxxxx
wind: sym vmap=... filp=... hwbp=... misc=...
wind: netlink unit=31
```

`user/Wind-Drivers-6.6.ko` 에 6.6 용 빌드 결과물, `user/Wind-Drivers-*.sh` 에 6개 커널용 자동 압축 해제 설치본

---

## 통신 경로

세 경로가 같은 오퍼코드와 같은 디스패처 `wind_dispatch_ioctl` 을 공유한다
적재 시 `transport` 파라미터와 탐지 결과로 선택

| transport | 경로 | 원리 | 비고 |
|---|---|---|---|
| 3 | netlink 사설 프로토콜 | 사설 `AF_NETLINK` 유닛 등록, 사용자 공간은 `socket(AF_NETLINK, SOCK_RAW, 31)` 로 `WIND_NL_MSG` 전송 | 기본. `/dev` 와 `/proc` 에 노드를 만들지 않고 커널 객체를 건드리지 않음 |
| 2 | 사설 misc 문자 디바이스 | `misc_register` 로 등록 | `/dev/null` 과 rodata 를 건드리지 않아 호환성 최고, 대신 디바이스 노드 하나 추가 |
| 1 | `/dev/null` fops 우회 | `inode->i_fop` 를 모듈 내부 사본으로 지정 | 최후 수단. `allow_fops_hook=1` 필요, 일부 기기에서 멈춤·재부팅 기록 있음 |

netlink 상수

```
기본 유닛 31             WIND_NL_DEFAULT_UNIT
메시지 매직 0x574E4C31   WIND_NL_MAGIC  ("WNL1")
오퍼코드 베이스 0x574E0000 WIND_OP_BASE
수용 범위               WIND_OP_BASE + 0x01 .. WIND_OP_BASE + 0x3F
```

범위 밖 op 는 응답 없이 버려서 예상 밖 입력이 디스패처에서 깊게 역참조되지 않게 한다

### 모듈 파라미터

| 파라미터 | 형 | 기본 | 역할 |
|---|---|---|---|
| `transport` | int | 3 | `3` netlink, `2` misc, `1` /dev/null i_fop |
| `install_hook` | bool | 1 | 적재 시 통신 경로 hook 설치, `0` 은 심볼 해석만 |
| `allow_fops_hook` | bool | 0 | `/dev/null` 의 `inode->i_fop` 우회 허용 |
| `hook_level` | int | — | 이분 탐색용 `0` off, `1` fops 만, `2` fops + ioctl/mmap, `3` 원래 `.open` 유지 |

```bash
insmod wind.ko transport=3 install_hook=1
insmod wind.ko transport=0            # 통신 경로 없이 적재만, 진단용
```

---

## 오퍼코드 전체

상위 16비트는 `0x574E`(ASCII "WN") 고정, 하위 16비트가 일련번호
일반적인 `0x800` 계열 예제 드라이버와 충돌하지 않는다

### 세션과 메모리

| 코드 | 이름 | 인자 | 기능 |
|---|---|---|---|
| 0x01 | `OP_BIND` | pid | 대상 프로세스 바인딩, `task_struct` 캐시 |
| 0x02 | `OP_UNBIND` | 0 | 해제 및 캐시 반환 |
| 0x03 | `OP_READ_MEM` | `COPY_MEMORY*` | 단일 읽기 |
| 0x04 | `OP_WRITE_MEM` | `COPY_MEMORY*` | 단일 쓰기 |
| 0x05 | `OP_READ_BATCH` | `PBATCH_REQUEST` | 배치 읽기, 한 syscall 로 여러 주소 |
| 0x06 | `OP_WRITE_BATCH` | `PBATCH_REQUEST` | 배치 쓰기 |
| 0x07 | `OP_SET_MODE` | `MODE_REQUEST*` | 읽기 모드 FAST / SAFE 전환 |
| 0x08 | `OP_MODULE_BASE` | `MODULE_BASE*` | 대상 프로세스 내 모듈 로드 베이스 |
| 0x33 | `OP_STATS` | `wind_map_stats*` | 페이지 매핑·변환 캐시 통계 |

### 은닉

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x09 | `OP_HIDE_FILE` | basename 일치로 파일과 디렉터리 항목 은닉, 최대 16개 |
| 0x0A | `OP_HIDE_PROCESS` | 프로세스 디렉터리 항목 은닉, 최대 64개 pid |
| 0x0B | `OP_UNHIDE_ALL` | 모든 은닉 해제 |
| 0x0E | `OP_KGSL_HIDE` | kgsl 쪽에서 pid 은닉 |
| 0x0F | `OP_KGSL_HOOK` | kgsl hook 설치 |

### 하드웨어 브레이크포인트 BRP/WRP

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x0C | `OP_BRK_SET` | 구버전 단일 하드웨어 브레이크포인트 |
| 0x0D | `OP_BRK_CLEAR` | 구버전 단일 포인트 해제 |
| 0x14 | `OP_HWBP_SET` | 브레이크포인트·관측점 그룹 설정, 16 슬롯 |
| 0x15 | `OP_HWBP_REMOVE` | 전부 해제하고 debug 레지스터 반환 |
| 0x16 | `OP_HWBP_GET_CONFIG` | 현재 설정과 여유 BRP/WRP 수 |
| 0x17 | `OP_HWBP_GET_RECORDS` | 집계된 트리거 PC 분할 읽기 |
| 0x18 | `OP_HWBP_SET_RECORD` | 기록과 2비트 레지스터 마스크 되쓰기 |
| 0x19 | `OP_HWBP_CLEAR_RECORDS` | 기록 삭제, `-1` 은 전체 |
| 0x34 | `OP_HWBP_SET_REGS` | 트리거 시 `s3`/`s4`/`s5` 기록, `.inst` 원시 인코딩 |
| 0x37 | `OP_HWBP_REFRESH` | 주소 이동 후 재등록 |

### PTE 섀도 페이지 실행 브레이크포인트

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x1A | `OP_PTEBP_SET` | PTE 속성 반전으로 fault 유발, 16 슬롯, debug 레지스터 미사용 |
| 0x1B | `OP_PTEBP_REMOVE` | 전부 해제하고 PTE 복원 |
| 0x1C | `OP_PTEBP_GET_CONFIG` | 현재 설정 |
| 0x1D | `OP_PTEBP_GET_RECORDS` | 트리거 기록 분할 읽기 |
| 0x1E | `OP_PTEBP_SET_RECORD` | 기록 되쓰기 |
| 0x1F | `OP_PTEBP_CLEAR_RECORDS` | 기록 삭제, `-1` 은 전체 |
| 0x35 | `OP_PTEBP_SET_REGS` | 트리거 시 섀도 트램펄린을 통해 레지스터 기록 |
| 0x36 | `OP_PTEBP_REFRESH` | 섀도 페이지와 PTE 재정렬 |

### BRK 소프트웨어 브레이크포인트

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x20 | `OP_BRKBP_SET` | 대상 명령을 `BRK` 로 변경, 16 슬롯 |
| 0x21 | `OP_BRKBP_REMOVE` | 전부 해제하고 원래 명령 바이트 복원 |
| 0x22 | `OP_BRKBP_GET_CONFIG` | 현재 설정 |
| 0x23 | `OP_BRKBP_GET_RECORDS` | 트리거 기록 분할 읽기 |
| 0x24 | `OP_BRKBP_SET_RECORD` | 기록 되쓰기 |
| 0x25 | `OP_BRKBP_CLEAR_RECORDS` | 기록 삭제, `-1` 은 전체 |

### WRP 데이터 관측점

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x26 | `OP_WRPBP_SET` | 대상 VA 에 쓰기 관측점 설치, `LEN_8` 창 |
| 0x27 | `OP_WRPBP_REMOVE` | 해제하고 WRP 레지스터 반환 |
| 0x28 | `OP_WRPBP_GET_CONFIG` | 현재 창 설정 |
| 0x29 | `OP_WRPBP_GET_RECORDS` | 트리거 기록 분할 읽기 |
| 0x2A | `OP_WRPBP_SET_VEC` | 2비트 마스크로 벡터 되쓰기, X/Y 개별 지정 |
| 0x2B | `OP_WRPBP_CLEAR_RECORDS` | 기록 삭제, `-1` 은 전체 |

### UDP 브레이크포인트

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x2E | `OP_UDPBP_SET` | UDP 슬롯 설치·갱신, 8 슬롯 |
| 0x2F | `OP_UDPBP_REMOVE` | 슬롯 해제, `-1` 은 전체 |
| 0x30 | `OP_UDPBP_GET_CONFIG` | 슬롯 표 |
| 0x31 | `OP_UDPBP_GET_RECORDS` | 분할 읽기, 슬롯당 256건 |
| 0x32 | `OP_UDPBP_CLEAR_RECORDS` | 기록 삭제, `-1` 은 전체 |

### 자이로

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x10 | `OP_GYRO_INSTALL` | 대상 pid 에 자이로 hook 설치 |
| 0x11 | `OP_GYRO_REMOVE` | hook 해제 |
| 0x12 | `OP_GYRO_SET` | 목표 각속도 x/y/z (rad/s, IEEE 754 비트 패턴) |
| 0x13 | `OP_GYRO_ENABLE` | 기록 동작 켜기·끄기 |
| 0x2C | `OP_GYRO_STOP` | 기록 일시 정지, hook 유지 |
| 0x2D | `OP_GYRO_RESUME` | 기록 재개, 포인트가 사라졌으면 자동 재바인딩 |

### 진단과 전체 스위치

| 코드 | 이름 | 기능 |
|---|---|---|
| 0x3E | `OP_BP_DIAG` | 다섯 계열의 슬롯과 트리거 수를 한 번에 읽기 |
| 0x3F | `OP_BP_ALL_OFF` | 모든 계열 일괄 해제, 추적 방식 전환 전에 호출 |

`0x38..0x3D` 는 폐지. 클라이언트에서 자리를 잡지 말 것

---

## 내부 동작

### 1 세션

`open` 뒤 한 번 `OP_BIND` 를 호출하면 `task_struct` 참조가 `driver_session` 에 캐시된다
이후 접근은 `find_get_pid` + `get_pid_task` 를 거치지 않아 pid 해시 탐색 한 번과 참조 카운트 연산 두 번을 절약한다
`close` 시 `session_destroy` 가 참조를 해제
`mm` 은 캐시하지 않고 매번 `get_task_mm` 으로 얻으므로 `exec` 로 `mm` 이 바뀌어도 매달린 포인터가 남지 않는다

### 2 메모리 접근

**단일 읽기·쓰기** 한 번의 ioctl 로 연속 구간을 다루고 페이지 경계마다 변환

**배치** 한 번의 ioctl 에 N개 이산 주소를 실어 N번의 점 접근을 1회 syscall 로 합친다
`mm` 획득이 배치당 한 번이라 N쌍의 `mmgrab`/`mmput` 이 한 쌍이 된다. 다중 스레드 고빈도 폴링에서 CPU 사용과 잠금 경쟁이 눈에 띄게 줄어든다

**mmap 제로 카피** 대상의 물리 페이지를 `remap_pfn_range` 로 호출자 주소 공간에 매핑한 뒤 포인터로 직접 접근. ioctl 이 필요 없어 대량 샘플링에 적합

**두 가지 읽기 모드**

| 모드 | 구현 | 장점 | 단점 |
|---|---|---|---|
| `MODE_FAST` | `phys_to_virt` + `memcpy` | 빠르고 발열 낮음 | 캐시 부채널로 탐지될 수 있음 |
| `MODE_SAFE` | `ioremap_nocache` + `__flush_dcache_area` | 캐시 없음, 부채널 내성 | 느리고 발열 높음 |

**페이지 매핑 캐시** 물리 페이지의 기존 매핑을 재사용하고 히트·퇴출 카운터를 `OP_STATS` 로 읽는다

**VA→PA 변환 캐시** 같은 가상 주소를 반복해 읽을 때 페이지 테이블 순회를 생략한다. 카운터는 `tr_hits` / `tr_misses`

**swap 대체 경로** `!pte_present`(swap entry 또는 미매핑)이면 `get_user_pages_remote(FOLL_GET)` 로 읽기 fault 를 유발해 올린 뒤 재변환
`FOLL_WRITE` 가 아니라 `FOLL_GET` 을 써서 읽기 전용 매핑에 COW 부작용을 만들지 않는다

**페이지 경계 정확성** `PAGE_SIZE - (addr & (PAGE_SIZE-1))` 로 분할하고 구간마다 따로 변환한다

**캐시 별칭 수정** 대상 메모리에 쓴 뒤 GPU / ION 버퍼가 옛 값을 읽으면 화면이 검게 된다
일반 RAM 은 선형 매핑 직접 읽기(프로세스 페이지 테이블과 같은 WB 캐시, ARM64 는 PIPT 라 물리 인덱스로 일치)를 쓰고 쓴 뒤 `__flush_dcache_area` 로 메모리에 되돌린다

### 3 모듈 베이스

`OP_MODULE_BASE` 는 모듈 이름으로 대상 `mm` 의 `vm_area_struct` 를 훑어 해당 ELF 로드 베이스를 돌려준다
6.1 미만은 리스트를 직접 순회하고, 6.1 이상은 maple tree VMA 이터레이터를 쓰며 `compat.h` 의 `KH_HAVE_VMA_ITER` 로 분기한다
`get_module_base` 는 커널 내부에서도 쓴다

### 4 은닉

**파일 은닉** `filldir64`(없으면 `filldir`)를 hook 하고 `pre_handler` 에서 대상 항목 이름을 `.` 로, `namlen=1` 로 바꾼다
basename 일치, 최대 16개

**프로세스 은닉** 같은 hook 에서 순수 숫자 PID 항목을 대조, 최대 64개

**kgsl 은닉** GPU kgsl 쪽에서 pid 를 숨긴다. `kgsl_hook_install` 과 함께 사용

**전체 해제** `OP_UNHIDE_ALL` 로 두 표를 한 번에 비운다

kprobe 를 쓰는 이유 Android GKI 5.x 이상은 kCFI 와 읽기 전용 rodata 를 갖고 있어 함수 프롤로그를 고치면 CFI 검사나 쓰기 보호에 걸린다
kprobe 는 브레이크포인트 기구로 콜백을 등록할 뿐 코드를 쓰지 않으므로 구조적으로 CFI 를 피한다

심볼 지연 등록 `hide_*` 최초 호출 시 후보 목록(`filldir64` → `filldir`)으로 등록하므로 심볼이 보이지 않아도 적재는 실패하지 않는다

### 5 다섯 브레이크포인트 계열

"슬롯 + 링 버퍼 기록 + 분할 되읽기" 골격을 공유하고 `0x3E` 진단과 `0x3F` 전체 스위치도 공용
실행 브레이크포인트끼리는 상호 배타다. arm64 의 BRP 는 4개, WRP 는 2개뿐이다

| 계열 | 기구 | 자원 | 기록에서 얻는 것 | 용도 |
|---|---|---|---|---|
| `hwbp` | BRP 실행 / WRP 관측, perf 이벤트 | BRP 4 + WRP 2 | 트리거 PC 와 레지스터 | 함수 진입, 읽기·쓰기 지점 |
| `ptebp` | PTE 속성 반전 + 32바이트 섀도 트램펄린 | debug 레지스터 미사용 | 트리거 지점과 제어 가능 레지스터 | 십여 지점 동시 관측 |
| `wrpbp` | WRP 데이터 관측점, `LEN_8` 창 | WRP 2 | 쓰기 시각과 벡터 | 단일 필드 기록 감시 |
| `brkbp` | 명령을 `BRK` 로 변경, kprobe 예외 처리 | 하드웨어 미사용 | 트리거 PC 와 범용 레지스터 | 기록형 관측 |
| `udpbp` | UDP 경로에 kprobe | 하드웨어 미사용 | 패킷 내용 미리보기 | 통신 리듬 관측 |

**hwbp** arm64 실행 브레이크포인트는 `HW_BREAKPOINT_LEN_4` 만 지원
트리거 시 설정대로 PC 를 기록하고 분할해 되읽으며 2비트 레지스터 마스크로 되쓸 수 있다
`OP_HWBP_SET_REGS` 는 트리거 콜백에서 `s3`/`s4`/`s5` 를 직접 쓴다

**ptebp** PTE 속성 비트를 반전해 fault 를 만든다. 16 슬롯, 슬롯당 최대 8 페이지
debug 레지스터를 쓰지 않아 `hwbp` 와 공존한다
원래 명령은 트램펄린에서 실행되고, 트램펄린은 대상 `mm` 안에 RWX 페이지로 개설한다
PTE 비트 영역(`PTE_UXN` / `PTE_PXN` / `PTE_RDONLY` / `PTE_VALID` / `PTE_AF`)을 직접 읽고 쓸 수 있으며 변경 후 TLB 를 갱신한다

**wrpbp** `arch_install_hw_breakpoint` 의 `LEN_8` 데이터 창
트리거 시 `perf_event` 의 값을 기록으로 옮기고 `OP_WRPBP_SET_VEC` 가 2비트 마스크로 되쓴다
자이로 정상 상태가 이 계층에 올라가 있다

**brkbp** 명령을 `BRK` 로 바꾸고 예외 핸들러로 받는다. 16 슬롯
트리거 시 커널 공간에서 `wind_read_process_va` / `wind_write_process_va` 로 인자를 읽고 반환값을 고치며 `__user` 를 거치지 않는다
현재는 기록만 하고 레지스터 되쓰기는 하지 않는다

**udpbp** 커널 UDP 경로에 kprobe

```
TX  udp_send_skb  ->  ip_send_skb  ->  ip_local_out   해석되는 첫 번째 사용
RX  __udp4_lib_rcv
```

TX 는 `udp_send_skb` 를 우선한다. GKI 의 LTO 빌드가 `ip_local_out` 을 인라인하기 때문
pid, 포트, 방향으로 거르고 트리거는 링 버퍼로. 분할 읽기로 가져온다
8 슬롯, 슬롯당 256건, 건당 32바이트 미리보기
IPv4 만 지원. IPv6 의 `__udp6_lib_rcv` 는 연결되지 않았다

**구버전 brk** `OP_BRK_SET` / `OP_BRK_CLEAR` 는 `register_user_hw_breakpoint` 로 단일 실행 포인트를 구현하며 호환을 위해 남겨 둔다

**FP 레지스터 기록** 커널은 `-mgeneral-regs-only` 로 빌드되어 clang 통합 어셈블러가 `stp q3,q4,[x9]` 같은 니모닉을 거부한다
`hwbp_fp_reg_direct` 는 `.inst` 로 원시 인코딩을 쓰고 베이스 레지스터는 `x9` 고정

| 명령 | 인코딩 |
|---|---|
| `stp q3, q4, [x9]` | `0xad001123` |
| `str q5, [x9]` | `0x3d800125` |
| `ldp q3, q4, [x9]` | `0xad401123` |
| `ldr q5, [x9]` | `0x3dc00125` |

### 6 자이로

사용자 공간의 `/dev/uinput` 주입이 아니라 커널 공간에서 시스템 라이브러리의 BitTube 출구를 hook 한다

```
대상 심볼 android::BitTube::sendObjects(sp<BitTube> const&, void const*, size_t, size_t)
후보 모듈 libsensor.so -> libsensorservice.so -> libui.so
```

위치 파악 `filp_open` + `kernel_read` 로 대상 ELF 를 읽고 `.dynsym` 에서 심볼 오프셋을 얻은 뒤 대상 프로세스 내 해당 모듈의 로드 베이스를 더해 실행 시 주소를 만든다
`sendObjects` 인자(arm64) `x1` = events 배열, `x2` = count. `x0` 는 tube 로 데이터 위치 파악에 쓰지 않는다

두 단계

1. **위치 파악 단계** `sendObjects` 에 실행 브레이크포인트를 걸고 첫 트리거에서 events 버퍼 주소를 학습한다
2. **정상 상태** 곧바로 실행 브레이크포인트를 떼고 `data[0..1]` 에 `LEN_8` 쓰기 관측점을 건다

정상 상태에서는 WRP 하나만 남고 system_server 안에 실행 브레이크포인트가 없다. 트리거 빈도가 "센서 배치마다"에서 "기록마다"로 떨어진다
자신의 overflow 핸들러에서 perf 이벤트를 해제하면 교착이 생기므로 전환은 `gyro_set_velocity`(프로세스 컨텍스트, 프레임당 한 번)에서 수행한다

이벤트 배치(`sensors_event_t`, 0x68바이트)

```
0x00 version | 0x04 sensor | 0x08 type | 0x10 timestamp
0x18 float data[16] | 0x58 flags
```

type 은 `SENSOR_TYPE_GYROSCOPE`(4) 와 `SENSOR_TYPE_GYROSCOPE_UNCALIBRATED`(16) 를 인식
둘 다 x/y/z 가 `data[0..2]` 에 있고 기록 방식이 같다
NDK 의 `ASensorEvent` 는 type 을 `0x04` 로 옮기므로 코드는 `0x08` 을 먼저 보고 다음 `0x04` 를 본다

**Z 가 움직이지 않는 것은 구조적**이며 파라미터 제어가 아니다. 실행 브레이크포인트는 `+0x18` 에 8바이트를 쓰고 WRP 창 `LEN_8` 은 `data[0..1]` 을 덮으며 클라이언트 마스크는 `0x3`
`+0x20`(z)은 어떤 쓰기 창에도 들어가지 않는다

WRP 재귀 안전 자체 `copy_to_user_nofault` 가 관측점을 다시 건드린다
`user_mode(regs)` 와 per-CPU `gyro_wrp_busy` / `wrpbp_busy` 이중 차단으로 막는다

`gyro_stop` / `gyro_resume` 은 기록 일시 정지와 재개만 하고 hook 은 끊지 않는다
`gyro_remove` 가 실제로 해제한다
대상 프로세스가 끝나면 `gyro_resume` 은 `-ESRCH`, 설치되지 않았으면 `-ENODEV` 를 돌려주며 둘 다 멱등이다

### 7 섀도 페이지와 커널 쓰기 보호

**섀도 페이지 PTE 조작** `wind_shadow_pte_set` / `wind_shadow_pte_get`
대상 `mm` 페이지 테이블을 훑어 PTE 를 찾고 `(old & ~mask) | (val & mask)` 로 고친 뒤 TLB 를 갱신한다. 무흔적 브레이크포인트와 페이지 권한 미세 조정에 쓴다
물리 주소 영역은 `KH_PTE_PA_MASK` 로 뽑고 새 PTE 는 `KH_PHYS_TO_PTE` 로 조립한다
52비트 PA 기기에서는 물리 주소 상위 2비트가 PTE `[9:8]` 에 있어 단순한 `| page_to_phys()` 는 엔트리를 망가뜨린다

**커널 이미지 쓰기 보호 우회** `wind_force_kernel_page_rw` / `wind_kernel_page_ro_state`
`set_memory_rw` 는 이미지 주소를 받지 않으므로 모듈이 직접 페이지 테이블을 훑어 이미지 페이지를 쓰기 가능하게 만든다
`ro_state` 는 페이지 테이블 경로를 검증한 뒤 리프 AP[2] 만 읽어 아무것도 쓰지 않고 읽기 전용 상태를 판정한다

---

## 심볼 전략: 제로 CRC + 실행 시 해석

두 축이며 하나만 빠져도 실기에서 적재되지 않는다

### 제로 CRC

빌드 후 `__versions` / `__version_ext_crcs` / `__version_ext_names` 세 섹션 길이를 0으로 만든다
커널의 `check_version()` 은 CRC 비교를 통째로 건너뛰고 `pr_warn_once` 한 번만 남긴다
`__versions` 섹션이 존재하므로 `same_magic(has_crcs=1)` 은 vermagic 의 첫 공백 이후 꼬리 `SMP preempt mod_unload modversions aarch64` 만 비교한다

| 커널 | 파일 | 버전 표가 비었을 때 분기 | `same_magic` 세 번째 인자 |
|---|---|---|---|
| 5.10.198 | `kernel/module.c` | `Broken toolchain` → `return 1` | `info->index.vers` |
| 5.15.149 | `kernel/module.c` | 동일 | 동일 |
| 6.1 | `kernel/module/version.c` | 동일 | 동일 |
| 6.6.66 | `kernel/module/version.c` | 동일 | 동일 |
| 6.12.58 | `kernel/module/version.c` | 동일, ext 표 분기 추가 | 동일 |
| 6.18.21 | `kernel/module/version.c` | 동일, ext 표 분기 추가 | 동일 |

경계 이 방식이 없애는 것은 "심볼 CRC 가 기기마다 같아야 한다"는 조건이며 vermagic 은 없애지 않는다
꼬리(`SMP preempt mod_unload modversions aarch64`)는 대상 커널과 바이트 단위로 일치해야 한다
그래서 같은 계열·같은 config 특성의 기기와 다른 `LOCALVERSION` 접두사는 하나의 ko 로 덮인다
계열을 넘으면(6.1 ko 를 6.12 에) `bad vermagic` 으로 거부된다
전제는 config 가 실기와 같은 구성이라는 점이다. 그렇지 않으면 CFI 타입 해시와 구조체 배치가 어긋나 적재 후에 죽는다

실기 vermagic

```
6.12.58-android16-6-g7704a1ae279b-ab15213644-4k SMP preempt mod_unload modversions aarch64
```

### 실행 시 심볼 해석

GKI 는 KMI 를 동결하고 벤더 모듈은 화이트리스트 교집합의 심볼만 참조할 수 있다
이 드라이버가 쓰는 32개 함수는 여기에 없어 직접 참조하면

```
wind: Unknown symbol kthread_use_mm (err -2)
```

`err -2` 는 ENOENT 로, 내보내기 표에 그 이름이 없어 `insmod` 가 즉시 실패한다
그래서 전부 `sym.c` 를 거치게 했다. `register_kprobe`(화이트리스트 내)가 이름으로 주소를 해석하고 빌드 시 CRC 참조를 만들지 않는다

```
mm/프로세스  mmput  kthread_use_mm  kthread_unuse_mm  remap_pfn_range
vmap        __get_vm_area_caller  free_vm_area  vunmap  ioremap_page_range  iounmap
페이지 테이블 set_memory_rw  set_memory_ro  contpte_ptep_get  __arch_clear_user
파일        filp_open  kernel_read  file_path
디바이스    misc_register  misc_deregister
netlink     netlink_kernel_create  __netlink_kernel_create  netlink_kernel_release
            __netlink_kernel_release  netlink_unicast  __alloc_skb
브레이크    register_user_hw_breakpoint  unregister_hw_breakpoint
            register_wide_hw_breakpoint  unregister_wide_hw_breakpoint
부동소수    fpsimd_preserve_current_state
nofault     copy_from_user_nofault  copy_to_user_nofault
```

해석 결과는 적재 시 dmesg 로 출력되고 실패하면 `wind: ... 未解析到` 를 남긴다
해당 기능은 축소되지만 적재 자체는 성공한다

### 호환 계층

`compat.h` 한 곳에 모아 5.10 부터 6.18 까지 한 소스로 처리한다

| 항목 | 경계 | 처리 |
|---|---|---|
| `mmap_lock` | 5.8 | `mmap_read_lock` ↔ `down_read(&mm->mmap_sem)` |
| `kmap_local_page` / `kunmap_local` | 5.11 | `kmap_atomic` / `kunmap_atomic` 으로 후퇴 |
| `ptep_get` | 5.10 | 없으면 `*(ptep)` |
| maple tree VMA 순회 | 6.1 | `KH_HAVE_VMA_ITER` |
| `get_user_pages_remote` 인자 수 | 5.9 / 6.5 | 세 갈래 |
| `__flush_dcache_area` | 5.11 | `dc civac` 인라인 시퀀스 |
| `arm64_use_ng_mappings` | 6.1 / 6.6 / 6.12 | 버전별 값 조회 |
| `PTE_ADDR_MASK` | 5.11 에서 삭제 | `KH_PTE_PA_MASK` 4단 수렴 |
| 물리 주소의 PTE 반영 | 52비트 PA | `KH_PHYS_TO_PTE` 로 비트 배치 조립 |
| 버전 헤더 | 6.10 에서 `linux/version.h` 삭제 | `generated/uapi/linux/version.h` 참조 |

---

## 사용자 공간 연동

| 파일 | 역할 |
|---|---|
| `user/Wind-对接头文件.h` | `c_wind` 한 클래스에 전 기능: 읽기·쓰기 / 배치 / 프레임 캐시 / 스냅샷 / 은닉 / kgsl / 자이로 / 다섯 브레이크포인트 계열 |
| `user/Wind-drivers-netlink_v1.3.0.zip` | netlink 클라이언트 예제 |
| `user/Wind-Drivers-*.sh` | 커널별 자동 압축 해제 설치본(5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18) |
| `user/Wind-Drivers-6.6.ko` | 6.6 용 빌드 결과물 |
| `user/1.sh` | 설치 보조 스크립트 |

최소 흐름

```cpp
#include "wind.hpp"

c_wind &d = c_wind::instance(target_pid);   // 기본 netlink unit 31
int v = d.read<int>(base + 0x10);
d.write<int>(base + 0x10, 123);

d.frame_begin();                            // 한 프레임에 같은 주소는 한 번만 읽음
uint64_t hp = 0;
d.frame_read(base + 0x100, hp);

d.snap_start(base + 0x200, 60);             // 백그라운드 60Hz 갱신
uint32_t cur = d.snap_value<uint32_t>();
d.snap_stop();
```

`sizeof(c_wind)` 는 약 48KB, 그중 47KB 가 프레임 캐시 표(2048 슬롯)
지역 인스턴스는 힙에 두는 편이 좋다

추적 방식을 바꾸기 전에 `bp_all_off()` 를 부른다. 이전 계열이 BRP/WRP 를 잡고 있으면 새 계열이 들어가지 않는다

```cpp
d.bp_all_off();        // 0x3F, 다섯 계열 일괄 해제
```

---

## 빌드

### 방식 A — GitHub Actions (권장)

```
Actions -> build-wind-ko -> Run workflow -> 커널 선택 -> artifact 내려받기
```

매트릭스는 여섯 줄이며 **전부 제로 CRC 방식**(`zero_versions: "1"`)

```
android12-5.10.198
android13-5.15.149
android14-6.1
android15-6.6.66
android16-6.12.58-device      실기 config 정렬
android17-6.18.21
```

### 방식 B — 로컬 크로스 빌드

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LLVM=1 LLVM_IAS=1
make -j"$(nproc)" M=kernel modules
```

clang 14 이상(5.x 는 접두사 없는 LLVM 도구 필요) 또는 aarch64-linux-gnu-gcc
커널은 `-mgeneral-regs-only` 로 빌드되므로 인라인 어셈블리의 FP/SIMD 는 `.inst` 로 써야 한다. 니모닉 형식을 받아 주는 것은 gcc 뿐이다

### 보조 도구

| 도구 | 용도 |
|---|---|
| `tools/dump_kcrc.py` | 커널 CRC 표의 `dump` / `harvest` / `merge` / `check` / `diff` / `count` |
| `tools/fix_versions.py` | symvers 표에 따라 `__versions` 의 CRC 재작성 |
| `tools/fix_vermagic.py` | `.modinfo` 를 파일 끝으로 옮기고 대상 vermagic 기록 |
| `tools/extract_installer_ko.py` | `.sh` 설치본에서 내장 ko 와 CRC 표 추출 |
| `tools/zero_versions.py` | 제로 CRC 방식, 버전 섹션 길이 0 처리(`-n` 은 확인만, 수정 시 `.bak` 유지) |
| `tools/harvest_kcrc.sh` | 실기 vendor 모듈에서 CRC 표 일괄 수집 |
| `tools/windbench.cpp` | 통신 경로 부하 시험과 발열 비교 |
| `tools/btf_offsets.py` | BTF 에서 구조체 오프셋 추출 |
| `tools/pad_info.sh` / `pad_pack.sh` / `pad_probe.sh` | 기기 정보 수집, 패킹, 탐지 |
| `tools/diag_lockup.sh` | 멈춤 현장 수집 |
| `extract_symvers.py` | ko 에서 symvers 추출 |

---

## 디렉터리 구조

```
Wind_driver/
├── kernel/                 모듈 소스 (8067줄)
│   ├── entry.c             적재 진입, 통신 경로 설치, ioctl 디스패치
│   ├── memory.c            읽기·쓰기, 페이지 테이블 순회, 매핑 캐시, mmap
│   ├── process.c           모듈 베이스, 프로세스 관련
│   ├── hide.c              파일·프로세스 은닉(kprobe)
│   ├── breakpoint.c        다섯 계열 + UDP + 섀도 페이지
│   ├── gyro.c              자이로 hook(실행 bp → WRP 정상 상태)
│   ├── kgsl.c              GPU kgsl hook
│   ├── nlcomm.c            netlink 경로와 op 화이트리스트
│   ├── sym.c               화이트리스트 밖 32개 심볼 실행 시 해석
│   ├── comm.h              오퍼코드와 프로토콜 구조체
│   ├── compat.h            커널 호환 계층(5.10 → 6.18)
│   ├── memory.h / breakpoint.h / gyro.h / nlcomm.h / process.h / kgsl.h / sym.h
│   ├── Kconfig
│   └── Makefile
├── user/                   클라이언트 헤더, 예제, 설치본
├── tools/                  빌드·기기 보조 스크립트
├── .github/workflows/      CI 크로스 빌드
├── CHANGES.md              라운드별 변경과 실기 증거
├── CHANGELOG.md            초기 마일스톤
├── HOOKPTE-对照.md         hook 구현 대조표
├── README.md               중국어
├── README.en.md            영어
├── README.ja.md            일본어
├── README.ru.md            러시아어
├── README.ko.md            한국어(이 파일)
└── LICENSE
```

---

## 문제 해결

| 증상 | 확인 지점 |
|---|---|
| 적재 시 `Unknown symbol ... (err -2)` | 해당 심볼이 내보내지지 않음. `sym.c` 실행 시 해석으로 돌린다 |
| `disagrees about version of symbol` | CRC 불일치. 제로 CRC 방식으로 가거나 다시 빌드 |
| `version magic ... should be` | vermagic 불일치 |
| `insmod` 후 통신 경로 없음 | `dmesg` 의 `transport`, `misc_register`, `netlink` |
| 통신 경로를 열면 멈춤·재부팅 | `transport=2` 사용, `allow_fops_hook=1` 회피 |
| 오래된 값이 읽힘 | `MODE_SAFE` 로 전환, GPU / ION 버퍼인지 확인 |
| 은닉이 동작하지 않음 | `CONFIG_KALLSYMS_ALL` 여부, `filldir64` 가시성 |
| 자이로가 동작하지 않음 | 대상이 센서를 읽는 프로세스인지, `sendObjects` 해석 여부, `dmesg` 에 WRP armed 여부 |
| 브레이크포인트가 걸리지 않음 | 먼저 `bp_all_off`, 다음 `OP_BP_DIAG` 로 BRP/WRP 여유 확인 |
| 섀도 페이지 트리거 후 대상이 죽음 | `KH_PTE_PA_MASK` / `KH_PHYS_TO_PTE` 와 실기 PA 비트 폭 일치 여부 |

로그 키워드

```
wind:
wind: misc_register
wind: netlink
wind: sym use_mm
filldir64
gyro
hwbp / ptebp / wrpbp / brkbp / udpbp
bp: all off
```

---

## 알려진 제한

- 은닉은 커널 심볼 가시성에 의존한다. `CONFIG_KALLSYMS_ALL` 이 없으면 `filldir64` 를 찾지 못한다
- `/dev/null` fops 경로는 일부 기기에서 멈춤·재부팅 기록이 있어 기본 비활성
- 제로 CRC 는 심볼 CRC 만 건너뛰고 vermagic 꼬리, CFI, 구조체 배치는 건너뛰지 않는다
- 제로 CRC 는 커널 계열을 넘지 못한다. 6.1 ko 를 6.12 에 두면 `bad vermagic` 으로 거부된다
- `brkbp` 는 기록만 하고 레지스터 되쓰기는 없다
- `hwbp` / `ptebp` 는 아직 TID 한정이 없어 트리거가 프로세스 전역이다
- arm64 실행 브레이크포인트는 `LEN_4` 만 지원하며 명령은 4바이트 정렬이어야 한다
- BRP 4개, WRP 2개는 하드웨어 상한이라 다섯 계열을 동시에 쓰면 서로 잡아먹는다
- `ptebp` 는 슬롯당 최대 8 페이지를 덮으며 페이지가 바뀌면 `OP_PTEBP_REFRESH` 가 필요하다
- UDP 브레이크포인트는 IPv4 만 지원하고 IPv6 경로는 연결되지 않았다
- 6.12.58 이전 산출물은 `tools/device/` 의 config 와 CRC 표에 의존하므로 기기를 바꾸면 다시 수집해야 한다

---

## 변경 이력

최근 여덟 라운드(50 → 57)

| 라운드 | 내용 |
|---|---|
| 50 | Keep 클라이언트를 새 기능 전부에 연결 |
| 51 | 추적 방식 전환 시 스택 손상 |
| 52 | `entry.c` 디스패치 블록 중복과 컴파일·논리 결함 세 건 |
| 53 | `breakpoint.c` 빌드 오류와 심볼 누락 |
| 54 | arm64 커널이 부동소수 레지스터를 막아 `.inst` 원시 인코딩으로 전환 |
| 55 | 5.10 / 6.12 / 6.18 이식 차이(`kmap_local_page`, `PTE_ADDR_MASK`) |
| 56 | 6.10 이후 `include/linux/version.h` 부재로 생성 헤더 참조 |
| 57 | `kthread_use_mm` / `kthread_unuse_mm` 미내보내기로 실행 시 해석 전환 |

라운드별 상세 기록과 실기 증거는 `CHANGES.md`
성능·발열 비교는 `tools/windbench.cpp`
