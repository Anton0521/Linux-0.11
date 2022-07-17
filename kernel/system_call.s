/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 *
 * system_call.s 文件包含系统调用(system-call)底层处理子程序。由于有些代码比较类似，所以
 * 同时也包括时钟中断处理(timer-interrupt)句柄。硬盘和软盘的中断处理程序也在这里。
 *
 * 注意：这段代码处理信号(signal)识别，在每次时钟中断和系统调用之后都会进行识别。一般
 * 中断信号并不处理信号识别，因为会给系统造成混乱。
 *
 * 从系统调用返回'ret_from_system_call'时堆栈的内容,見19~30行.
 */

SIG_CHLD	= 17

EAX		= 0x00					# 堆栈中各个寄存器的偏移位置。
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C
CS		= 0x20
EFLAGS		= 0x24
OLDESP		= 0x28
OLDSS		= 0x2C

state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

nr_system_calls = 72

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2 								# 内存2字节对齐
bad_sys_call: 							# 错误的系统调用号
	movl $-1,%eax 						# eax中置-1
	iret 								# 退出中断
.align 2
reschedule: 							# 重新执行调度程序入口,调度程序schedule在(kernel/sched.c).
	pushl $ret_from_sys_call 			# 将ret_from_sys_call的地址入栈
	jmp _schedule
.align 2
_system_call: 							# linux系统调用入口(调用中断int 0x80，eax中是调用号)
	cmpl $nr_system_calls-1,%eax 		# 检测调用号eax是否超出范围
	ja bad_sys_call 					# 超出范围,返回-1,退出中断
	push %ds 							
	push %es 							
	push %fs 							
	pushl %edx
	pushl %ecx							# push %ebx,%ecx,%edx as parameters to the system call
	pushl %ebx							# 将系统调用参数入栈, 其中ebx,ecx,edx对应函数参数0,1,2
	movl $0x10,%edx		
	mov %dx,%ds							# set up ds,es to kernel space. ds,es指向内核数据段(全局描述符表中数据段描述符)
	mov %dx,%es
	movl $0x17,%edx	
	mov %dx,%fs							# fs points to local data space. fs指向局部数据段(局部描述符表中数据段描述符)
	call _sys_call_table(,%eax,4)		# call_addr = _sys_call_table + %eax * 4, 其中sys_call_table在(include/linux/sys.h),(call -> pushl %eip| movl call_addr %eip)
	pushl %eax							# 把系统调用号入栈
	movl _current,%eax					# 取当前进程数据结构地址
	cmpl $0,state(%eax)					# 查看当前任务的运行状态
	jne reschedule						# 如果当前任务不在就绪状态"[%eax + state] != $0"，执行调度程序(跳转到reschedule)
	cmpl $0,counter(%eax)				# 如果当前任务在就绪状态，查看时间片是否用完
	je reschedule						# 如果时间片已用完“[%eax + counter] = $0”, 则也执行调度程序(跳转到reschedule)
ret_from_sys_call:						# 从系统调用C函数返回后，对信号进行识别处理
	movl _current,%eax					# task[0] cannot have signals
	cmpl _task,%eax						# 判断当前任务是否是初始任务task0
	je 3f								# 如果是，则不比对其进行信号量方面的处理，直接返回(向前[forward]跳到标号3)。
	cmpw $0x0f,CS(%esp)					# was old code segment supervisor ?  判断调用程序是否为用户任务(内核态执行时不可抢占)。
	jne 3f								# 如果不是(说明某个中断服务程序跳转到上面的)，则直接退出中断。
	cmpw $0x17,OLDSS(%esp)				# was stack segment = 0x17 ?  判断原堆栈是否在用户段中
	jne 3f								# 如果不是(说明系统调用的调用者不是用户任务），则也退出。
	movl signal(%eax),%ebx				# 取信号位图"[%eax + signal] → ebx",每1位代表1种信号，共32个信号
	movl blocked(%eax),%ecx				# 取阻塞(屏蔽)信号位图"[%eax + blocked] → ecx"
	notl %ecx							# 每位取反
	andl %ebx,%ecx						# 获得许可信号位图
	bsfl %ecx,%ecx						# 从低位(位0)开始扫描位图，看是否有1的位，若有，则ecx保留该位的偏移值
	je 3f								# 如果没有信号则向前跳转退出
	btrl %ecx,%ebx						# 复位该信号(ebx含有原signal位图)
	movl %ebx,signal(%eax)				# 重新保存signal位图信息→current->signal.
	incl %ecx							# 将信号调整为从1开始的数(1-32)
	pushl %ecx							# 信号值入栈作为调用do_signal的参数之一
	call _do_signal						# 调用C函数信号处理程序(kernel/signal.c)
	popl %eax							# 弹出入栈的信号值
3:	popl %eax							# eax中含有上面入栈系统调用的返回值
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret

.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	call _math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret

.align 2
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call

.align 2
_sys_execve:
	lea EIP(%esp),%eax
	pushl %eax
	call _do_execve
	addl $4,%esp
	ret

.align 2
_sys_fork:
	call _find_empty_process
	testl %eax,%eax
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process
	addl $20,%esp
1:	ret

_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
1:	xorl %edx,%edx
	xchgl _do_hd,%edx
	testl %edx,%edx
	jne 1f
	movl $_unexpected_hd_interrupt,%edx
1:	outb %al,$0x20
	call *%edx		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
