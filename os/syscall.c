#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_getpid(void)
{
    return curr_proc()->pid;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd){
	struct proc *p = curr_proc();

	// Start must be page aligned, as required in some vm.c functions
	if ((start % PGSIZE) != 0){
		return -1;
	}

	// Following 4 are cases check for errors pinted out in the instructions
	uint64 end = start + len;
	if (end < start){
		return -1;
	}

	if (len > (1UL << 30)){
		return -1;
	}

	if ((port & ~0x7) != 0){
		return -1;
	}

	if ((port & 0x7) == 0){
		return -1;
	}

	// Determine number of pages needed for requested len
	uint64 map_len = PGROUNDUP(len);
	uint64 map_end = start + map_len;
	
	// Check needed to pass Test 04_3
	if(map_end < start){
		return -1;
	}

	// Check to make sure that none of the pages assinged have alreaby been mapped by using walkaddr
	for (uint64 va = start; va < map_end; va += PGSIZE) {
		if (walkaddr(p->pagetable, va) != 0)
			return -1;
	}

	// Determine permission based on bits provided
	int perm = PTE_U;
	if (port & 0x1){
		perm |= PTE_R;
	}
	if (port & 0x2){
		perm |= PTE_W;
	}
	if (port & 0x4){
		perm |= PTE_X;
	}

	// Allocate and map each page
	for (uint64 va = start; va < map_end; va += PGSIZE) {
		void *pa = kalloc();

		// If mapping fails, then unmap and free all pages that were already done.
		if (pa == 0) {
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
			return -1;
		}

		// Inserts the mapping into the process's page table, but if it fails then free everything
		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) != 0) {
			kfree(pa);
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
			return -1;
		}
	}

	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len){
	struct proc *p = curr_proc();

	// Must be page aligned
	if ((start % PGSIZE) != 0){
		return -1;
	}

	uint64 end = start + len;
	
	// Ensure all pages are in range are mapped
	for (uint64 va = start; va < end; va += PGSIZE) {
		if (walkaddr(p->pagetable, va) == 0){
			return -1;
		}
	}

	// Removes the mapping
	uvmunmap(p->pagetable, start, len / PGSIZE, 1);
	return 0;
}

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// YOUR CODE
	struct proc *p = curr_proc();

	// Transform address from virtual to physical so solution from project 1 will work.
	uint64 pa = useraddr(p->pagetable, (uint64)val);
	if(pa == 0){
		return -1;
	}

	val = (TimeVal *)pa;

	uint64 cycle = get_cycle();
	val->sec = cycle / CPU_FREQ;
	val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/

int sys_task_info(TaskInfo *ti){
	// grab process currently being ran
	struct proc *p = curr_proc();

	// With virtual memory now in use, we must first use useraddr function to turn virtual address to physical.
	uint64 pa = useraddr(p->pagetable, (uint64)ti);
	if(pa == 0){
		return -1;
	}

	ti = (TaskInfo *)pa;

	// convert procstate enum to TaskStatus enum
	switch(p->state){
		case RUNNING:
    		ti->status = Running;
    		break;
		case RUNNABLE:
		case USED:
		case SLEEPING:
    		ti->status = Ready;
    		break;
		case ZOMBIE:
    		ti->status = Exited;
    		break;
		case UNUSED:
		default:
    		ti->status = UnInit;
    		break;
	}

	// Get the number of syscalls from proc
	memmove(ti->syscall_times, p->syscall_times, sizeof(p->syscall_times));

	// Calculate time
	uint64 now = get_cycle() / (CPU_FREQ / 1000);
	if (p->start_time == 0)
    	ti->time = 0;
	else
    	ti->time = (int)((now - p->start_time));

	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	/*
	* LAB1: you may need to update syscall counter for task info here
	*/

		curr_proc()->syscall_times[id]++;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_getpid:
    ret = (int)sys_getpid();
    	break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;

	// New cases so that the functions are called when needed.
	case SYS_mmap:
	ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
	ret = sys_munmap(args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/

	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;

	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
