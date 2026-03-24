#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "test_harness.h"
#include "trace_protocol.h"
#include "usb_task.h"

/* ─── Busy-wait helper ─────────────────────────────────────────────── */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();

    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        /* spin */
    }
}

/* ─── Generic periodic task body ───────────────────────────────────── */

typedef struct
{
    const char * pcName;
    TickType_t   xSimulatedWCET;
} MpTaskParams_t;

static void vMpPeriodicTask( void * pvParameters )
{
    MpTaskParams_t * p = ( MpTaskParams_t * ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();

        vBusyWait( p->xSimulatedWCET );

        printf( "[%s] t=%lu done (start=%lu)\n",
                p->pcName,
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned long ) xStart );

        vTaskEdfWaitForNextPeriod();
    }
}

/* ─── Monitor task ─────────────────────────────────────────────────── */

static void vMpMonitor( void * pvParameters )
{
    const char * pcScenario = ( const char * ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( 3000 ) );

    printf( "\n=== %s Report (after 3s) ===\n", pcScenario );
    printf( "  Check trace output for #TS events with core IDs.\n" );

    #if ( configUSE_MP == 1 )
    {
        for( BaseType_t c = 0; c < ( BaseType_t ) configNUMBER_OF_CORES; c++ )
        {
            printf( "  Core %ld utilization: %lu / 10000\n",
                    ( long ) c,
                    ( unsigned long ) ulTaskMpGetCoreUtilization( c ) );
        }
    }
    #endif

    printf( "=== End of %s ===\n\n", pcScenario );
    vTraceEmitInfo( "MP demo complete" );

    vTaskSuspend( NULL );
}

/* ─── Dummy task for admission tests ──────────────────────────────── */

static void vDummyTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vTaskEdfWaitForNextPeriod();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Scenario 1: Global EDF — 4 tasks on 2 cores
 *
 *  | Task | T=D(ms) | C(ms) | U    |
 *  |------|---------|-------|------|
 *  | tau1 | 100     | 30    | 0.30 |
 *  | tau2 | 150     | 40    | 0.27 |
 *  | tau3 | 200     | 50    | 0.25 |
 *  | tau4 | 300     | 60    | 0.20 |
 *  | Total|         |       | 1.02 |
 *
 *  Total U=1.02 fits on 2 cores (U ≤ 2.0).
 *  Tasks may migrate between cores.
 * ═══════════════════════════════════════════════════════════════════ */

static MpTaskParams_t xS1Params[ 4 ];

