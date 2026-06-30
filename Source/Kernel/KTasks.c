#include <KTasks.h>
#include <Kernel/Scheduler.h>
#include <Console.h>

NOPTR KTasksCreateAll(NOPTR) {
    // Idle
    SchedulerCreateIdleTask();
    // System64
    SchedulerCreateTask("System64", MainWorkerEntry, NULLPTR,
			  SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    // PciInit64
    SchedulerCreateTask("PciInit64", PciInitTask, NULLPTR,
		 	  SCHED_PRIORITY_HIGH, TASK_DEFAULT_QUANTUM);
    SchedulerCreateTask("Init64", InitTask, NULLPTR,
                          SCHED_PRIORITY_HIGH, TASK_DEFAULT_QUANTUM);
    SchedulerCreateTask("HCBlink", HCBlinkTask, NULLPTR,
                        SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    SchedulerCreateTask("Shell64", Shell64Entry, NULLPTR,
                          SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    return;
}
