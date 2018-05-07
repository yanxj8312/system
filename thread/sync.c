#include "sync.h"
#include "list.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"

void sema_init(struct semaphore* psema, uint8_t value) {
	psema->value = value;		//信号量赋初始值
	list_init(&psema->waiters);	//初始化信号量的等待队列
}

/*初始化锁plock*/
void lock_init(struct lock* plock) {
	plock->holder = NULL;
	plock->holder_repeat_nr = 0;
	sema_init(&plock->semaphore,1);	//信号量初值为1
}

/*信号量down操作*/
void sema_down(struct semaphore* psema) {
/*关中断来保证原子操作*/
	enum intr_status old_status = intr_disable();
	while(psema->value == 0) {	//若value为0,表示已经被别的线程持有 线程被唤醒之后还需要进行竞争，所以使用while进行判断
		ASSERT(!elem_find(&psema->waiters, &running_thread()->general_tag));
		/*当前线程不应该已在信号量的waiters队列中*/
		if(elem_find(&psema->waiters, &running_thread()->general_tag)) {
			PANIC("sema_down: thread blocked has been in waiters_list\n");
		}
	/*若信号量的之等于0,则将当前线程加入到该锁的等待队列中，然后阻塞自己*/
		list_append(&psema->waiters, &running_thread()->general_tag);
		thread_block(TASK_BLOCKED);	//阻塞线程，知道为唤醒
	}
/*若value为1或被换下后，会执行下面的代码，也就是获得了锁*/
	psema->value--;
	ASSERT(psema->value == 0);
/*恢复之前的中断状态*/
	intr_set_status(old_status);
}

/*信号量的up操作*/
void sema_up(struct semaphore* psema) {
	/*关中断，保证原子操作*/
	enum intr_status old_status = intr_disable();
	ASSERT(psema->value == 0);
	if(!list_empty(&psema->waiters)) {
		struct task_struct* thread_blocked = elem2entry(struct task_struct, general_tag, list_pop(&psema->waiters));
		thread_unblock(thread_blocked);
	}
	psema->value++;
	ASSERT(psema->value == 1);
	/*恢复之前中断的状态*/
	intr_set_status(old_status);
}

/*获取锁plock*/
void lock_acquire(struct lock* plock) {
/*排除曾经自己已经持有锁还未释放的情况*/
	if(plock->holder != running_thread()) {
		sema_down(&plock->semaphore);	//对信号量p操作，原子操作
		plock->holder = running_thread();
		ASSERT(plock->holder_repeat_nr == 0);
		plock->holder_repeat_nr = 1;
	} else {
		plock->holder_repeat_nr++;
	}
}

/*释放锁plock*/
void lock_release(struct lock* plock) {
	ASSERT(plock->holder == running_thread());
	if(plock->holder_repeat_nr > 1) {
		plock->holder_repeat_nr--;
		return ;
	}
	ASSERT(plock->holder_repeat_nr == 1);	
	plock->holder = NULL;			//把锁置空放在v操作之前
	plock->holder_repeat_nr = 0;
	sema_up(&plock->semaphore);	//信号量的v操作原子操作
}




