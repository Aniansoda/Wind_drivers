# 精简体积: 尺寸优化, 去除 unwinder 表
ccflags-y += -Os -fno-unwind-tables -fno-asynchronous-unwind-tables

# 单模块 khack.ko, 合并全部 .o (外部编译 make M=... modules 产出 khack.ko)
obj-m += khack.o
khack-y := memory.o process.o entry.o hide.o breakpoint.o kgsl.o
