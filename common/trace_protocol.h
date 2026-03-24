#ifndef TRACE_PROTOCOL_H
#define TRACE_PROTOCOL_H

#include <stdio.h>
#include <stdint.h>

/* ─── Trace event protocol ─────────────────────────────────────────────────
 *
 * All events are single lines starting with '#' so the host GUI can
 * distinguish them from human-readable printf output.
 *
 * Format: #<TYPE>:<field1>,<field2>,...\n
 *
 * Event types:
 *   #TR  Task Registered    id,name,period,deadline,wcet,type
 *   #TS  Task Switch        tick,name
 *   #JS  Job Start          tick,name,abs_deadline
 *   #JC  Job Complete       tick,name
 *   #DM  Deadline Miss      tick,name,abs_deadline,miss_count
 *   #RL  Resource Lock      tick,name,res_idx,ceiling
 *   #RU  Resource Unlock    tick,name,res_idx,ceiling
 *   #AR  Admission Result   name,result,util_num,util_den
 *   #IN  Info message       text
 *   #ID  Identification     task_label,scenario_number
 * ──────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* Call once before any trace output. */
void vTraceInit( void );

/* Enable / disable trace output at runtime. */
void vTraceEnable( void );
void vTraceDisable( void );

/* ─── Explicit event emitters (called from demo code) ────────────────── */

void vTraceRegisterTask( const char *pcName,
                         unsigned long ulPeriod,
                         unsigned long ulDeadline,
                         unsigned long ulWCET,
                         const char *pcType );

void vTraceEmitJobStart( const char *pcName,
                         unsigned long ulAbsDeadline );

void vTraceEmitJobComplete( const char *pcName );

void vTraceEmitAdmission( const char *pcName,
                          int xResult,
                          unsigned long ulUtilNum,
                          unsigned long ulUtilDen );

void vTraceEmitInfo( const char *pcMsg );

/* Emit an identification event — called once at startup so the host
 * monitor can auto-detect which demo/scenario is running.
 * pcTaskLabel is "edf", "srp", "cbs", or "mp". */
void vTraceEmitId( const char *pcTaskLabel, int xScenario );

/* ─── Trace hook helpers (called from FreeRTOS trace macros) ─────────── */

/* Called from traceTASK_SWITCHED_IN — receives task name and tick. */
void vTraceHookTaskSwitchedIn( const char *pcName,
                               unsigned long ulTick );

/* Called from traceEDF_DEADLINE_MISSED — receives task details. */
void vTraceHookDeadlineMissed( const char *pcName,
                               unsigned long ulTick,
                               unsigned long ulAbsDeadline,
                               unsigned long ulMissCount );

/* Called from traceSRP_CEILING_RAISED — resource lock event. */
void vTraceHookResourceLock( const char *pcTaskName,
                             unsigned long ulTick,
                             unsigned long ulResIdx,
                             unsigned long ulCeiling );

/* Called from traceSRP_CEILING_LOWERED — resource unlock event. */
void vTraceHookResourceUnlock( const char *pcTaskName,
                               unsigned long ulTick,
                               unsigned long ulResIdx,
                               unsigned long ulCeiling );

/* Called from traceCBS_BUDGET_EXHAUSTED — CBS budget exhaustion event. */
void vTraceHookCbsBudgetExhausted( const char *pcName,
                                    unsigned long ulTick,
                                    unsigned long ulNewBudget,
                                    unsigned long ulNewDeadline );

/* Called from traceCBS_JOB_ARRIVAL — CBS job arrival event. */
void vTraceHookCbsJobArrival( const char *pcName,
                               unsigned long ulTick,
                               unsigned long ulBudget,
                               unsigned long ulDeadline );

/* Called from traceTASK_SWITCHED_IN in SMP mode — includes core ID. */
void vTraceHookTaskSwitchedInCore( const char *pcName,
                                    unsigned long ulTick,
                                    unsigned long ulCoreID );

/* Called from traceMP_TASK_MIGRATED — task migration event. */
void vTraceHookMigration( const char *pcName,
                           unsigned long ulTick,
                           unsigned long ulFromCore,
                           unsigned long ulToCore );

#ifdef __cplusplus
}
#endif

#endif /* TRACE_PROTOCOL_H */