static void vScenario1_GlobalEDF( void )
{
    printf( "\n=== Scenario 1: Global EDF (4 tasks, 2 cores) ===\n" );
    printf( "tau1: T=D=100, C=30 (U=0.30)\n" );
    printf( "tau2: T=D=150, C=40 (U=0.27)\n" );
    printf( "tau3: T=D=200, C=50 (U=0.25)\n" );
    printf( "tau4: T=D=300, C=60 (U=0.20)\n" );
    printf( "Total U = 1.02 — fits on 2 cores\n\n" );

    vTraceInit();

    static const struct { const char *name; TickType_t T; TickType_t C; } tasks[] = {
        { "tau1", 100, 30 },
        { "tau2", 150, 40 },
        { "tau3", 200, 50 },
        { "tau4", 300, 60 },
    };

    for( int i = 0; i < 4; i++ )
    {
        xS1Params[ i ] = ( MpTaskParams_t ){ tasks[ i ].name, pdMS_TO_TICKS( tasks[ i ].C ) };

        BaseType_t xResult = xTaskCreateEDF(
            vMpPeriodicTask, tasks[ i ].name, 512, &xS1Params[ i ],
            pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
            pdMS_TO_TICKS( tasks[ i ].C ), NULL );

        printf( "  %s: %s\n", tasks[ i ].name,
                xResult == pdPASS ? "admitted" : "REJECTED" );
        vTraceRegisterTask( tasks[ i ].name, tasks[ i ].T, tasks[ i ].T,
                            tasks[ i ].C, "periodic" );
    }

    printf( "\n" );

    xTaskCreate( vMpMonitor, "Mon", 512,
                 ( void * ) "Scenario 1 (Global EDF)", 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════
 *  Scenario 2: Partitioned EDF — Automatic WFD
 *
 *  Same 4 tasks as Scenario 1, but assigned via Worst-Fit Decreasing.
 * ═══════════════════════════════════════════════════════════════════ */

#if defined( PARTITIONED_EDF_ENABLE ) && ( PARTITIONED_EDF_ENABLE == 1 )

static MpTaskParams_t xS2Params[ 4 ];

static void vScenario2_PartitionedWFD( void )
{
    printf( "\n=== Scenario 2: Partitioned EDF — Auto WFD ===\n" );

    vTraceInit();

    static const struct { const char *name; TickType_t T; TickType_t C; } tasks[] = {
        { "tau1", 100, 30 },
        { "tau2", 150, 40 },
        { "tau3", 200, 50 },
        { "tau4", 300, 60 },
    };

    for( int i = 0; i < 4; i++ )
    {
        xS2Params[ i ] = ( MpTaskParams_t ){ tasks[ i ].name, pdMS_TO_TICKS( tasks[ i ].C ) };

        BaseType_t xResult = xTaskCreateEDFAutoPartition(
            vMpPeriodicTask, tasks[ i ].name, 512, &xS2Params[ i ],
            pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
            pdMS_TO_TICKS( tasks[ i ].C ), NULL );

        printf( "  %s: %s\n", tasks[ i ].name,
                xResult == pdPASS ? "admitted" : "REJECTED" );
        vTraceRegisterTask( tasks[ i ].name, tasks[ i ].T, tasks[ i ].T,
                            tasks[ i ].C, "periodic" );
    }

    printf( "\n" );

    for( BaseType_t c = 0; c < ( BaseType_t ) configNUMBER_OF_CORES; c++ )
    {
        printf( "  Core %ld utilization: %lu / 10000\n",
                ( long ) c, ( unsigned long ) ulTaskMpGetCoreUtilization( c ) );
    }

    printf( "\n" );

    xTaskCreate( vMpMonitor, "Mon", 512,
                 ( void * ) "Scenario 2 (Partitioned WFD)", 1, NULL );

}

#endif /* PARTITIONED_EDF_ENABLE */

/* ═══════════════════════════════════════════════════════════════════
 *  Scenario 3: Partitioned EDF — Manual Assignment
 * ═══════════════════════════════════════════════════════════════════ */

#if defined( PARTITIONED_EDF_ENABLE ) && ( PARTITIONED_EDF_ENABLE == 1 )

static MpTaskParams_t xS3Params[ 4 ];

static void vScenario3_ManualPartition( void )
{
    printf( "\n=== Scenario 3: Partitioned EDF — Manual ===\n" );
    printf( "Core 0: tau1 (T=100,C=30), tau2 (T=150,C=40) → U=0.57\n" );
    printf( "Core 1: tau3 (T=200,C=50), tau4 (T=300,C=60) → U=0.45\n\n" );

    vTraceInit();

    static const struct { const char *name; TickType_t T; TickType_t C; BaseType_t core; } tasks[] = {
        { "tau1", 100, 30, 0 },
        { "tau2", 150, 40, 0 },
        { "tau3", 200, 50, 1 },
        { "tau4", 300, 60, 1 },
    };

    for( int i = 0; i < 4; i++ )
    {
        xS3Params[ i ] = ( MpTaskParams_t ){ tasks[ i ].name, pdMS_TO_TICKS( tasks[ i ].C ) };

        BaseType_t xResult = xTaskCreateEDFPartitioned(
            vMpPeriodicTask, tasks[ i ].name, 512, &xS3Params[ i ],
            pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
            pdMS_TO_TICKS( tasks[ i ].C ), tasks[ i ].core, NULL );

        printf( "  %s → core %ld: %s\n", tasks[ i ].name,
                ( long ) tasks[ i ].core,
                xResult == pdPASS ? "admitted" : "REJECTED" );
        vTraceRegisterTask( tasks[ i ].name, tasks[ i ].T, tasks[ i ].T,
                            tasks[ i ].C, "periodic" );
    }

    printf( "\n" );

    xTaskCreate( vMpMonitor, "Mon", 512,
                 ( void * ) "Scenario 3 (Manual Partition)", 1, NULL );

}

#endif /* PARTITIONED_EDF_ENABLE */

/* ═══════════════════════════════════════════════════════════════════
 *  Scenario 4: Task Migration
 * ═══════════════════════════════════════════════════════════════════ */

#if defined( PARTITIONED_EDF_ENABLE ) && ( PARTITIONED_EDF_ENABLE == 1 )

static TaskHandle_t xS4_hTau4;
static MpTaskParams_t xS4Params[ 4 ];

static void vMigrationController( void * pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    printf( "\n  [migrate] t=%lu  Migrating tau4 core 1 → core 0...\n",
            ( unsigned long ) xTaskGetTickCount() );

    BaseType_t xResult = xTaskMpMigrate( xS4_hTau4, 0 );
    printf( "  [migrate] Result: %s\n\n",
            xResult == pdPASS ? "OK" : "FAILED (core full)" );

    vTaskSuspend( NULL );
}

static void vScenario4_Migration( void )
{
    printf( "\n=== Scenario 4: Task Migration ===\n" );
    printf( "Start: tau1,tau2 on core 0; tau3,tau4 on core 1\n" );
    printf( "At t=2s: migrate tau4 → core 0\n\n" );

    vTraceInit();

    static const struct { const char *name; TickType_t T; TickType_t C; BaseType_t core; } tasks[] = {
        { "tau1", 200, 30, 0 },
        { "tau2", 300, 40, 0 },
        { "tau3", 200, 50, 1 },
        { "tau4", 500, 30, 1 },
    };

    for( int i = 0; i < 4; i++ )
    {
        xS4Params[ i ] = ( MpTaskParams_t ){ tasks[ i ].name, pdMS_TO_TICKS( tasks[ i ].C ) };
        TaskHandle_t hTask;

        xTaskCreateEDFPartitioned(
            vMpPeriodicTask, tasks[ i ].name, 512, &xS4Params[ i ],
            pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
            pdMS_TO_TICKS( tasks[ i ].C ), tasks[ i ].core, &hTask );

        if( i == 3 ) { xS4_hTau4 = hTask; }

        vTraceRegisterTask( tasks[ i ].name, tasks[ i ].T, tasks[ i ].T,
                            tasks[ i ].C, "periodic" );
    }

    xTaskCreate( vMigrationController, "Migr", 512, NULL, 1, NULL );
    xTaskCreate( vMpMonitor, "Mon", 512,
                 ( void * ) "Scenario 4 (Migration)", 1, NULL );

}

#endif /* PARTITIONED_EDF_ENABLE */

/* ═══════════════════════════════════════════════════════════════════
 *  Scenario 5: Admission Control Comparison
 * ═══════════════════════════════════════════════════════════════════ */

static void vScenario5_Admission( void )
{
    printf( "\n=== Scenario 5: Admission Control ===\n\n" );

    static const struct { const char *name; TickType_t T; TickType_t C; } tasks[] = {
        { "tau1", 100, 30 },  /* U=0.30 */
        { "tau2", 100, 40 },  /* U=0.40 */
        { "tau3", 100, 50 },  /* U=0.50 */
        { "tau4", 100, 30 },  /* U=0.30 */
    };

    printf( "Task set: U = 0.30 + 0.40 + 0.50 + 0.30 = 1.50\n\n" );

    #if defined( GLOBAL_EDF_ENABLE ) && ( GLOBAL_EDF_ENABLE == 1 )
    {
        printf( "Global EDF mode (bound: U <= %d.0):\n",
                configNUMBER_OF_CORES );

        for( int i = 0; i < 4; i++ )
        {
            BaseType_t xResult = xTaskCreateEDF(
                vDummyTask, tasks[ i ].name, 256, NULL,
                pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
                pdMS_TO_TICKS( tasks[ i ].C ), NULL );

            printf( "  %s (U=0.%02lu): %s\n",
                    tasks[ i ].name,
                    ( unsigned long ) ( tasks[ i ].C * 100 / tasks[ i ].T ),
                    xResult == pdPASS ? "ACCEPTED" :
                    xResult == errEDF_ADMISSION_FAILED ? "REJECTED" : "ERROR" );
        }

        printf( "\n  Expected: all 4 accepted (1.50 <= 2.0)\n" );
    }
    #elif defined( PARTITIONED_EDF_ENABLE ) && ( PARTITIONED_EDF_ENABLE == 1 )
    {
        printf( "Partitioned EDF mode (WFD, per-core U <= 1.0):\n" );

        for( int i = 0; i < 4; i++ )
        {
            BaseType_t xResult = xTaskCreateEDFAutoPartition(
                vDummyTask, tasks[ i ].name, 256, NULL,
                pdMS_TO_TICKS( tasks[ i ].T ), pdMS_TO_TICKS( tasks[ i ].T ),
                pdMS_TO_TICKS( tasks[ i ].C ), NULL );

            printf( "  %s (U=0.%02lu): %s\n",
                    tasks[ i ].name,
                    ( unsigned long ) ( tasks[ i ].C * 100 / tasks[ i ].T ),
                    xResult == pdPASS ? "ACCEPTED" :
                    xResult == errEDF_ADMISSION_FAILED ? "REJECTED" : "ERROR" );
        }

        for( BaseType_t c = 0; c < ( BaseType_t ) configNUMBER_OF_CORES; c++ )
        {
            printf( "  Core %ld utilization: %lu / 10000\n",
                    ( long ) c, ( unsigned long ) ulTaskMpGetCoreUtilization( c ) );
        }
    }
    #endif

    printf( "\n=== Admission control demo complete ===\n" );

    for( ;; )
    {
        tight_loop_contents();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main — scenario selector
 * ═══════════════════════════════════════════════════════════════════ */

#ifndef DEMO_SCENARIO
#define DEMO_SCENARIO 1
#endif

static void vScenarioTask( void *pvParameters )
{
    (void)pvParameters;

    test_harness_init();
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    vTraceEmitId( "mp", DEMO_SCENARIO );

    #if DEMO_SCENARIO == 1
        vScenario1_GlobalEDF();
    #elif DEMO_SCENARIO == 2
        vScenario2_PartitionedWFD();
    #elif DEMO_SCENARIO == 3
        vScenario3_ManualPartition();
    #elif DEMO_SCENARIO == 4
        vScenario4_Migration();
    #elif DEMO_SCENARIO == 5
        vScenario5_Admission();
    #else
        #error "Invalid DEMO_SCENARIO for mp_demo (expected 1-5)"
    #endif

    vTaskDelete( NULL );
}

int main( void )
{
    if( cyw43_arch_init() )
    {
        return -1;
    }

    vStartUsbTask();
    xTaskCreate( vScenarioTask, "Init", 2048, NULL, 1, NULL );
    vTaskStartScheduler();

    for( ;; );
}
