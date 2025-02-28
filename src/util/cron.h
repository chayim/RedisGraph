/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#pragma once

#include "heap.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

// CRON is a task scheduler
// a task is defined by:
// when it should run; delta in ms from the time it's introduced
// a callback to call when it is time to execute the task
// and an optional private data passed to the callback

// task callback function
typedef void (*CronTaskCB)(void *pdata);
typedef uintptr_t CronTaskHandle;

// start CRON, should be called once
void Cron_Start(void);

// stop CRON
void Cron_Stop(void);

// create a new CRON task
CronTaskHandle Cron_AddTask
(
	uint when,      // number of miliseconds until task invocation
	CronTaskCB cb,  // callback to call when task is due
	void *pdata     // private data to pass to callback
);

// aborts CRON task
// waits for task to complete in-case it is already executing
// otherwise marks task as aborted
void Cron_AbortTask
(
	CronTaskHandle t
);

