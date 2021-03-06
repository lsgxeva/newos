/*
** Copyright 2001-2004, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/debug.h>
#include <kernel/console.h>
#include <kernel/thread.h>
#include <kernel/arch/thread.h>
#include <kernel/khash.h>
#include <kernel/int.h>
#include <kernel/smp.h>
#include <kernel/timer.h>
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/int.h>
#include <kernel/arch/vm.h>
#include <kernel/sem.h>
#include <kernel/port.h>
#include <kernel/vfs.h>
#include <kernel/elf.h>
#include <kernel/heap.h>
#include <kernel/signal.h>
#include <kernel/list.h>
#include <newos/user_runtime.h>
#include <newos/errors.h>
#include <boot/stage2.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

struct proc_key {
	proc_id id;
};

struct thread_key {
	thread_id id;
};

struct proc_arg {
	char *path;
	char **args;
	unsigned int argc;
};

static void insert_proc_into_parent(struct proc *parent, struct proc *p);
static void remove_proc_from_parent(struct proc *parent, struct proc *p);
static struct proc *create_proc_struct(const char *name, bool kernel);
static int proc_struct_compare(void *_p, const void *_key);
static unsigned int proc_struct_hash(void *_p, const void *_key, unsigned int range);
static void proc_reparent_children(struct proc *p);

// global
spinlock_t thread_spinlock = 0;
const int fault_handler_offset = (addr_t)&(((struct thread *)0)->fault_handler) - (addr_t)0;

// proc list
static void *proc_hash = NULL;
static struct proc *kernel_proc = NULL;
static proc_id next_proc_id = 1;
static spinlock_t proc_spinlock = 0;
	// NOTE: PROC lock can be held over a THREAD lock acquisition,
	// but not the other way (to avoid deadlock)
#define GRAB_PROC_LOCK() acquire_spinlock(&proc_spinlock)
#define RELEASE_PROC_LOCK() release_spinlock(&proc_spinlock)

// process groups
struct pgid_node {
	pgrp_id id; 
	struct list_node node;
	struct list_node list;
};
static void *pgid_hash = NULL;
static int pgid_node_compare(void *_p, const void *_key);
static unsigned int pgid_node_hash(void *_p, const void *_key, unsigned int range);
static int add_proc_to_pgroup(struct proc *p, pgrp_id pgid);
static int remove_proc_from_pgroup(struct proc *p, pgrp_id pgid);
static struct pgid_node *create_pgroup_struct(pgrp_id pgid);
static int send_pgrp_signal_etc_locked(pgrp_id pgid, uint signal, uint32 flags);

// session groups
struct sid_node {
	pgrp_id id; 
	struct list_node node;
	struct list_node list;
};
static void *sid_hash = NULL;
static int sid_node_compare(void *_s, const void *_key);
static unsigned int sid_node_hash(void *_s, const void *_key, unsigned int range);
static int add_proc_to_session(struct proc *p, sess_id sid);
static int remove_proc_from_session(struct proc *p, sess_id sid);
static struct sid_node *create_session_struct(sess_id sid);

// thread list
static struct thread *idle_threads[_MAX_CPUS];
static void *thread_hash = NULL;
static thread_id next_thread_id = 1;

static sem_id snooze_sem = -1;

// death stacks
// used temporarily as a thread cleans itself up
struct death_stack {
	region_id rid;
	addr_t address;
	bool in_use;
};
static struct death_stack *death_stacks;
static unsigned int num_death_stacks;
static unsigned int volatile death_stack_bitmap;
static sem_id death_stack_sem;

// thread queues
static struct list_node run_q[THREAD_NUM_PRIORITY_LEVELS] = { { NULL, NULL }, };
static struct list_node dead_q;

static int _rand(void);
//static struct proc *proc_get_proc_struct(proc_id id); // unused
static struct proc *proc_get_proc_struct_locked(proc_id id);

// insert a thread onto the tail of a queue
void thread_enqueue(struct thread *t, struct list_node *q)
{
	list_add_tail(q, &t->q_node);
}

struct thread *thread_lookat_queue(struct list_node *q)
{
	return list_peek_head_type(q, struct thread, q_node);
}

struct thread *thread_dequeue(struct list_node *q)
{
	return list_remove_head_type(q, struct thread, q_node);
}

void thread_dequeue_thread(struct thread *t)
{
	list_delete(&t->q_node);
}

struct thread *thread_lookat_run_q(int priority)
{
	return thread_lookat_queue(&run_q[priority]);
}

void thread_enqueue_run_q(struct thread *t)
{
	// these shouldn't exist
	if(t->priority > THREAD_MAX_PRIORITY)
		t->priority = THREAD_MAX_PRIORITY;
	if(t->priority < 0)
		t->priority = 0;

	thread_enqueue(t, &run_q[t->priority]);
}

static struct thread *thread_dequeue_run_q(int priority)
{
	return thread_dequeue(&run_q[priority]);
}

static void insert_thread_into_proc(struct proc *p, struct thread *t)
{
	list_add_head(&p->thread_list, &t->proc_node);
	p->num_threads++;
	if(p->num_threads == 1) {
		// this was the first thread
		p->main_thread = t;
	}
	t->proc = p;
}

static void remove_thread_from_proc(struct proc *p, struct thread *t)
{
	list_delete(&t->proc_node);
	p->num_threads--;
}

static int thread_struct_compare(void *_t, const void *_key)
{
	struct thread *t = _t;
	const struct thread_key *key = _key;

	if(t->id == key->id) return 0;
	else return 1;
}

// Frees the argument list
// Parameters
// 	args  argument list.
//  args  number of arguments

static void free_arg_list(char **args, int argc)
{
	int  cnt = argc;

	if(args != NULL) {
		for(cnt = 0; cnt < argc; cnt++){
			kfree(args[cnt]);
		}

	    kfree(args);
	}
}

// Copy argument list from  userspace to kernel space
// Parameters
//			args   userspace parameters
//       argc   number of parameters
//       kargs  usespace parameters
//			return < 0 on error and **kargs = NULL

static int user_copy_arg_list(char **args, int argc, char ***kargs)
{
	char **largs;
	int err;
	int cnt;
	char *source;
	char buf[SYS_THREAD_ARG_LENGTH_MAX];

	*kargs = NULL;

	if(is_kernel_address(args))
		return ERR_VM_BAD_USER_MEMORY;

	largs = kmalloc((argc + 1) * sizeof(char *));
	if(largs == NULL){
		return ERR_NO_MEMORY;
	}

	// scan all parameters and copy to kernel space

	for(cnt = 0; cnt < argc; cnt++) {
		err = user_memcpy(&source, &(args[cnt]), sizeof(char *));
		if(err < 0)
			goto error;

		if(is_kernel_address(source)){
			err = ERR_VM_BAD_USER_MEMORY;
			goto error;
		}

		err = user_strncpy(buf,source, SYS_THREAD_ARG_LENGTH_MAX - 1);
		if(err < 0)
			goto error;
		buf[SYS_THREAD_ARG_LENGTH_MAX - 1] = 0;

		largs[cnt] = kstrdup(buf);
		if(largs[cnt] == NULL){
			err = ERR_NO_MEMORY;
			goto error;
		}
	}

	largs[argc] = NULL;

	*kargs = largs;
	return NO_ERROR;

error:
	free_arg_list(largs,cnt);
	dprintf("user_copy_arg_list failed %d \n",err);
	return err;
}

static unsigned int thread_struct_hash(void *_t, const void *_key, unsigned int range)
{
	struct thread *t = _t;
	const struct thread_key *key = _key;

	if(t != NULL)
		return (t->id % range);
	else
		return (key->id % range);
}

static struct thread *create_thread_struct(const char *name)
{
	struct thread *t;

	int_disable_interrupts();
	GRAB_THREAD_LOCK();
	t = thread_dequeue(&dead_q);
	RELEASE_THREAD_LOCK();
	int_restore_interrupts();

	if(t == NULL) {
		t = (struct thread *)kmalloc(sizeof(struct thread));
		if(t == NULL)
			goto err;
	}

	strncpy(&t->name[0], name, SYS_MAX_OS_NAME_LEN-1);
	t->name[SYS_MAX_OS_NAME_LEN-1] = 0;

	t->id = atomic_add(&next_thread_id, 1);
	t->proc = NULL;
	t->cpu = NULL;
	t->fpu_cpu = NULL;
	t->fpu_state_saved = true;
	t->sem_blocking = -1;
	t->fault_handler = 0;
	t->kernel_stack_region_id = -1;
	t->kernel_stack_base = 0;
	t->user_stack_region_id = -1;
	t->user_stack_base = 0;
	list_clear_node(&t->proc_node);
	t->priority = -1;
	t->args = NULL;
	t->sig_pending = 0;
	t->sig_block_mask = 0;
	memset(t->sig_action, 0, 32 * sizeof(struct sigaction));
	memset(&t->alarm_event, 0, sizeof(t->alarm_event));
	t->in_kernel = true;
	t->int_disable_level = 0;
	t->user_time = 0;
	t->kernel_time = 0;
	t->last_time = 0;
	t->last_time_type = KERNEL_TIME;
	{
		char temp[64];

		sprintf(temp, "thread_0x%x_retcode_sem", t->id);
		t->return_code_sem = sem_create(0, temp);
		if(t->return_code_sem < 0)
			goto err1;
	}

	if(arch_thread_init_thread_struct(t) < 0)
		goto err2;

	return t;

err2:
	sem_delete_etc(t->return_code_sem, -1);
err1:
	kfree(t);
err:
	return NULL;
}

static void delete_thread_struct(struct thread *t)
{
	if(t->return_code_sem >= 0)
		sem_delete_etc(t->return_code_sem, -1);
	kfree(t);
}

static int _create_user_thread_kentry(void)
{
	struct thread *t;

	// simulates the thread spinlock release that would occur if the thread had been
	// rescheded from. The resched didn't happen because the thread is new.
	RELEASE_THREAD_LOCK();
	int_restore_interrupts(); // this essentially simulates a return-from-interrupt

	t = thread_get_current_thread();

	// start tracking kernel time
	t->last_time = system_time();
	t->last_time_type = KERNEL_TIME;

	// a signal may have been delivered here
	thread_atkernel_exit();

	// jump to the entry point in user space
	arch_thread_enter_uspace(t, (addr_t)t->entry, t->args, t->user_stack_base + STACK_SIZE);

	// never get here, the thread will exit by calling the thread_exit syscall
	return 0;
}

static int _create_kernel_thread_kentry(void)
{
	int (*func)(void *args);
	struct thread *t;
	int retcode;

	// simulates the thread spinlock release that would occur if the thread had been
	// rescheded from. The resched didn't happen because the thread is new.
	RELEASE_THREAD_LOCK();
	int_restore_interrupts(); // this essentially simulates a return-from-interrupt

	// start tracking kernel time
	t = thread_get_current_thread();
	t->last_time = system_time();
	t->last_time_type = KERNEL_TIME;

	// call the entry function with the appropriate args
	func = (void *)t->entry;
	retcode = func(t->args);

	// we're done, exit
	thread_exit(retcode);

	// shoudn't get to here
	return 0;
}

static thread_id _create_thread(const char *name, proc_id pid, addr_t entry, void *args, bool kernel)
{
	struct thread *t;
	struct proc *p;
	char stack_name[64];
	bool abort = false;

	t = create_thread_struct(name);
	if(t == NULL)
		return ERR_NO_MEMORY;

	t->priority = THREAD_MEDIUM_PRIORITY;
	t->state = THREAD_STATE_BIRTH;
	t->next_state = THREAD_STATE_SUSPENDED;

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	// insert into global list
	hash_insert(thread_hash, t);
	RELEASE_THREAD_LOCK();

	GRAB_PROC_LOCK();
	// look at the proc, make sure it's not being deleted
	p = proc_get_proc_struct_locked(pid);
	if(p != NULL && p->state != PROC_STATE_DEATH) {
		insert_thread_into_proc(p, t);
	} else {
		abort = true;
	}
	RELEASE_PROC_LOCK();
	if(abort) {
		GRAB_THREAD_LOCK();
		hash_remove(thread_hash, t);
		RELEASE_THREAD_LOCK();
	}
	int_restore_interrupts();
	if(abort) {
		delete_thread_struct(t);
		return ERR_TASK_PROC_DELETED;
	}

	sprintf(stack_name, "%s_kstack", name);
	t->kernel_stack_region_id = vm_create_anonymous_region(vm_get_kernel_aspace_id(), stack_name,
		(void **)&t->kernel_stack_base, REGION_ADDR_ANY_ADDRESS, KSTACK_SIZE,
		REGION_WIRING_WIRED, LOCK_RW|LOCK_KERNEL);
	if(t->kernel_stack_region_id < 0)
		panic("_create_thread: error creating kernel stack!\n");

	t->args = args;
	t->entry = entry;

	if(kernel) {
		// this sets up an initial kthread stack that runs the entry
		arch_thread_initialize_kthread_stack(t, &_create_kernel_thread_kentry);
	} else {
		// create user stack
		// XXX make this better. For now just keep trying to create a stack
		// until we find a spot.
		t->user_stack_base = (USER_STACK_REGION  - STACK_SIZE) + USER_STACK_REGION_SIZE;
		while(t->user_stack_base > USER_STACK_REGION) {
			sprintf(stack_name, "%s_stack%d", p->name, t->id);
			t->user_stack_region_id = vm_create_anonymous_region(p->aspace_id, stack_name,
				(void **)&t->user_stack_base,
				REGION_ADDR_ANY_ADDRESS, STACK_SIZE, REGION_WIRING_LAZY, LOCK_RW);
			if(t->user_stack_region_id < 0) {
				t->user_stack_base -= STACK_SIZE;
			} else {
				// we created a region
				break;
			}
		}
		if(t->user_stack_region_id < 0)
			panic("_create_thread: unable to create user stack!\n");

		// copy the user entry over to the args field in the thread struct
		// the function this will call will immediately switch the thread into
		// user space.
		arch_thread_initialize_kthread_stack(t, &_create_user_thread_kentry);
	}

	// set the interrupt disable level of the new thread to one (as if it had had int_disable_interrupts called)
	t->int_disable_level = 1;

	// set the initial state of the thread to suspended
	t->state = THREAD_STATE_SUSPENDED;

	return t->id;
}

thread_id user_thread_create_user_thread(char *uname, addr_t entry, void *args)
{
	char name[SYS_MAX_OS_NAME_LEN];
	int rc;
	proc_id pid = thread_get_current_thread()->proc->id;

	if(is_kernel_address(uname))
		return ERR_VM_BAD_USER_MEMORY;
	if(is_kernel_address(entry))
		return ERR_VM_BAD_USER_MEMORY;

	rc = user_strncpy(name, uname, SYS_MAX_OS_NAME_LEN-1);
	if(rc < 0)
		return rc;
	name[SYS_MAX_OS_NAME_LEN-1] = 0;

	return thread_create_user_thread(name, pid, entry, args);
}

thread_id thread_create_user_thread(char *name, proc_id pid, addr_t entry, void *args)
{
	return _create_thread(name, pid, entry, args, false);
}

thread_id thread_create_kernel_thread(const char *name, int (*func)(void *), void *args)
{
	return _create_thread(name, proc_get_kernel_proc()->id, (addr_t)func, args, true);
}

static thread_id thread_create_kernel_thread_etc(const char *name, int (*func)(void *), void *args, struct proc *p)
{
	return _create_thread(name, p->id, (addr_t)func, args, true);
}

int thread_suspend_thread(thread_id id)
{
	return send_signal_etc(id, SIGSTOP, SIG_FLAG_NO_RESCHED);
}

thread_id thread_get_current_thread_id(void)
{
	struct thread *t = thread_get_current_thread();

	return t ? t->id : 0;
}

int thread_resume_thread(thread_id id)
{
	return send_signal_etc(id, SIGCONT, SIG_FLAG_NO_RESCHED);
}

int thread_set_priority(thread_id id, int priority)
{
	struct thread *t;
	int retval;

	// make sure the passed in priority is within bounds
	if(priority > THREAD_MAX_RT_PRIORITY)
		priority = THREAD_MAX_RT_PRIORITY;
	if(priority < THREAD_MIN_PRIORITY)
		priority = THREAD_MIN_PRIORITY;

	t = thread_get_current_thread();
	if(t->id == id) {
		// it's ourself, so we know we aren't in a run queue, and we can manipulate
		// our structure directly
		t->priority = priority;
		retval = NO_ERROR;
	} else {
		int_disable_interrupts();
		GRAB_THREAD_LOCK();

		t = thread_get_thread_struct_locked(id);
		if(t) {
			if(t->state == THREAD_STATE_READY && t->priority != priority) {
				// this thread is in a ready queue right now, so it needs to be reinserted
				thread_dequeue_thread(t);
				t->priority = priority;
				thread_enqueue_run_q(t);
			} else {
				t->priority = priority;
			}
			retval = NO_ERROR;
		} else {
			retval = ERR_INVALID_HANDLE;
		}

		RELEASE_THREAD_LOCK();
		int_restore_interrupts();
	}

	return retval;
}

int user_thread_set_priority(thread_id id, int priority)
{
	// clamp the priority levels the user can set their threads to
	if(priority > THREAD_MAX_PRIORITY)
		priority = THREAD_MAX_PRIORITY;
	return thread_set_priority(id, priority);
}

int thread_get_thread_info(thread_id id, struct thread_info *outinfo)
{
	struct thread *t;
	struct thread_info info;
	int err;

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	t = thread_get_thread_struct_locked(id);
	if(!t) {
		err = ERR_INVALID_HANDLE;
		goto out;
	}

	/* found the thread, copy the data out */
	info.id = id;
	info.owner_proc_id = t->proc->id;
	strncpy(info.name, t->name, SYS_MAX_OS_NAME_LEN-1);
	info.name[SYS_MAX_OS_NAME_LEN-1] = '\0';
	info.state = t->state;
	info.priority = t->priority;
	info.user_stack_base = t->user_stack_base;
	info.user_time = t->user_time;
	info.kernel_time = t->kernel_time;

	err = NO_ERROR;

