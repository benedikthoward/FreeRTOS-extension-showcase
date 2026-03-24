#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ─── Extension toggles (set via CMake presets / -D flags) ──────────────── */
#ifndef configUSE_EDF
#define configUSE_EDF 0
#endif

#ifndef configEDF_MAX_TASKS
#define configEDF_MAX_TASKS 128
#endif

#ifndef configUSE_SRP
#define configUSE_SRP 0
#endif

#ifndef configSRP_MAX_RESOURCES
#define configSRP_MAX_RESOURCES 8
#endif

#ifndef configSRP_CEILING_STACK_DEPTH
#define configSRP_CEILING_STACK_DEPTH 16
#endif

#ifndef configSRP_MAX_STACK_GROUPS
#define configSRP_MAX_STACK_GROUPS 32
#endif

#ifndef configUSE_CBS
#define configUSE_CBS 0
#endif

#ifndef configUSE_MP
#define configUSE_MP 0
#endif

/* ─── Core FreeRTOS settings ────────────────────────────────────────────── */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      150000000
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                512
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   3
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* ─── Memory allocation ─────────────────────────────────────────────────── */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (128 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ─── Assertions ───────────────────────────────────────────────────────── */
#define configASSERT( x ) \
    do { if( !( x ) ) { __asm volatile( "bkpt #0" ); for( ;; ); } } while( 0 )

/* ─── Hook functions ────────────────────────────────────────────────────── */
#define configUSE_IDLE_HOOK                     0
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* ─── Run-time stats ────────────────────────────────────────────────────── */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ─── Co-routine (unused) ───────────────────────────────────────────────── */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ─── Software timers ───────────────────────────────────────────────────── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

/* ─── INCLUDE functions ─────────────────────────────────────────────────── */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/* ─── Trace hooks (structured protocol for host GUI) ───────────────────── */
#include "trace_protocol.h"

/*  traceTASK_SWITCHED_IN is expanded inside tasks.c where pxCurrentTCB and
 *  xTickCount are in scope.  We forward the relevant fields to a plain C
 *  function so trace_protocol.c stays decoupled from kernel internals.    */
#if ( configNUMBER_OF_CORES > 1 )
#define traceTASK_SWITCHED_IN() \
    vTraceHookTaskSwitchedInCore( \
        pxCurrentTCBs[ portGET_CORE_ID() ]->pcTaskName, \
        ( unsigned long ) xTickCount, \
        ( unsigned long ) portGET_CORE_ID() )
#else
#define traceTASK_SWITCHED_IN() \
    vTraceHookTaskSwitchedIn( pxCurrentTCB->pcTaskName, \
                              ( unsigned long ) xTickCount )
#endif

#if ( configUSE_EDF == 1 )
#define traceEDF_DEADLINE_MISSED( pxTCB ) \
    vTraceHookDeadlineMissed( ( pxTCB )->pcTaskName, \
                              ( unsigned long ) xTickCount, \
                              ( unsigned long ) ( pxTCB )->xAbsoluteDeadline, \
                              ( unsigned long ) ( pxTCB )->ulDeadlineMissCount )
#endif

#if ( configUSE_SRP == 1 )
#define traceSRP_CEILING_RAISED( uxResourceIdx, uxNewCeiling ) \
    vTraceHookResourceLock( pxCurrentTCB->pcTaskName, \
                            ( unsigned long ) xTickCount, \
                            ( unsigned long ) ( uxResourceIdx ), \
                            ( unsigned long ) ( uxNewCeiling ) )

#define traceSRP_CEILING_LOWERED( uxResourceIdx, uxNewCeiling ) \
    vTraceHookResourceUnlock( pxCurrentTCB->pcTaskName, \
                              ( unsigned long ) xTickCount, \
                              ( unsigned long ) ( uxResourceIdx ), \
                              ( unsigned long ) ( uxNewCeiling ) )
#endif

#if ( configUSE_CBS == 1 )
#define traceCBS_BUDGET_EXHAUSTED( pxTCB ) \
    vTraceHookCbsBudgetExhausted( ( pxTCB )->pcTaskName, \
                                   ( unsigned long ) xTickCount, \
                                   ( unsigned long ) ( pxTCB )->xCbsBudgetRemaining, \
                                   ( unsigned long ) ( pxTCB )->xAbsoluteDeadline )

#define traceCBS_JOB_ARRIVAL( pxTCB ) \
    vTraceHookCbsJobArrival( ( pxTCB )->pcTaskName, \
                              ( unsigned long ) xTickCount, \
                              ( unsigned long ) ( pxTCB )->xCbsBudgetRemaining, \
                              ( unsigned long ) ( pxTCB )->xAbsoluteDeadline )
#endif

#if ( configUSE_MP == 1 )
#define traceMP_TASK_MIGRATED( pxTCB, xFromCore, xToCore ) \
    vTraceHookMigration( ( pxTCB )->pcTaskName, \
                          ( unsigned long ) xTickCount, \
                          ( unsigned long ) ( xFromCore ), \
                          ( unsigned long ) ( xToCore ) )
#endif

/* ─── Pico SDK interop ─────────────────────────────────────────────────── */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

/* ─── RP2350 Cortex-M33 port settings ──────────────────────────────────── */
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1

/* RP2350 has 4 priority bits (16 levels, 0-15).  The ARMv8-M port
 * auto-detects configPRIO_BITS at runtime — do NOT define it.
 * configMAX_SYSCALL_INTERRUPT_PRIORITY is a raw byte value;
 * the official pico-examples use 16 (priority level 1 in top nibble). */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

/* ─── RP2350 SMP support ────────────────────────────────────────────────── */
#if ( configUSE_MP == 1 )
    #define configNUMBER_OF_CORES               2
    #define configRUN_MULTIPLE_PRIORITIES        1
    #define configUSE_CORE_AFFINITY             1
    #define configTICK_CORE                     0

    #ifndef GLOBAL_EDF_ENABLE
        #define GLOBAL_EDF_ENABLE               1
    #endif
    #ifndef PARTITIONED_EDF_ENABLE
        #define PARTITIONED_EDF_ENABLE          0
    #endif

    #if ( GLOBAL_EDF_ENABLE == 1 ) && ( PARTITIONED_EDF_ENABLE == 1 )
        #error "Only one of GLOBAL_EDF_ENABLE or PARTITIONED_EDF_ENABLE may be active"
    #endif
    #if ( GLOBAL_EDF_ENABLE == 0 ) && ( PARTITIONED_EDF_ENABLE == 0 )
        #undef GLOBAL_EDF_ENABLE
        #define GLOBAL_EDF_ENABLE               1
    #endif

    /* SRP requires partitioned EDF in multiprocessor mode. */
    #if ( configUSE_SRP == 1 ) && ( GLOBAL_EDF_ENABLE == 1 )
        #error "SRP requires partitioned EDF in multiprocessor mode (set PARTITIONED_EDF_ENABLE=1)"
    #endif

#else
    #define configNUMBER_OF_CORES               1
    #define configRUN_MULTIPLE_PRIORITIES        0
    #define configUSE_CORE_AFFINITY             0
#endif

#endif /* FREERTOS_CONFIG_H */
