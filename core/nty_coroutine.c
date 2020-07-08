/*
 *  Author : WangBoJing , email : 1989wangbojing@gmail.com
 * 
 *  Copyright Statement:
 *  --------------------
 *  This software is protected by Copyright and the information contained
 *  herein is confidential. The software may not be copied and the information
 *  contained herein may not be used or disclosed except with the written
 *  permission of Author. (C) 2017
 * 
 *

****       *****                                      *****
  ***        *                                       **    ***
  ***        *         *                            *       **
  * **       *         *                           **        **
  * **       *         *                          **          *
  *  **      *        **                          **          *
  *  **      *       ***                          **
  *   **     *    ***********    *****    *****  **                   ****
  *   **     *        **           **      **    **                 **    **
  *    **    *        **           **      *     **                 *      **
  *    **    *        **            *      *     **                **      **
  *     **   *        **            **     *     **                *        **
  *     **   *        **             *    *      **               **        **
  *      **  *        **             **   *      **               **        **
  *      **  *        **             **   *      **               **        **
  *       ** *        **              *  *       **               **        **
  *       ** *        **              ** *        **          *   **        **
  *        ***        **               * *        **          *   **        **
  *        ***        **     *         **          *         *     **      **
  *         **        **     *         **          **       *      **      **
  *         **         **   *          *            **     *        **    **
*****        *          ****           *              *****           ****
                                       *
                                      *
                                  *****
                                  ****



 *
 */

#include "nty_coroutine.h"

pthread_key_t global_sched_key;
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;


int _switch(nty_cpu_ctx *new_ctx, nty_cpu_ctx *cur_ctx);

static void _exec(void *lt) {
#if defined(__lvm__) && defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" (lt));
#endif

	nty_coroutine *co = (nty_coroutine*)lt;
	co->func(co->arg);
	co->status |= (BIT(NTY_COROUTINE_STATUS_EXITED) | BIT(NTY_COROUTINE_STATUS_FDEOF) | BIT(NTY_COROUTINE_STATUS_DETACH));
#if 1
	nty_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

extern int nty_schedule_create(int stack_size);



void nty_coroutine_free(nty_coroutine *co) {
	if (co == NULL) return ;
	co->sched->spawned_coroutines --;
#if 1
	if (co->stack) {
		free(co->stack);
		co->stack = NULL;
	}
#endif
	if (co) {
		free(co);
	}

}

static void nty_coroutine_init(nty_coroutine *co) {

	void **stack = (void **)(co->stack + co->stack_size);

	stack[-3] = NULL;
	stack[-2] = (void *)co;

	co->ctx.esp = (void*)stack - (4 * sizeof(void*));
	co->ctx.ebp = (void*)stack - (3 * sizeof(void*));
	co->ctx.eip = (void*)_exec;
	co->status = BIT(NTY_COROUTINE_STATUS_READY);
	
}

void nty_coroutine_yield(nty_coroutine *co) {
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
}

static inline void nty_coroutine_madvise(nty_coroutine *co) {

	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp;
	assert(current_stack <= co->stack_size);

	if (current_stack < co->last_stack_size &&
		co->last_stack_size > co->sched->page_size) {
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1));
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
	}
	co->last_stack_size = current_stack;
}

int nty_coroutine_resume(nty_coroutine *co) {
	
	if (co->status & BIT(NTY_COROUTINE_STATUS_NEW)) {
		nty_coroutine_init(co);
	}

	nty_schedule *sched = nty_coroutine_get_sched();
	sched->curr_thread = co;
	_switch(&co->ctx, &co->sched->ctx);
	sched->curr_thread = NULL;

	nty_coroutine_madvise(co);
#if 1
	if (co->status & BIT(NTY_COROUTINE_STATUS_EXITED)) {
		if (co->status & BIT(NTY_COROUTINE_STATUS_DETACH)) {
			printf("nty_coroutine_resume --> \n");
			nty_coroutine_free(co);
		}
		return -1;
	} 
#endif
	return 0;
}


void nty_coroutine_renice(nty_coroutine *co) {
	co->ops ++;
#if 1
	if (co->ops < 5) return ;
#endif
	printf("nty_coroutine_renice\n");
	TAILQ_INSERT_TAIL(&nty_coroutine_get_sched()->ready, co, ready_next);
	printf("nty_coroutine_renice 111\n");
	nty_coroutine_yield(co);
}


void nty_coroutine_sleep(uint64_t msecs) {
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;

	if (msecs == 0) {
		TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);
		nty_coroutine_yield(co);
	} else {
		nty_schedule_sched_sleepdown(co, msecs);
	}
}

void nty_coroutine_detach(void) {
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
	co->status |= BIT(NTY_COROUTINE_STATUS_DETACH);
}

static void nty_coroutine_sched_key_destructor(void *data) {
	free(data);
}

static void nty_coroutine_sched_key_creator(void) {
	assert(pthread_key_create(&global_sched_key, nty_coroutine_sched_key_destructor) == 0);
	assert(pthread_setspecific(global_sched_key, NULL) == 0);
	
	return ;
}


// coroutine --> 
// create 
//
int nty_coroutine_create(nty_coroutine **new_co, proc_coroutine func, void *arg) {

	assert(pthread_once(&sched_key_once, nty_coroutine_sched_key_creator) == 0);
	nty_schedule *sched = nty_coroutine_get_sched();

	if (sched == NULL) {
		nty_schedule_create(0);
		
		sched = nty_coroutine_get_sched();
		if (sched == NULL) {
			printf("Failed to create scheduler\n");
			return -1;
		}
	}

	nty_coroutine *co = calloc(1, sizeof(nty_coroutine));
	if (co == NULL) {
		printf("Failed to allocate memory for new coroutine\n");
		return -2;
	}

	int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size);
	if (ret) {
		printf("Failed to allocate stack for new coroutine\n");
		free(co);
		return -3;
	}

	co->sched = sched;
	co->stack_size = sched->stack_size;
	co->status = BIT(NTY_COROUTINE_STATUS_NEW); //
	co->id = sched->spawned_coroutines ++;
	co->func = func;
#if CANCEL_FD_WAIT_UINT64
	co->fd = -1;
	co->events = 0;
#else
	co->fd_wait = -1;
#endif
	co->arg = arg;
	co->birth = nty_coroutine_usec_now();
	*new_co = co;

	TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);

	return 0;
}




