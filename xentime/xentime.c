/******************************************************************************
 * xentime.c
 * 
 * Xen time driver - enables getting runstate information from hypervisor.
 * 
 *  Copyright (c) 2013, Daniil Cherednik
 *  
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/sys/dev/xen/xentime/xentime.c,v $");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/smp.h>

#include <libkern/quad.h>

#include <machine/xen/xen-os.h>
#include <machine/xen/xenvar.h>
#include <machine/xen/xenfunc.h>
#include <xen/hypervisor.h>
#include <xen/interface/vcpu.h>

#include <vm/vm.h>
#include <vm/vm_page.h>

#define NS_PER_TICK (1000000000ULL/hz)

static int xentime_init_runstate_info(int cpu);
static int xentime_get_runstate_info(int cpu, struct vcpu_runstate_info *runstate);
static int xentime_do_init(void);
static void xentime_do_stolen_accounting(int cpu);
static void xentime_increase_counter(int ticks);
static struct vcpu_runstate_info last_runstate[MAXCPU];
static int xentime_thread_flag = -1;
static struct mtx xentime_thread_lock;
static struct proc *xentime_proc;


static int xentime_stolen_ticks = 0;
SYSCTL_INT(_debug, OID_AUTO, stolen_ticks, CTLFLAG_RD,
           &xentime_stolen_ticks, 0,
	              "Counter of CPUs ticks stolen by XEN hypervisor");
static int
xentime_init_runstate_info(int cpu)
{
	return xentime_get_runstate_info(cpu, &last_runstate[cpu]);
}

static int
xentime_get_runstate_info(int cpu, struct vcpu_runstate_info *runstate)
{
	return HYPERVISOR_vcpu_op(VCPUOP_get_runstate_info,
				cpu, runstate);
}

static void
xentime_increase_counter(int ticks)
{
	xentime_stolen_ticks += ticks;
}

static void
xentime_do_stolen_accounting(int cpu)
{
	struct vcpu_runstate_info cur_runstate;
	long long unsigned int runnable, offline;
	static long long unsigned int stolen[MAXCPU];
	uint32_t ticks;
	/* get current runstate */
	xentime_get_runstate_info(cpu, &cur_runstate);

#ifdef notyet	
	blocked = cur_runstate.time[RUNSTATE_blocked] - last_runstate[cpu].time[RUNSTATE_blocked];
#endif
	runnable = cur_runstate.time[RUNSTATE_runnable] - last_runstate[cpu].time[RUNSTATE_runnable];
	offline = cur_runstate.time[RUNSTATE_offline] - last_runstate[cpu].time[RUNSTATE_offline];

	memcpy(&last_runstate[cpu], &cur_runstate, sizeof(cur_runstate));

	stolen[cpu] = runnable + offline + stolen[cpu];
	if (stolen[cpu] < 0)
		stolen[cpu] = 0;
	ticks = __qdivrem(stolen[cpu], NS_PER_TICK, &stolen[cpu]);
	xentime_increase_counter(ticks);
}

static int
xentime_do_init(void)
{
	int i;
	int rc = 0;
	CPU_FOREACH(i)
		if((rc = xentime_init_runstate_info(i)))
		{
			printf("Err. Can`t make hypercall: %i \n", rc);
			break;
		}
	return rc;
}

static void
xentime_do_thread(void *unused)
{
	int i;
	mtx_lock(&xentime_thread_lock);
	while(!xentime_thread_flag)
	{
                msleep(xentime_do_thread, &xentime_thread_lock, 0, "xentime_do_thread",
		                       hz);
		CPU_FOREACH(i)
			xentime_do_stolen_accounting(i);
	}
	mtx_unlock(&xentime_thread_lock);
	kproc_exit(0);
}



static int event_handler(struct module *module, int event, void *arg)
{
	int rc, e = 0;
	switch (event)
	{
		case MOD_LOAD:
			mtx_init(&xentime_thread_lock, "xentime_thread_lock", NULL, MTX_DEF);
			if(xentime_do_init() == 0)
				xentime_thread_flag = 0;
				kproc_create(xentime_do_thread, NULL, &xentime_proc, 0, 0, "xentime_conf");
		break;
		case MOD_UNLOAD:
		        xentime_thread_flag = -1;
			mtx_lock(&xentime_thread_lock);
			if ((rc = mtx_sleep(&xentime_proc->p_stype, &xentime_thread_lock, 0, "waiting", 0)))
			{
				printf("Can`t finish thread, sorry... %i\n", rc);
				e = EPERM;
			}
			mtx_destroy(&xentime_thread_lock);
		break;
		default:
			e = EOPNOTSUPP;
		break;
	}

	return(e);
}

static moduledata_t xentime_conf = {
	"xentime",
	event_handler,
	NULL
};

DECLARE_MODULE(xentime, xentime_conf, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);