out:
	RELEASE_THREAD_LOCK();
	int_restore_interrupts();

	if(err >= 0)
		memcpy(outinfo, &info, sizeof(info));

	return err;
}

int user_thread_get_thread_info(thread_id id, struct thread_info *uinfo)
{
	struct thread_info info;
	int err, err2;

	if(is_kernel_address(uinfo)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	err = thread_get_thread_info(id, &info);
	if(err < 0)
		return err;

	err2 = user_memcpy(uinfo, &info, sizeof(info));
	if(err2 < 0)
		return err2;

	return err;
}

int thread_get_next_thread_info(uint32 *_cookie, proc_id pid, struct thread_info *outinfo)
{
	struct thread *t;
	struct proc *p;
	struct thread_info info;
	int err;
	thread_id cookie;

	cookie = (thread_id)*_cookie;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(pid);
	if(!p) {
		err = ERR_INVALID_HANDLE;
		goto out;
	}

	/* find the next thread in the list of threads in the proc structure */
	t = NULL;
	if(cookie == 0) {
		t = list_peek_head_type(&p->thread_list, struct thread, proc_node);
	} else {
		list_for_every_entry(&p->thread_list, t, struct thread, proc_node) {
			if(t->id == cookie) {
				/* we found what the last search got us, walk one past the last search */
				t = list_next_type(&p->thread_list, &t->proc_node, struct thread, proc_node);
				break;
			}
		}
	}

	if(!t) {
		err = ERR_NOT_FOUND;
		goto out;
	}

	/* found the thread, copy the data out */
	info.id = t->id;
	info.owner_proc_id = t->proc->id;
	strncpy(info.name, t->name, SYS_MAX_OS_NAME_LEN-1);
	info.name[SYS_MAX_OS_NAME_LEN-1] = '\0';
	info.state = t->state;
	info.priority = t->priority;
	info.user_stack_base = t->user_stack_base;
	info.user_time = t->user_time;
	info.kernel_time = t->kernel_time;

	err = NO_ERROR;

	*_cookie = (uint32)t->id;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(err >= 0)
		memcpy(outinfo, &info, sizeof(info));

	return err;
}

int user_thread_get_next_thread_info(uint32 *ucookie, proc_id pid, struct thread_info *uinfo)
{
	struct thread_info info;
	uint32 cookie;
	int err, err2;

	if(is_kernel_address(ucookie)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	if(is_kernel_address(uinfo)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	err2 = user_memcpy(&cookie, ucookie, sizeof(cookie));
	if(err2 < 0)
		return err2;

	err = thread_get_next_thread_info(&cookie, pid, &info);
	if(err < 0)
		return err;

	err2 = user_memcpy(uinfo, &info, sizeof(info));
	if(err2 < 0)
		return err2;

	err2 = user_memcpy(ucookie, &cookie, sizeof(cookie));
	if(err2 < 0)
		return err2;

	return err;
}


static void _dump_proc_info(struct proc *p)
{
	dprintf("PROC: %p\n", p);
	dprintf("id:            0x%x\n", p->id);
	dprintf("pgid:          0x%x\n", p->pgid);
	dprintf("sid:           0x%x\n", p->sid);
	dprintf("name:          '%s'\n", p->name);
	dprintf("next:          %p\n", p->next);
	dprintf("parent:        %p (0x%x)\n", p->parent, p->parent ? p->parent->id : -1);
	dprintf("children.next: %p\n", p->children.next);
	dprintf("siblings.prev: %p\n", p->siblings_node.prev);
	dprintf("siblings.next: %p\n", p->siblings_node.next);
	dprintf("num_threads:   %d\n", p->num_threads);
	dprintf("state:         %d\n", p->state);
	dprintf("ioctx:         %p\n", p->ioctx);
	dprintf("aspace_id:     0x%x\n", p->aspace_id);
	dprintf("aspace:        %p\n", p->aspace);
	dprintf("kaspace:       %p\n", p->kaspace);
	dprintf("main_thread:   %p\n", p->main_thread);
	dprintf("thread_list.next: %p\n", p->thread_list.next);
}

static void dump_proc_info(int argc, char **argv)
{
	struct proc *p;
	int id = -1;
	unsigned long num;
	struct hash_iterator i;

	if(argc < 2) {
		dprintf("proc: not enough arguments\n");
		return;
	}

	// if the argument looks like a hex number, treat it as such
	if(strlen(argv[1]) > 2 && argv[1][0] == '0' && argv[1][1] == 'x') {
		num = atoul(argv[1]);
		if(num > vm_get_kernel_aspace()->virtual_map.base) {
			// XXX semi-hack
			_dump_proc_info((struct proc*)num);
			return;
		} else {
			id = num;
		}
	}

	// walk through the thread list, trying to match name or id
	hash_open(proc_hash, &i);
	while((p = hash_next(proc_hash, &i)) != NULL) {
		if((p->name && strcmp(argv[1], p->name) == 0) || p->id == id) {
			_dump_proc_info(p);
			break;
		}
	}
	hash_close(proc_hash, &i, false);
}


static const char *state_to_text(int state)
{
	switch(state) {
		case THREAD_STATE_READY:
			return "READY";
		case THREAD_STATE_RUNNING:
			return "RUNNING";
		case THREAD_STATE_WAITING:
			return "WAITING";
		case THREAD_STATE_SUSPENDED:
			return "SUSPEND";
		case THREAD_STATE_FREE_ON_RESCHED:
			return "DEATH";
		case THREAD_STATE_BIRTH:
			return "BIRTH";
		default:
			return "UNKNOWN";
	}
}

static struct thread *last_thread_dumped = NULL;

static void _dump_thread_info(struct thread *t)
{
	dprintf("THREAD: %p\n", t);
	dprintf("id:          0x%x\n", t->id);
	dprintf("name:        '%s'\n", t->name);
	dprintf("next:        %p\nproc_node.prev:  %p\nproc_node.next:  %p\nq_node.prev:     %p\nq_node.next:     %p\n",
		t->next, t->proc_node.prev, t->proc_node.next, t->q_node.prev, t->q_node.next);
	dprintf("priority:    0x%x\n", t->priority);
	dprintf("state:       %s\n", state_to_text(t->state));
	dprintf("next_state:  %s\n", state_to_text(t->next_state));
	dprintf("cpu:         %p ", t->cpu);
	if(t->cpu)
		dprintf("(%d)\n", t->cpu->cpu_num);
	else
		dprintf("\n");
	dprintf("sig_pending:  0x%lx\n", t->sig_pending);
	dprintf("sig_block_mask:  0x%lx\n", t->sig_block_mask);
	dprintf("in_kernel:   %d\n", t->in_kernel);
	dprintf("int_disable_level: %d\n", t->int_disable_level);
	dprintf("sem_blocking:0x%x\n", t->sem_blocking);
	dprintf("sem_count:   0x%x\n", t->sem_count);
	dprintf("sem_deleted_retcode: 0x%x\n", t->sem_deleted_retcode);
	dprintf("sem_errcode: 0x%x\n", t->sem_errcode);
	dprintf("sem_flags:   0x%x\n", t->sem_flags);
	dprintf("fault_handler: 0x%lx\n", t->fault_handler);
	dprintf("args:        %p\n", t->args);
	dprintf("entry:       0x%lx\n", t->entry);
	dprintf("proc:        %p\n", t->proc);
	dprintf("return_code_sem: 0x%x\n", t->return_code_sem);
	dprintf("kernel_stack_region_id: 0x%x\n", t->kernel_stack_region_id);
	dprintf("kernel_stack_base: 0x%lx\n", t->kernel_stack_base);
	dprintf("user_stack_region_id:   0x%x\n", t->user_stack_region_id);
	dprintf("user_stack_base:   0x%lx\n", t->user_stack_base);
	dprintf("kernel_time:       %Ld\n", t->kernel_time);
	dprintf("user_time:         %Ld\n", t->user_time);
	dprintf("architecture dependant section:\n");
	arch_thread_dump_info(&t->arch_info);

	last_thread_dumped = t;
}

static void dump_thread_info(int argc, char **argv)
{
	struct thread *t;
	int id = -1;
	unsigned long num;
	struct hash_iterator i;

	if(argc < 2) {
		dprintf("thread: not enough arguments\n");
		return;
	}

	// if the argument looks like a hex number, treat it as such
	if(strlen(argv[1]) > 2 && argv[1][0] == '0' && argv[1][1] == 'x') {
		num = atoul(argv[1]);
		if(num > vm_get_kernel_aspace()->virtual_map.base) {
			// XXX semi-hack
			_dump_thread_info((struct thread *)num);
			return;
		} else {
			id = num;
		}
	}

	// walk through the thread list, trying to match name or id
	hash_open(thread_hash, &i);
	while((t = hash_next(thread_hash, &i)) != NULL) {
		if((t->name && strcmp(argv[1], t->name) == 0) || t->id == id) {
			_dump_thread_info(t);
			break;
		}
	}
	hash_close(thread_hash, &i, false);
}

static void dump_thread_list(int argc, char **argv)
{
	struct thread *t;
	struct hash_iterator i;

	hash_open(thread_hash, &i);
	while((t = hash_next(thread_hash, &i)) != NULL) {
		dprintf("%p", t);
		if(t->name != NULL)
			dprintf("\t%32s", t->name);
		else
			dprintf("\t%32s", "<NULL>");
		dprintf("\t0x%x", t->id);
		dprintf("\t%16s", state_to_text(t->state));
		if(t->cpu)
			dprintf("\t%d", t->cpu->cpu_num);
		else
			dprintf("\tNOCPU");
		dprintf("\t0x%lx\n", t->kernel_stack_base);
	}
	hash_close(thread_hash, &i, false);
}

static void dump_next_thread_in_q(int argc, char **argv)
{
	struct thread *t = last_thread_dumped;

	if(t == NULL) {
		dprintf("no thread previously dumped. Examine a thread first.\n");
		return;
	}

	dprintf("next thread in queue after thread @ %p\n", t);
	if(t->q_node.next != NULL) {
		_dump_thread_info(containerof(t->q_node.next, struct thread, q_node)); // XXX fixme
	} else {
		dprintf("NULL\n");
	}
}

static void dump_next_thread_in_all_list(int argc, char **argv)
{
	struct thread *t = last_thread_dumped;

	if(t == NULL) {
		dprintf("no thread previously dumped. Examine a thread first.\n");
		return;
	}

	dprintf("next thread in global list after thread @ %p\n", t);
	if(t->next != NULL) {
		_dump_thread_info(t->next);
	} else {
		dprintf("NULL\n");
	}
}

static void dump_next_thread_in_proc(int argc, char **argv)
{
	struct thread *t = last_thread_dumped;

	if(t == NULL) {
		dprintf("no thread previously dumped. Examine a thread first.\n");
		return;
	}

	dprintf("next thread in proc after thread @ %p\n", t);

	t = list_next_type(&t->proc->thread_list, &t->proc_node, struct thread, proc_node);
	if(t)
		_dump_thread_info(t);
	else
		dprintf("NULL\n");
}

static int get_death_stack(void)
{
	int i;
	unsigned int bit;

	sem_acquire(death_stack_sem, 1);

	// grap the thread lock, find a free spot and release
	int_disable_interrupts();
	GRAB_THREAD_LOCK();
	bit = death_stack_bitmap;
	bit = (~bit)&~((~bit)-1);
	death_stack_bitmap |= bit;
	RELEASE_THREAD_LOCK();


	// sanity checks
	if( !bit ) {
		panic("get_death_stack: couldn't find free stack!\n");
	}
	if( bit & (bit-1)) {
		panic("get_death_stack: impossible bitmap result!\n");
	}


	// bit to number
	i= -1;
	while(bit) {
		bit >>= 1;
		i += 1;
	}

//	dprintf("get_death_stack: returning 0x%lx\n", death_stacks[i].address);

	return i;
}

static void put_death_stack_and_reschedule(unsigned int index)
{
//	dprintf("put_death_stack...: passed %d\n", index);

	if(index >= num_death_stacks)
		panic("put_death_stack: passed invalid stack index %d\n", index);

	if(!(death_stack_bitmap & (1 << index)))
		panic("put_death_stack: passed invalid stack index %d\n", index);

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	death_stack_bitmap &= ~(1 << index);

	sem_release_etc(death_stack_sem, 1, SEM_FLAG_NO_RESCHED);

	thread_resched();
}

int thread_init(kernel_args *ka)
{
	struct thread *t;
	struct pgid_node *pgnode;
	struct sid_node *snode;
	unsigned int i;

	dprintf("thread_init: entry\n");
	kprintf("initializing threading system...\n");

	// create the process hash table
	proc_hash = hash_init(15, offsetof(struct proc, next), &proc_struct_compare, &proc_struct_hash);

	// create the pgroup hash table
	pgid_hash = hash_init(15, offsetof(struct pgid_node, node), &pgid_node_compare, &pgid_node_hash);

	// create the session hash table
	sid_hash = hash_init(15, offsetof(struct sid_node, node), &sid_node_compare, &sid_node_hash);

	// create the kernel process
	kernel_proc = create_proc_struct("kernel", true);
	if(kernel_proc == NULL)
		panic("could not create kernel proc!\n");
	kernel_proc->state = PROC_STATE_NORMAL;

	// the kernel_proc is it's own parent
	kernel_proc->parent = kernel_proc;

	// it's part of the kernel process group
	pgnode = create_pgroup_struct(kernel_proc->id);
	hash_insert(pgid_hash, pgnode);
	add_proc_to_pgroup(kernel_proc, kernel_proc->id);

	// ditto with session
	snode = create_session_struct(kernel_proc->id);
	hash_insert(sid_hash, snode);
	add_proc_to_session(kernel_proc, kernel_proc->id);

	kernel_proc->ioctx = vfs_new_ioctx(NULL);
	if(kernel_proc->ioctx == NULL)
		panic("could not create ioctx for kernel proc!\n");

	// stick it in the process hash
	hash_insert(proc_hash, kernel_proc);

	// create the thread hash table
	thread_hash = hash_init(15, offsetof(struct thread, next),
		&thread_struct_compare, &thread_struct_hash);

	// zero out the run queues
	for(i = 0; i < THREAD_NUM_PRIORITY_LEVELS; i++) {
		list_initialize(&run_q[i]);
	}

	// zero out the dead thread structure q
	list_initialize(&dead_q);

	// allocate a snooze sem
	snooze_sem = sem_create(0, "snooze sem");
	if(snooze_sem < 0) {
		panic("error creating snooze sem\n");
		return snooze_sem;
	}

	// create an idle thread for each cpu
	for(i=0; i<ka->num_cpus; i++) {
		char temp[64];
		vm_region *region;

		sprintf(temp, "idle_thread%d", i);
		t = create_thread_struct(temp);
		if(t == NULL) {
			panic("error creating idle thread struct\n");
			return ERR_NO_MEMORY;
		}
		t->proc = proc_get_kernel_proc();
		t->priority = THREAD_IDLE_PRIORITY;
		t->state = THREAD_STATE_RUNNING;
		t->next_state = THREAD_STATE_READY;
		t->int_disable_level = 1; // ints are disabled until the int_restore_interrupts in main()
		t->last_time = system_time();
		sprintf(temp, "idle_thread%d_kstack", i);
		t->kernel_stack_region_id = vm_find_region_by_name(vm_get_kernel_aspace_id(), temp);
		region = vm_get_region_by_id(t->kernel_stack_region_id);
		if(!region) {
			panic("error finding idle kstack region\n");
		}
		t->kernel_stack_base = region->base;
		vm_put_region(region);
		hash_insert(thread_hash, t);
		insert_thread_into_proc(t->proc, t);
		idle_threads[i] = t;
		if(i == 0)
			arch_thread_set_current_thread(t);
		t->cpu = &cpu[i];
	}

	// create a set of death stacks
	num_death_stacks = smp_get_num_cpus();
	if(num_death_stacks > 8*sizeof(death_stack_bitmap)) {
		/*
		 * clamp values for really beefy machines
		 */
		num_death_stacks = 8*sizeof(death_stack_bitmap);
	}
	death_stack_bitmap = 0;
	death_stacks = (struct death_stack *)kmalloc(num_death_stacks * sizeof(struct death_stack));
	if(death_stacks == NULL) {
		panic("error creating death stacks\n");
		return ERR_NO_MEMORY;
	}
	{
		char temp[64];

		for(i=0; i<num_death_stacks; i++) {
			sprintf(temp, "death_stack%d", i);
			death_stacks[i].rid = vm_create_anonymous_region(vm_get_kernel_aspace_id(), temp,
				(void **)&death_stacks[i].address,
				REGION_ADDR_ANY_ADDRESS, KSTACK_SIZE, REGION_WIRING_WIRED, LOCK_RW|LOCK_KERNEL);
			if(death_stacks[i].rid < 0) {
				panic("error creating death stacks\n");
				return death_stacks[i].rid;
			}
			death_stacks[i].in_use = false;
		}
	}
	death_stack_sem = sem_create(num_death_stacks, "death_stack_noavail_sem");

	// set up some debugger commands
	dbg_add_command(dump_thread_list, "threads", "list all threads");
	dbg_add_command(dump_thread_info, "thread", "list info about a particular thread");
	dbg_add_command(dump_next_thread_in_q, "next_q", "dump the next thread in the queue of last thread viewed");
	dbg_add_command(dump_next_thread_in_all_list, "next_all", "dump the next thread in the global list of the last thread viewed");
	dbg_add_command(dump_next_thread_in_proc, "next_proc", "dump the next thread in the process of the last thread viewed");
	dbg_add_command(dump_proc_info, "proc", "list info about a particular process");

	// initialize the architectural specific thread routines
	arch_thread_init(ka);

	return 0;
}

int thread_init_percpu(int cpu_num)
{
	arch_thread_set_current_thread(idle_threads[cpu_num]);
	return 0;
}

// this starts the scheduler. Must be run under the context of
// the initial idle thread.
void thread_start_threading(void)
{
	// XXX may not be the best place for this
	// invalidate all of the other processors' TLB caches
	int_disable_interrupts();
	arch_cpu_global_TLB_invalidate();
	smp_send_broadcast_ici(SMP_MSG_GLOBAL_INVL_PAGE, 0, 0, 0, NULL, SMP_MSG_FLAG_SYNC);
	int_restore_interrupts();

	// start the other processors
	smp_send_broadcast_ici(SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	thread_resched();

	RELEASE_THREAD_LOCK();
	int_restore_interrupts();
}

int user_thread_snooze(bigtime_t time)
{
	thread_snooze(time);
	return NO_ERROR;
}

int thread_snooze(bigtime_t time)
{
	return sem_acquire_etc(snooze_sem, 1, SEM_FLAG_TIMEOUT|SEM_FLAG_INTERRUPTABLE, time, NULL);
}

int user_thread_yield(void)
{
	thread_yield();
	return NO_ERROR;
}

void thread_yield(void)
{
	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	thread_resched();

	RELEASE_THREAD_LOCK();
	int_restore_interrupts();
}

// NOTE: PROC_LOCK must be held
static bool check_for_pgrp_connection(pgrp_id pgid, pgrp_id check_for, struct proc *ignore_proc)
{
	struct pgid_node *node;
	struct proc *temp_proc;
	bool connection = false;

	if(ignore_proc)
		dprintf("check_for_pgrp_connection: pgid %d check for %d ignore_proc %d\n", pgid, check_for, ignore_proc->id);
	else
		dprintf("check_for_pgrp_connection: pgid %d check for %d\n", pgid, check_for);

	node = hash_lookup(pgid_hash, &pgid);
	if(node) {
		list_for_every_entry(&node->list, temp_proc, struct proc, pg_node) {
			ASSERT(temp_proc->pgid == pgid);
			dprintf(" looking at %d, pgid %d, ppgid %d\n", temp_proc->id, temp_proc->pgid, temp_proc->parent->pgid);
			if(temp_proc != ignore_proc && temp_proc->parent->pgid == check_for) {
				connection = true;
				break;
			}
		}
	}
	return connection;
}

// used to pass messages between thread_exit and thread_exit2
struct thread_exit_args {
	struct thread *t;
	region_id old_kernel_stack;
	unsigned int death_stack;
};

static void thread_exit2(void *_args)
{
	struct thread_exit_args args;

	// copy the arguments over, since the source is probably on the kernel stack we're about to delete
	memcpy(&args, _args, sizeof(struct thread_exit_args));

	// restore the interrupts
	int_restore_interrupts();

//	dprintf("thread_exit2, running on death stack 0x%lx\n", args.t->kernel_stack_base);

	// delete the old kernel stack region
//	dprintf("thread_exit2: deleting old kernel stack id 0x%x for thread 0x%x\n", args.old_kernel_stack, args.t->id);
	vm_delete_region(vm_get_kernel_aspace_id(), args.old_kernel_stack);

//	dprintf("thread_exit2: removing thread 0x%x from global lists\n", args.t->id);

	// remove this thread from all of the global lists
	int_disable_interrupts();
	GRAB_PROC_LOCK();
	remove_thread_from_proc(kernel_proc, args.t);
	RELEASE_PROC_LOCK();
	GRAB_THREAD_LOCK();
	hash_remove(thread_hash, args.t);
	RELEASE_THREAD_LOCK();

//	dprintf("thread_exit2: done removing thread from lists\n");

	// set the next state to be gone. Will return the thread structure to a ready pool upon reschedule
	args.t->next_state = THREAD_STATE_FREE_ON_RESCHED;

	// throw away our fpu context
	if(args.t->fpu_cpu) {
		args.t->fpu_cpu->fpu_state_thread = NULL;
		args.t->fpu_cpu = NULL;
		args.t->fpu_state_saved = true; // a lie actually
	}

	// return the death stack and reschedule one last time
	put_death_stack_and_reschedule(args.death_stack);
	// never get to here
	panic("thread_exit2: made it where it shouldn't have!\n");
}

void thread_exit(int retcode)
{
	struct thread *t = thread_get_current_thread();
	struct proc *p = t->proc;
	proc_id parent_pid = -1;
	bool delete_proc = false;
	unsigned int death_stack;

	dprintf("thread 0x%x exiting w/return code 0x%x\n", t->id, retcode);

	if(!kernel_startup && !int_are_interrupts_enabled())
		panic("thread_exit called with ints disabled\n");

	// boost our priority to get this over with
	thread_set_priority(t->id, THREAD_HIGH_PRIORITY);

	// cancel any pending alarms
	timer_cancel_event(&t->alarm_event);

	// delete the user stack region first
	if(p->aspace_id >= 0 && t->user_stack_region_id >= 0) {
		region_id rid = t->user_stack_region_id;
		t->user_stack_region_id = -1;
		vm_delete_region(p->aspace_id, rid);
	}

	if(p != kernel_proc) {
		// remove this thread from the current process and add it to the kernel
		// put the thread into the kernel proc until it dies
		int_disable_interrupts();
		GRAB_PROC_LOCK();
		remove_thread_from_proc(p, t);
		insert_thread_into_proc(kernel_proc, t);
		if(p->main_thread == t) {
			// this was main thread in this process
			delete_proc = true;
			p->state = PROC_STATE_DEATH;
		}

		RELEASE_PROC_LOCK();
		// swap address spaces, to make sure we're running on the kernel's pgdir
		vm_aspace_swap(kernel_proc->kaspace);
		int_restore_interrupts();
	}

	// delete the process
	if(delete_proc) {
		if(p->num_threads > 0) {
			// there are other threads still in this process,
			// cycle through and signal kill on each of the threads
			// XXX this can be optimized. There's got to be a better solution.
			struct thread *temp_thread;

			int_disable_interrupts();
			GRAB_PROC_LOCK();
			// we can safely walk the list because of the lock. no new threads can be created
			// because of the PROC_STATE_DEATH flag on the process
			list_for_every_entry(&p->thread_list, temp_thread, struct thread, proc_node) {
				thread_kill_thread_nowait(temp_thread->id);
			}

			RELEASE_PROC_LOCK();
			int_restore_interrupts();

			// Now wait for all of the threads to die
			// XXX block on a semaphore
			while((volatile int)p->num_threads > 0) {
				thread_snooze(10000); // 10 ms
			}
		}

		int_disable_interrupts();
		GRAB_PROC_LOCK();

		// see if the process group we are in is going to be orphaned
		// it's orphaned if no parent of any other process in the group is in the
		// same process group as our parent
		if(p->sid == p->parent->sid && p->pgid != p->parent->pgid) {
			if(!check_for_pgrp_connection(p->pgid, p->parent->pgid, p)) {
				dprintf("thread_exit: killing process %d orphans process group %d\n", p->id, p->pgid);
				send_pgrp_signal_etc_locked(p->pgid, SIGHUP, SIG_FLAG_NO_RESCHED);
				send_pgrp_signal_etc_locked(p->pgid, SIGCONT, SIG_FLAG_NO_RESCHED);
			}		
		}

		// remove us from the process list
		hash_remove(proc_hash, p);

		// reparent each of our children
		proc_reparent_children(p);

		// we're not part of our process groups and session anymore
		remove_proc_from_pgroup(p, p->pgid);
		remove_proc_from_session(p, p->sid);

		// remember who our parent was so we can send a signal
		parent_pid = p->parent->id;

		// remove us from our parent
		remove_proc_from_parent(p->parent, p);	

		RELEASE_PROC_LOCK();
		int_restore_interrupts();

		// clean up resources owned by the process
		vm_put_aspace(p->aspace);
		vm_delete_aspace(p->aspace_id);
		port_delete_owned_ports(p->id);
		sem_delete_owned_sems(p->id);
		vfs_free_ioctx(p->ioctx);
		kfree(p);
	}

	// send a signal to the parent
	send_proc_signal_etc(parent_pid, SIGCHLD, SIG_FLAG_NO_RESCHED);

	// delete the sem that others will use to wait on us and get the retcode
	{
		sem_id s = t->return_code_sem;

		t->return_code_sem = -1;
		sem_delete_etc(s, retcode);
	}

	// get_death_stack leaves interrupts disabled
	death_stack = get_death_stack();
	{
		struct thread_exit_args args;

		args.t = t;
		args.old_kernel_stack = t->kernel_stack_region_id;
		args.death_stack = death_stack;

		// set the new kernel stack officially to the death stack, wont be really switched until
		// the next function is called. This bookkeeping must be done now before a context switch
		// happens, or the processor will interrupt to the old stack
		t->kernel_stack_region_id = death_stacks[death_stack].rid;
		t->kernel_stack_base = death_stacks[death_stack].address;

		// we will continue in thread_exit2(), on the new stack
		arch_thread_switch_kstack_and_call(t->kernel_stack_base + KSTACK_SIZE, thread_exit2, &args);
	}

	panic("never can get here\n");
}

int thread_kill_thread(thread_id id)
{
	int status = send_signal_etc(id, SIGKILLTHR, SIG_FLAG_NO_RESCHED);
	if (status < 0)
		return status;

	if (id != thread_get_current_thread()->id)
		thread_wait_on_thread(id, NULL);

	return status;
}

int thread_kill_thread_nowait(thread_id id)
{
	return send_signal_etc(id, SIGKILLTHR, SIG_FLAG_NO_RESCHED);
}

int user_thread_wait_on_thread(thread_id id, int *uretcode)
{
	int retcode;
	int rc, rc2;

	if(is_kernel_address(uretcode))
		return ERR_VM_BAD_USER_MEMORY;

	rc = thread_wait_on_thread(id, &retcode);

	rc2 = user_memcpy(uretcode, &retcode, sizeof(retcode));
	if(rc2 < 0)
		return rc2;

	return rc;
}

int thread_wait_on_thread(thread_id id, int *retcode)
{
	sem_id sem;
	struct thread *t;
	int rc;

	rc = send_signal_etc(id, SIGCONT, 0);
	if (rc < NO_ERROR)
		return rc;

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	t = thread_get_thread_struct_locked(id);
	if(t != NULL) {
		sem = t->return_code_sem;
	} else {
		sem = ERR_INVALID_HANDLE;
	}

	RELEASE_THREAD_LOCK();
	int_restore_interrupts();

	rc = sem_acquire_etc(sem, 1, SEM_FLAG_INTERRUPTABLE, 0, retcode);

	/* This thread died the way it should, dont ripple a non-error up */
	if (rc == ERR_SEM_DELETED)
		rc = NO_ERROR;

	return rc;
}

int user_proc_wait_on_proc(proc_id id, int *uretcode)
{
	int retcode;
	int rc, rc2;

	if(is_kernel_address(uretcode))
		return ERR_VM_BAD_USER_MEMORY;

	rc = proc_wait_on_proc(id, &retcode);
	if(rc < 0)
		return rc;

	rc2 = user_memcpy(uretcode, &retcode, sizeof(retcode));
	if(rc2 < 0)
		return rc2;

	return rc;
}

int proc_wait_on_proc(proc_id id, int *retcode)
{
	struct proc *p;
	thread_id tid;

	int_disable_interrupts();
	GRAB_PROC_LOCK();
	p = proc_get_proc_struct_locked(id);
	if(p && p->main_thread) {
		tid = p->main_thread->id;
	} else {
		tid = ERR_INVALID_HANDLE;
	}
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(tid < 0)
		return tid;

	return thread_wait_on_thread(tid, retcode);
}

struct thread *thread_get_thread_struct(thread_id id)
{
	struct thread *t;

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	t = thread_get_thread_struct_locked(id);

	RELEASE_THREAD_LOCK();
	int_restore_interrupts();

	return t;
}

struct thread *thread_get_thread_struct_locked(thread_id id)
{
	struct thread_key key;

	key.id = id;

	return hash_lookup(thread_hash, &key);
}

// unused
#if 0
static struct proc *proc_get_proc_struct(proc_id id)
{
	struct proc *p;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(id);

	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	return p;
}
#endif

static struct proc *proc_get_proc_struct_locked(proc_id id)
{
	struct proc_key key;

	key.id = id;

	return hash_lookup(proc_hash, &key);
}

static void thread_context_switch(struct thread *t_from, struct thread *t_to)
{
	vm_translation_map *new_tmap;

	// track kernel time
	bigtime_t now = system_time();
	if(t_from->last_time_type == KERNEL_TIME)
		t_from->kernel_time += now - t_from->last_time;
	else
		t_from->user_time += now - t_from->last_time;
	t_to->last_time = now;

	// XXX remove this?

	// remember that this cpu will hold the current fpu state if 
	// a) it's not already saved in the thread structure
	// b) it's not on another cpu
	if(!t_from->fpu_state_saved) {
		if(t_from->fpu_cpu == NULL) {	// does another cpu "own" our state?
			cpu_ent *cpu = get_curr_cpu_struct();

			// the current cpu *has* to own our state
			ASSERT(cpu->fpu_state_thread == t_from);
		}
	}
 
	// set the current cpu and thread pointer
	t_to->cpu = t_from->cpu;
	arch_thread_set_current_thread(t_to);
	t_from->cpu = NULL;

	// decide if we need to switch to a new mmu context
	if(t_from->proc->aspace_id >= 0 && t_to->proc->aspace_id >= 0) {
		// they are both uspace threads
		if(t_from->proc->aspace_id == t_to->proc->aspace_id) {
			// same address space
			new_tmap = NULL;
		} else {
			// switching to a new address space
			new_tmap = &t_to->proc->aspace->translation_map;
		}
	} else if(t_from->proc->aspace_id < 0 && t_to->proc->aspace_id < 0) {
		// they must both be kspace threads
		new_tmap = NULL;
	} else if(t_to->proc->aspace_id < 0) {
		// the one we're switching to is kspace
		new_tmap = &t_to->proc->kaspace->translation_map;
	} else {
		new_tmap = &t_to->proc->aspace->translation_map;
	}

	// do the architecture specific context switch
	arch_thread_context_switch(t_from, t_to, new_tmap);
}

static int _rand(void)
{
	static int next = 0;

	if(next == 0)
		next = system_time();

	next = next * 1103515245 + 12345;
	return((next >> 16) & 0x7FFF);
}

static int reschedule_event(void *unused)
{
	// this function is called as a result of the timer event set by the scheduler
	// returning this causes a reschedule on the timer event
	thread_get_current_thread()->cpu->preempted= 1;
	return INT_RESCHEDULE;
}

// NOTE: expects thread_spinlock to be held
void thread_resched(void)
{
	struct thread *next_thread = NULL;
	int last_thread_pri = -1;
	struct thread *old_thread = thread_get_current_thread();
	int i;
	bigtime_t quantum;
	struct timer_event *quantum_timer;

//	dprintf("top of thread_resched: cpu %d, cur_thread = 0x%x\n", smp_get_current_cpu(), thread_get_current_thread());

	switch(old_thread->next_state) {
		case THREAD_STATE_RUNNING:
		case THREAD_STATE_READY:
//			dprintf("enqueueing thread 0x%x into run q. pri = %d\n", old_thread, old_thread->priority);
			thread_enqueue_run_q(old_thread);
			break;
		case THREAD_STATE_SUSPENDED:
			dprintf("suspending thread 0x%x\n", old_thread->id);
			break;
		case THREAD_STATE_FREE_ON_RESCHED:
			thread_enqueue(old_thread, &dead_q);
			break;
		default:
//			dprintf("not enqueueing thread 0x%x into run q. next_state = %d\n", old_thread, old_thread->next_state);
			;
	}
	old_thread->state = old_thread->next_state;

	// search the real-time queue
	for(i = THREAD_MAX_RT_PRIORITY; i >= THREAD_MIN_RT_PRIORITY; i--) {
		next_thread = thread_dequeue_run_q(i);
		if(next_thread)
			goto found_thread;
	}

	// search the regular queue
	for(i = THREAD_MAX_PRIORITY; i > THREAD_IDLE_PRIORITY; i--) {
		next_thread = thread_lookat_run_q(i);
		if(next_thread != NULL) {
			// skip it sometimes
			if(_rand() > 0x3000) {
				next_thread = thread_dequeue_run_q(i);
				goto found_thread;
			}
			last_thread_pri = i;
			next_thread = NULL;
		}
	}
	if(next_thread == NULL) {
		if(last_thread_pri != -1) {
			next_thread = thread_dequeue_run_q(last_thread_pri);
			if(next_thread == NULL)
				panic("next_thread == NULL! last_thread_pri = %d\n", last_thread_pri);
		} else {
			next_thread = thread_dequeue_run_q(THREAD_IDLE_PRIORITY);
			if(next_thread == NULL)
				panic("next_thread == NULL! no idle priorities!\n");
		}
	}

found_thread:
	next_thread->state = THREAD_STATE_RUNNING;
	next_thread->next_state = THREAD_STATE_READY;

	// XXX should only reset the quantum timer if we are switching to a new thread,
	// or we got here as a result of a quantum expire.

	// XXX calculate quantum
	quantum = 10000;

	// get the quantum timer for this cpu
	quantum_timer = &old_thread->cpu->quantum_timer;
	if(!old_thread->cpu->preempted) {
		_local_timer_cancel_event(old_thread->cpu->cpu_num, quantum_timer);
	}
	old_thread->cpu->preempted= 0;
	timer_setup_timer(&reschedule_event, NULL, quantum_timer);
	timer_set_event(quantum, TIMER_MODE_ONESHOT, quantum_timer);

	if(next_thread != old_thread) {
//		dprintf("thread_resched: cpu %d switching from thread %d to %d\n",
//			smp_get_current_cpu(), old_thread->id, next_thread->id);
		thread_context_switch(old_thread, next_thread);
	}
}

static void insert_proc_into_parent(struct proc *parent, struct proc *p)
{
	list_add_head(&parent->children, &p->siblings_node);
	p->parent = parent;
}

static void remove_proc_from_parent(struct proc *parent, struct proc *p)
{
	list_delete(&p->siblings_node);
	p->parent = NULL;
}

static int proc_struct_compare(void *_p, const void *_key)
{
	struct proc *p = _p;
	const struct proc_key *key = _key;

	if(p->id == key->id) return 0;
	else return 1;
}

static unsigned int proc_struct_hash(void *_p, const void *_key, unsigned int range)
{
	struct proc *p = _p;
	const struct proc_key *key = _key;

	if(p != NULL)
		return (p->id % range);
	else
		return (key->id % range);
}

struct proc *proc_get_kernel_proc(void)
{
	return kernel_proc;
}

proc_id proc_get_kernel_proc_id(void)
{
	if(!kernel_proc)
		return 0;
	else
		return kernel_proc->id;
}

proc_id proc_get_current_proc_id(void)
{
	return thread_get_current_thread()->proc->id;
}

struct proc *proc_get_current_proc(void)
{
	return thread_get_current_thread()->proc;
}

static struct proc *create_proc_struct(const char *name, bool kernel)
{
	struct proc *p;

	p = (struct proc *)kmalloc(sizeof(struct proc));
	if(p == NULL)
		goto error;
	p->next = NULL;
	list_clear_node(&p->siblings_node);
	list_initialize(&p->children);
	p->parent = NULL;
	p->id = atomic_add(&next_proc_id, 1);
	p->pgid = -1;
	p->sid = -1;
	list_clear_node(&p->pg_node);
	list_clear_node(&p->session_node);
	strncpy(&p->name[0], name, SYS_MAX_OS_NAME_LEN-1);
	p->name[SYS_MAX_OS_NAME_LEN-1] = 0;
	p->num_threads = 0;
	p->ioctx = NULL;
	p->aspace_id = -1;
	p->aspace = NULL;
	p->kaspace = vm_get_kernel_aspace();
	vm_put_aspace(p->kaspace);
	list_initialize(&p->thread_list);
	p->main_thread = NULL;
	p->state = PROC_STATE_BIRTH;

	if(arch_proc_init_proc_struct(p, kernel) < 0)
		goto error1;

	return p;

error1:
	kfree(p);
error:
	return NULL;
}

static void delete_proc_struct(struct proc *p)
{
	kfree(p);
}

int proc_get_proc_info(proc_id id, struct proc_info *outinfo)
{
	struct proc *p;
	struct proc_info info;
	int err;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(id);
	if(!p) {
		err = ERR_INVALID_HANDLE;
		goto out;
	}

	/* found the proc, copy the data out */
	info.pid = id;
	info.ppid = p->parent->id;
	info.pgid = p->pgid;
	info.sid = p->sid;
	strncpy(info.name, p->name, SYS_MAX_OS_NAME_LEN-1);
	info.name[SYS_MAX_OS_NAME_LEN-1] = '\0';
	info.state = p->state;
	info.num_threads = p->num_threads;

	err = NO_ERROR;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(err >= 0)
		memcpy(outinfo, &info, sizeof(info));

	return err;
}

int user_proc_get_proc_info(proc_id id, struct proc_info *uinfo)
{
	struct proc_info info;
	int err, err2;

	if(is_kernel_address(uinfo)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	err = proc_get_proc_info(id, &info);
	if(err < 0)
		return err;

	err2 = user_memcpy(uinfo, &info, sizeof(info));
	if(err2 < 0)
		return err2;

	return err;
}

int proc_get_next_proc_info(uint32 *cookie, struct proc_info *outinfo)
{
	struct proc *p;
	struct proc_info info;
	int err;
	struct hash_iterator i;
	proc_id id = (proc_id)*cookie;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	hash_open(proc_hash, &i);
	while((p = hash_next(proc_hash, &i)) != NULL) {
		if(id == 0)
			break; // initial search, return the first proc
		if(p->id == id) {
			// we found the last proc that was looked at, increment to the next one
			p = hash_next(proc_hash, &i);
			break;
		}
	}
	if(p == NULL) {
		err = ERR_NO_MORE_HANDLES;
		goto out;
	}

	// we have the proc structure, copy the data out of it
	info.pid = p->id;
	info.ppid = p->parent->id;
	info.pgid = p->pgid;
	info.sid = p->sid;
	strncpy(info.name, p->name, SYS_MAX_OS_NAME_LEN-1);
	info.name[SYS_MAX_OS_NAME_LEN-1] = '\0';
	info.state = p->state;
	info.num_threads = p->num_threads;

	err = 0;

	*cookie = (uint32)p->id;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(err >= 0)
		memcpy(outinfo, &info, sizeof(info));

	return err;
}

int user_proc_get_next_proc_info(uint32 *ucookie, struct proc_info *uinfo)
{
	struct proc_info info;
	uint32 cookie;
	int err, err2;

	if(is_kernel_address(ucookie)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	if(is_kernel_address(uinfo)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	err2 = user_memcpy(&cookie, ucookie, sizeof(cookie));
	if(err2 < 0)
		return err2;

	err = proc_get_next_proc_info(&cookie, &info);
	if(err < 0)
		return err;

	err2 = user_memcpy(uinfo, &info, sizeof(info));
	if(err2 < 0)
		return err2;

	err2 = user_memcpy(ucookie, &cookie, sizeof(cookie));
	if(err2 < 0)
		return err2;

	return err;
}

static int get_arguments_data_size(char **args,int argc)
{
	int cnt;
	int tot_size = 0;

	for(cnt = 0; cnt < argc; cnt++)
		tot_size += strlen(args[cnt]) + 1;
	tot_size += (argc + 1) * sizeof(char *);

	return tot_size + sizeof(struct uspace_prog_args_t);
}

static int proc_create_proc2(void *args)
{
	int err;
	struct thread *t;
	struct proc *p;
	struct proc_arg *pargs = args;
	char *path;
	addr_t entry;
	char ustack_name[128];
	int tot_top_size;
	char **uargs;
	char *udest;
	struct uspace_prog_args_t *uspa;
	unsigned int  cnt;

	t = thread_get_current_thread();
	p = t->proc;

	dprintf("proc_create_proc2: entry thread %d\n", t->id);

	// create an initial primary stack region

	tot_top_size = STACK_SIZE + PAGE_ALIGN(get_arguments_data_size(pargs->args,pargs->argc));
	t->user_stack_base = ((USER_STACK_REGION  - tot_top_size) + USER_STACK_REGION_SIZE);
	sprintf(ustack_name, "%s_primary_stack", p->name);
	t->user_stack_region_id = vm_create_anonymous_region(p->aspace_id, ustack_name, (void **)&t->user_stack_base,
		REGION_ADDR_EXACT_ADDRESS, tot_top_size, REGION_WIRING_LAZY, LOCK_RW);
	if(t->user_stack_region_id < 0) {
		panic("proc_create_proc2: could not create default user stack region\n");
		return t->user_stack_region_id;
	}

	uspa  = (struct uspace_prog_args_t *)(t->user_stack_base + STACK_SIZE);
	uargs = (char **)(uspa + 1);
	udest = (char  *)(uargs + pargs->argc + 1);
//	dprintf("addr: stack base=0x%x uargs = 0x%x  udest=0x%x tot_top_size=%d \n\n",t->user_stack_base,uargs,udest,tot_top_size);

	for(cnt = 0;cnt < pargs->argc;cnt++){
		uargs[cnt] = udest;
		user_strcpy(udest, pargs->args[cnt]);
		udest += strlen(pargs->args[cnt]) + 1;
	}
	uargs[cnt] = NULL;

	user_memcpy(uspa->prog_name, p->name, sizeof(uspa->prog_name));
	user_memcpy(uspa->prog_path, pargs->path, sizeof(uspa->prog_path));
	uspa->argc = cnt;
	uspa->argv = uargs;
	uspa->envc = 0;
	uspa->envp = 0;

	if(pargs->args != NULL)
		free_arg_list(pargs->args,pargs->argc);

	path = pargs->path;
	dprintf("proc_create_proc2: loading elf binary '%s'\n", path);

	err = elf_load_uspace("/boot/libexec/rld.so", p, 0, &entry);
	if(err < 0){
		// XXX clean up proc
		return err;
	}

	// free the args
	kfree(pargs->path);
	kfree(pargs);

	dprintf("proc_create_proc2: loaded elf. entry = 0x%lx\n", entry);

	p->state = PROC_STATE_NORMAL;

	// jump to the entry point in user space
	arch_thread_enter_uspace(t, entry, uspa, t->user_stack_base + STACK_SIZE);

	// never gets here
	return 0;
}

proc_id proc_create_proc(const char *path, const char *name, char **args, int argc, int priority, int flags)
{
	struct proc *p;
	struct proc *curr_proc;
	thread_id tid;
	proc_id pid;
	proc_id curr_proc_id;
	int err;
	struct proc_arg *pargs;
	struct sid_node *snode = NULL;
	struct pgid_node *pgnode = NULL;

	dprintf("proc_create_proc: entry '%s', name '%s' args = %p argc = %d, flags = 0x%x\n", path, name, args, argc, flags);

	p = create_proc_struct(name, false);
	if(p == NULL)
		return ERR_NO_MEMORY;

	pid = p->id;
	curr_proc_id = proc_get_current_proc_id();

	// preallocate a process group and session node if we need it
	if(flags & PROC_FLAG_NEW_SESSION) {
		snode = create_session_struct(p->id);
		flags |= PROC_FLAG_NEW_PGROUP; // creating your own session implies your own pgroup
	}	
	if(flags & PROC_FLAG_NEW_PGROUP)
		pgnode = create_pgroup_struct(p->id);

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	// insert this proc into the global list
	hash_insert(proc_hash, p);

	// add it to the parent's list
	curr_proc = proc_get_proc_struct_locked(curr_proc_id);
	insert_proc_into_parent(curr_proc, p);

	if(flags & PROC_FLAG_NEW_SESSION) {
		hash_insert(sid_hash, snode);
		add_proc_to_session(p, p->id);
	} else {
		// inheirit the parent's session
		p->sid = curr_proc->sid;
		add_proc_to_session(p, curr_proc->sid);
	}

	if(flags & PROC_FLAG_NEW_PGROUP) {
		hash_insert(pgid_hash, pgnode);
		add_proc_to_pgroup(p, p->id); 
	} else {
		// inheirit the creating processes's process group
		p->pgid = curr_proc->pgid;
		add_proc_to_pgroup(p, curr_proc->pgid);
	}

	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	// copy the args over
	pargs = kmalloc(sizeof(struct proc_arg));
	if(pargs == NULL){
		err = ERR_NO_MEMORY;
		goto err1;
	}
	pargs->path = kstrdup(path);
	if(pargs->path == NULL){
		err = ERR_NO_MEMORY;
		goto err2;
	}
	pargs->argc = argc;
	pargs->args = args;

	// create a new ioctx for this process
	p->ioctx = vfs_new_ioctx(thread_get_current_thread()->proc->ioctx);
	if(!p->ioctx) {
		err = ERR_NO_MEMORY;
		goto err3;
	}

	// create an address space for this process
	p->aspace_id = vm_create_aspace(p->name, USER_BASE, USER_BASE, USER_SIZE, false);
	if(p->aspace_id < 0) {
		err = p->aspace_id;
		goto err4;
	}
	p->aspace = vm_get_aspace_by_id(p->aspace_id);

	// create a kernel thread, but under the context of the new process
	tid = thread_create_kernel_thread_etc(name, proc_create_proc2, pargs, p);
	if(tid < 0) {
		err = tid;
		goto err5;
	}

	if((flags & PROC_FLAG_SUSPENDED) == 0)
		thread_resume_thread(tid);

	return pid;

err5:
	vm_put_aspace(p->aspace);
	vm_delete_aspace(p->aspace_id);
err4:
	vfs_free_ioctx(p->ioctx);
err3:
	kfree(pargs->path);
err2:
	kfree(pargs);
err1:
	// remove the proc structure from the proc hash table and delete the proc structure
	int_disable_interrupts();
	GRAB_PROC_LOCK();
	hash_remove(proc_hash, p);
	RELEASE_PROC_LOCK();
	int_restore_interrupts();
	delete_proc_struct(p);
//err:
	return err;
}

proc_id user_proc_create_proc(const char *upath, const char *uname, char **args, int argc, int priority, int flags)
{
	char path[SYS_MAX_PATH_LEN];
	char name[SYS_MAX_OS_NAME_LEN];
	char **kargs;
	int rc;

	dprintf("user_proc_create_proc : argc=%d \n",argc);

	if(is_kernel_address(upath))
		return ERR_VM_BAD_USER_MEMORY;
	if(is_kernel_address(uname))
		return ERR_VM_BAD_USER_MEMORY;

	rc = user_copy_arg_list(args, argc, &kargs);
	if(rc < 0)
		goto error;

	rc = user_strncpy(path, upath, SYS_MAX_PATH_LEN-1);
	if(rc < 0)
		goto error;

	path[SYS_MAX_PATH_LEN-1] = 0;

	rc = user_strncpy(name, uname, SYS_MAX_OS_NAME_LEN-1);
	if(rc < 0)
		goto error;

	name[SYS_MAX_OS_NAME_LEN-1] = 0;

	return proc_create_proc(path, name, kargs, argc, priority, flags);
error:
	free_arg_list(kargs,argc);
	return rc;
}

int proc_kill_proc(proc_id id)
{
	struct proc *p;
	thread_id tid = -1;
	int retval = 0;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(id);
	if(p != NULL) {
		tid = p->main_thread->id;
	} else {
		retval = ERR_INVALID_HANDLE;
	}

	RELEASE_PROC_LOCK();
	int_restore_interrupts();
	if(retval < 0)
		return retval;

	// just kill the main thread in the process. The cleanup code there will
	// take care of the process
	return thread_kill_thread(tid);
}

thread_id proc_get_main_thread(proc_id id)
{
	struct proc *p;
	thread_id tid;
	
	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(id);
	if(p != NULL) {
		tid = p->main_thread->id;
	} else {
		tid = ERR_INVALID_HANDLE;
	}

	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	return tid;
}

// reparent each of our children
// NOTE: must have PROC lock held
static void proc_reparent_children(struct proc *p)
{
	struct proc *child, *next;

	list_for_every_entry_safe(&p->children, child, next, struct proc, siblings_node) {
		// remove the child from the current proc and add to the parent
		remove_proc_from_parent(p, child);
		insert_proc_into_parent(p->parent, child);

		// check to see if this orphans the process group the child is in
		if(p->sid == child->sid && p->pgid != child->pgid) {
			if(!check_for_pgrp_connection(child->pgid, p->pgid, NULL)) {
				dprintf("thread_exit: killing process %d orphans process group %d\n", p->id, child->pgid);
				send_pgrp_signal_etc_locked(child->pgid, SIGHUP, SIG_FLAG_NO_RESCHED);
				send_pgrp_signal_etc_locked(child->pgid, SIGCONT, SIG_FLAG_NO_RESCHED);
			}
		}
	}
}

// called in the int handler code when a thread enters the kernel from user space (via syscall)
void thread_atkernel_entry(void)
{
	struct thread *t;
	bigtime_t now;

//	dprintf("thread_atkernel_entry: entry thread 0x%x\n", t->id);

	t = thread_get_current_thread();

	int_disable_interrupts();

	// track user time
	now = system_time();
	t->user_time += now - t->last_time;
	t->last_time = now;
	t->last_time_type = KERNEL_TIME;

	GRAB_THREAD_LOCK();

	t->in_kernel = true;

	RELEASE_THREAD_LOCK();
	int_restore_interrupts();
}

// called when a thread exits kernel space to user space
void thread_atkernel_exit(void)
{
	struct thread *t;
	int resched;
	bigtime_t now;

//	dprintf("thread_atkernel_exit: entry\n");

	t = thread_get_current_thread();

	int_disable_interrupts();
	GRAB_THREAD_LOCK();

	resched = handle_signals(t);

	if (resched)
		thread_resched();

	t->in_kernel = false;

	RELEASE_THREAD_LOCK();

	// track kernel time
	now = system_time();
	t->kernel_time += now - t->last_time;
	t->last_time = now;
	t->last_time_type = USER_TIME;

	int_restore_interrupts();

}

// called at the end of an interrupt routine, tries to deliver signals
int thread_atinterrupt_exit(void)
{
	int resched;
	struct thread *t;

	t = thread_get_current_thread();
	if(!t)
		return INT_NO_RESCHEDULE;

	GRAB_THREAD_LOCK();

	resched = handle_signals(t);

	RELEASE_THREAD_LOCK();

	return resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
}

int user_getrlimit(int resource, struct rlimit * urlp)
{
	int				ret;
	struct rlimit	rl;

	if (urlp == NULL) {
		return ERR_INVALID_ARGS;
	}
	if(is_kernel_address(urlp)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	ret = getrlimit(resource, &rl);

	if (ret == 0) {
		ret = user_memcpy(urlp, &rl, sizeof(struct rlimit));
		if (ret < 0) {
			return ret;
		}
		return 0;
	}

	return ret;
}

int getrlimit(int resource, struct rlimit * rlp)
{
	if (!rlp) {
		return -1;
	}

	switch(resource) {
		case RLIMIT_NOFILE:
			return vfs_getrlimit(resource, rlp);

		default:
			return -1;
	}

	return 0;
}

int user_setrlimit(int resource, const struct rlimit * urlp)
{
	int				err;
	struct rlimit	rl;

	if (urlp == NULL) {
		return ERR_INVALID_ARGS;
	}
	if(is_kernel_address(urlp)) {
		return ERR_VM_BAD_USER_MEMORY;
	}

	err = user_memcpy(&rl, urlp, sizeof(struct rlimit));
	if (err < 0) {
		return err;
	}

	return setrlimit(resource, &rl);
}

int setrlimit(int resource, const struct rlimit * rlp)
{
	if (!rlp) {
		return -1;
	}

	switch(resource) {
		case RLIMIT_NOFILE:
			return vfs_setrlimit(resource, rlp);

		default:
			return -1;
	}

	return 0;
}

static int pgid_node_compare(void *_p, const void *_key)
{
	struct pgid_node *p = _p;
	const pgrp_id *key = _key;

	if(p->id == *key) return 0;
	else return 1;
}

static unsigned int pgid_node_hash(void *_p, const void *_key, unsigned int range)
{
	struct pgid_node *p = _p;
	const pgrp_id *key = _key;

	if(p != NULL)
		return (p->id % range);
	else
		return (*key % range);
}

// assumes PROC_LOCK is held
static int add_proc_to_pgroup(struct proc *p, pgrp_id pgid)
{
	struct pgid_node *node = hash_lookup(pgid_hash, &pgid);

	if(!node)
		return ERR_NOT_FOUND;

	p->pgid = pgid;
	ASSERT(p->pg_node.next == NULL && p->pg_node.prev == NULL);
	list_add_head(&node->list, &p->pg_node);

	return 0;
}

static int remove_proc_from_pgroup(struct proc *p, pgrp_id pgid)
{
	struct pgid_node *node = hash_lookup(pgid_hash, &pgid);

	if(!node)
		return ERR_NOT_FOUND;

	ASSERT(p->pgid == pgid);
	list_delete(&p->pg_node);

	return 0;
}

static struct pgid_node *create_pgroup_struct(pgrp_id pgid)
{
	struct pgid_node *node = kmalloc(sizeof(struct pgid_node));
	if(!node)
		return NULL;

	node->id = pgid;
	list_clear_node(&node->node);
	list_initialize(&node->list);

	return node;
}

static int send_pgrp_signal_etc_locked(pgrp_id pgid, uint signal, uint32 flags)
{
	struct pgid_node *node;
	struct proc *p;
	int err = NO_ERROR;

	node = hash_lookup(pgid_hash, &pgid);
	if(!node) {
		err = ERR_NOT_FOUND;
		goto out;
	}

	list_for_every_entry(&node->list, p, struct proc, pg_node) {
		dprintf("send_pgrp_signal_etc: sending sig %d to proc %d in pgid %d\n", signal, p->id, pgid);
		send_signal_etc(p->main_thread->id, signal, flags | SIG_FLAG_NO_RESCHED);
	}

out:
	return err;
}

int send_pgrp_signal_etc(pgrp_id pgid, uint signal, uint32 flags)
{
	int err;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	err = send_pgrp_signal_etc_locked(pgid, signal, flags);

	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	return err;
}

static int sid_node_compare(void *_s, const void *_key)
{
	struct sid_node *s = _s;
	const sess_id *key = _key;

	if(s->id == *key) return 0;
	else return 1;
}

static unsigned int sid_node_hash(void *_s, const void *_key, unsigned int range)
{
	struct sid_node *s = _s;
	const sess_id *key = _key;

	if(s != NULL)
		return (s->id % range);
	else
		return (*key % range);
}

// assumes PROC_LOCK is held
static int add_proc_to_session(struct proc *p, sess_id sid)
{
	struct sid_node *node = hash_lookup(sid_hash, &sid);
	if(!node)
		return ERR_NOT_FOUND;

	p->sid = sid;
	ASSERT(p->session_node.next == NULL && p->session_node.prev == NULL);
	list_add_head(&node->list, &p->session_node);

	return 0;
}

static int remove_proc_from_session(struct proc *p, sess_id sid)
{
	struct sid_node *node = hash_lookup(sid_hash, &sid);
	if(!node)
		return ERR_NOT_FOUND;

	ASSERT(p->sid == sid);
	list_delete(&p->session_node);

	return 0;
}

static struct sid_node *create_session_struct(sess_id sid)
{
	struct sid_node *node = kmalloc(sizeof(struct sid_node));
	if(!node)
		return NULL;

	node->id = sid;
	list_clear_node(&node->node);
	list_initialize(&node->list);

	return node;
}

int send_session_signal_etc(sess_id sid, uint signal, uint32 flags)
{
	struct sid_node *node;
	struct proc *p;
	int err = NO_ERROR;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	node = hash_lookup(sid_hash, &sid);
	if(!node) {
		err = ERR_NOT_FOUND;
		goto out;
	}

	list_for_every_entry(&node->list, p, struct proc, session_node) {
		send_proc_signal_etc(p->main_thread->id, signal, flags | SIG_FLAG_NO_RESCHED);
	}

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	return err;
}

int setpgid(proc_id pid, pgrp_id pgid)
{
	struct proc *p;
	struct pgid_node *free_node = NULL;
	int err;

	if(pid < 0 || pgid < 0)
		return ERR_INVALID_ARGS;

	if(pid == 0)
		pid = proc_get_current_proc_id();

	if(pgid == 0)
		pgid = pid;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(pid);
	if(!p) {
		err = ERR_NOT_FOUND;
		goto out;
	}

	// see if it's already in the target process group
	if(p->pgid == pgid) {
		err = NO_ERROR;
		goto out;
	}

	// see if the target process group exists
	if(hash_lookup(pgid_hash, &pgid) == NULL) {
		// create it
		// NOTE, we need to release the proc spinlock because we might have to
		// block while allocating the node for the process group
		struct pgid_node *node;

		RELEASE_PROC_LOCK();
		int_restore_interrupts();

		node = create_pgroup_struct(pgid);
		if(!node) {
			err = ERR_NO_MEMORY;
			goto out2;
		}			

		int_disable_interrupts();
		GRAB_PROC_LOCK();

		// check before we add the newly created pgroup struct to the hash.
		// it could have been created while we had the PROC_LOCK released.
		if(hash_lookup(pgid_hash, &pgid) != NULL) {
			free_node = node; // erase it later and use the pgroup that was already added
		} else {
			// add our new pgroup node to the list
			hash_insert(pgid_hash, node); 
		}
	}

	// remove the process from it's current group
	remove_proc_from_pgroup(p, p->pgid);

	// add it to the new group
	add_proc_to_pgroup(p, pgid);

	err = NO_ERROR;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(free_node)
		kfree(free_node);

out2:
	return err;
}

pgrp_id getpgid(proc_id pid)
{
	struct proc *p;
	pgrp_id retval;

	if(pid < 0)
		return ERR_INVALID_ARGS;

	if(pid == 0)
		pid = proc_get_current_proc_id();
	
	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(pid);
	if(!p) {
		retval = ERR_NOT_FOUND;
		goto out;
	}

	retval = p->pgid;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	return retval;
}

sess_id setsid(void)
{
	struct proc *p;
	struct sid_node *free_node = NULL;
	proc_id pid;
	sess_id sid;
	int err;

	pid = proc_get_current_proc_id();
	sid = pid;

	int_disable_interrupts();
	GRAB_PROC_LOCK();

	p = proc_get_proc_struct_locked(pid);
	if(!p) {
		err = ERR_NOT_FOUND;
		goto out;
	}

	// see if it's already in the target session
	if(p->sid == sid) {
		err = NO_ERROR;
		goto out;
	}

	// see if the target session exists
	if(hash_lookup(sid_hash, &sid) == NULL) {
		// create it
		// NOTE, we need to release the proc spinlock because we might have to
		// block while allocating the node for the session
		struct sid_node *node;

		RELEASE_PROC_LOCK();
		int_restore_interrupts();

		node = create_session_struct(sid);
		if(!node) {
			err = ERR_NO_MEMORY;
			goto out2;
		}			

		int_disable_interrupts();
		GRAB_PROC_LOCK();

		// check before we add the newly created pgroup struct to the hash.
		// it could have been created while we had the PROC_LOCK released.
		if(hash_lookup(sid_hash, &sid) != NULL) {
			free_node = node; // erase it later and use the pgroup that was already added
		} else {
			// add our new pgroup node to the list
			hash_insert(sid_hash, node); 
		}
	}

	// remove the process from it's current group
	remove_proc_from_session(p, p->sid);

	// add it to the new group
	add_proc_to_session(p, sid);

	err = NO_ERROR;

out:
	RELEASE_PROC_LOCK();
	int_restore_interrupts();

	if(free_node)
		kfree(free_node);

out2:
	return err;
}

