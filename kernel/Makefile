# 精简体积: 尺寸优化, 去除 unwinder 表
ccflags-y += -Os -fno-unwind-tables -fno-asynchronous-unwind-tables

# 单模块 wind.ko, 合并全部 .o (外部编译 make M=... modules 产出 wind.ko)
obj-m += wind.o
wind-y := sym.o memory.o process.o entry.o hide.o kgsl.o gyro.o gyro_sock.o nlcomm.o clonebp.o ace.o windcfg.o
