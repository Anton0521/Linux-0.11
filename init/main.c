/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();									// 送格式化输出到一字符串中(在kernel/vsprintf.c)
extern void init(void);									// 函数原形,初始化(在后面)
extern void blk_dev_init(void);							// 块设备初始化子程序(kernel/blk_drv/ll_rw_blk.c)
extern void chr_dev_init(void);							// 字符设备初始化(kernel/chr_drv/tty_io.c)
extern void hd_init(void);								// 硬盘初始化程序(kernel/blk_drv/hd.c)
extern void floppy_init(void);							// 软驱初始化程序(kernel/blk_drv/floppy.c)
extern void mem_init(long start, long end);				// 内存管理初始化(mm/memory.c)
extern long rd_init(long mem_start, int length);		// 虚拟盘初始化(kernel/blk_drv/ramdisk.c)
extern long kernel_mktime(struct tm * tm);				// 建立内核时间(秒)
extern long startup_time;								// 内核启动时间(开机时间)(秒)

/*
 * This is set up by the setup-routine at boot-time
 * 以下这些数据是由setup.s 程序在引导时间设置的。
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 * 是啊，是啊，下面这段程序很差劲，但我不知道如何正确地实现，而且好象它还能运行。
 * 如果有关于实时时钟更多的资料，那我很感兴趣。这些都是试探出来的，以及看了一些bios 程序，呵！
 */

/*
 * 这段宏读取CMOS 实时时钟信息。
 * 0x70 是写端口号，0x80|addr 是要读取的CMOS 内存地址。
 * 0x71 是读端口号。
 */
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)    // 将BCD码转换成数字。

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;									// 机器具有的内存(字节数)
static long buffer_memory_end = 0;							// 高速缓冲区末端地址
static long main_memory_start = 0;							// 主内存(将用于分页)开始的位置

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. 这里确实是void，并没错。*/
{					/* The startup routine assumes (well, ...) this. 在startup 程序(head.s)中就是这样假设的。*/
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 * 此时中断仍被禁止着，做完必要的设置后就将其开启。
 */
 	ROOT_DEV = ORIG_ROOT_DEV;								// 根设备号 -> ROOT_DEV
 	drive_info = DRIVE_INFO;
	memory_end = (1<<20) + (EXT_MEM_K<<10);					// 机器内存数 -> memory_end, 内存大小 = 1Mb字节 + 扩展内存(k)*1024 字节。
	memory_end &= 0xfffff000;								// 忽略不到4Kb(1页)的内存数。
	if (memory_end > 16*1024*1024)							// 如果内存>16Mb, 则按16Mb计。
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 							// 如果内存>12Mb，则设置缓冲区末端=4Mb
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)						// 如果12Mb>内存>6Mb，则设置缓冲区末端=2Mb
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;					// 否则设置缓冲区末端=1Mb
	main_memory_start = buffer_memory_end;					// 主内存起始位置 = 缓冲区末端
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	mem_init(main_memory_start,memory_end);
	trap_init();											// 陷阱门(硬件中断向量)初始化. (kernel/traps.c)
	blk_dev_init();											// 块设备初始化. (kernel/blk_dev/ll_rw_blk.c)
	chr_dev_init();											// 字符设备初始化. (kernel/chr_dev/tty_io.c)空，为以后扩展做准备。
	tty_init();												// tty 初始化. (kernel/chr_dev/tty_io.c)
	time_init();											// 设置开机启动时间 -> startup_time
	sched_init();											// 调度程序初始化(加载了任务0的tr,ldtr). (kernel/sched.c)
	buffer_init(buffer_memory_end);							// 缓冲管理初始化，建内存链表等. (fs/buffer.c）
	hd_init();												// 硬盘初始化. (kernel/blk_dev/hd.c)
	floppy_init();											// 软驱初始化. (kernel/blk_dev/floppy.c)
	sti();													// 所有初始化工作都做完了，开启中断.
	move_to_user_mode();									// 移到用户模式. (include/asm/system.h)
	if (!fork()) {		/* we count on this going ok */
		init();
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 * 注意!! 对于任何其它的任务，'pause()'将意味着我们必须等待收到一个信号才会返
 * 回就绪运行态，但任务0（task0）是唯一的意外情况（参见'schedule()'），因为任
 * 务0 在任何空闲时间里都会被激活（当没有其它任务在运行时），
 * 因此对于任务0'pause()'仅意味着我们返回来查看是否有其它任务可以运行，如果没
 * 有的话我们就回到这里，一直循环执行'pause()'。
 */
	for(;;) pause();
}

															// 产生格式化信息并输出到标准输出设备stdout(1)，这里是指屏幕上显示。
															// 参数'*fmt'指定输出将采用的格式，参见各种标准C 语言书籍。
															// 该子程序正好是vsprintf 如何使用的一个例子。
															// 该程序使用vsprintf()将格式化的字符串放入printbuf缓冲区，
															// 然后用write()将缓冲区的内容输出到标准设备(1--stdout)。
static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);							// 对应函数是sys_setup(),在kernel/blk_drv/hd.c
	(void) open("/dev/tty0",O_RDWR,0);						// 用读写访问方式打开设备"/dev/tty0"(终端控制台),返回的句柄号0 -- stdin标准输入设备。
	(void) dup(0);											// 复制句柄，产生句柄1号 -- stdout标准输出设备。
	(void) dup(0);											// 复制句柄，产生句柄2号 -- stderr标准出错输出设备。
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);								// 打印缓冲区块数和总字节数，每块1024 字节。
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start); // 打印空闲内存字节数。
	if (!(pid=fork())) {									// 创建一个子进程,父进程将返回子进程的进程号,运行子进程.
		close(0);											// 子进程关闭了句柄0(stdin)
		if (open("/etc/rc",O_RDONLY,0))						// 子进程以只读方式打开/etc/rc文件
			_exit(1);										// 子进程如果打开文件失败,则退出. (/lib/_exit.c)
		execve("/bin/sh",argv_rc,envp_rc);					// 装入/bin/sh程序并执行,所带参数和环境变量分别由argv_rc和envp_rc数组给出. (/lib/execve.c)
		_exit(2);											// 若execve()执行失败则退出(出错码2,“文件或目录不存在”).
	}
	if (pid>0)												// 父进程执行的语句
		while (pid != wait(&i))								// 等待子进程停止或终止,返回子进程的进程号(pid),&i存放返回状态信息,如果wait()返回值不等于子进程号，则继续等待
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
