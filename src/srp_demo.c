#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "test_harness.h"
#include "trace_protocol.h"
#include "usb_task.h"

/* ─── Busy-wait helper (spin for xTicks ticks) ─────────────────────────── */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();

    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        /* spin */
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 1: SRP Correctness (3 tasks, 2 resources)
 *
 *  | Task | T=D | C | Uses | CS length |
 *  |------|-----|---|------|-----------|
 *  | τ1   | 100 | 20| R1   | 5         |
 *  | τ2   | 200 | 30| R1,R2| 8, 10     |
 *  | τ3   | 500 | 40| R2   | 15        |
 *
 *  π1 > π2 > π3.  C(R1) = π1, C(R2) = π2.
 *  No task ever blocks on a resource; ceiling rises/falls correctly;
 *  all deadlines met.
 * ═══════════════════════════════════════════════════════════════════════ */

static SemaphoreHandle_t xR1, xR2;

typedef struct
{
    const char * pcName;
    TickType_t   xWorkBefore;   /* work before CS */
    TickType_t   xCsLength;     /* work inside CS */
    TickType_t   xWorkAfter;    /* work after CS */
    SemaphoreHandle_t xRes1;    /* first resource (or NULL) */
    SemaphoreHandle_t xRes2;    /* second resource (or NULL) */
    TickType_t   xCs2Length;    /* CS length for second resource */
} SrpTaskParams_t;

static SrpTaskParams_t xS1Params[ 3 ];

static void vSrpPeriodicTask( void * pvParameters )
{
    SrpTaskParams_t * p = ( SrpTaskParams_t * ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();

        /* Work before critical section. */
        vBusyWait( p->xWorkBefore );

        /* First resource. */
        if( p->xRes1 != NULL )
        {
            xSemaphoreTake( p->xRes1, portMAX_DELAY );
            printf( "  [%s] t=%lu  LOCK R1\n",
                    p->pcName, ( unsigned long ) xTaskGetTickCount() );
            vBusyWait( p->xCsLength );
            xSemaphoreGive( p->xRes1 );
            printf( "  [%s] t=%lu  UNLOCK R1\n",
                    p->pcName, ( unsigned long ) xTaskGetTickCount() );
        }
        else
        {
            vBusyWait( p->xCsLength );
        }

        /* Second resource (τ2 uses both R1 and R2). */
        if( p->xRes2 != NULL )
        {
            xSemaphoreTake( p->xRes2, portMAX_DELAY );
            printf( "  [%s] t=%lu  LOCK R2\n",
                    p->pcName, ( unsigned long ) xTaskGetTickCount() );
            vBusyWait( p->xCs2Length );
            xSemaphoreGive( p->xRes2 );
            printf( "  [%s] t=%lu  UNLOCK R2\n",
                    p->pcName, ( unsigned long ) xTaskGetTickCount() );
        }

        /* Work after critical section. */
        vBusyWait( p->xWorkAfter );

        printf( "[%s] t=%lu  job done (start=%lu)\n",
                p->pcName,
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned long ) xStart );

        vTaskEdfWaitForNextPeriod();
    }
}

static void vSrpMonitor( void * pvParameters )
{
    ( void ) pvParameters;

    /* Let system run for 3 seconds, then report. */
    vTaskDelay( pdMS_TO_TICKS( 3000 ) );

    printf( "\n=== SRP Correctness Report (after 3s) ===\n" );
    printf( "  Check trace output above:\n" );
    printf( "  - #RL/#RU events show ceiling rises/falls\n" );
    printf( "  - No task blocked on a resource (SRP guarantee)\n" );
    printf( "  - All deadlines met (#DM events absent)\n" );
    printf( "=== End of SRP correctness demo ===\n\n" );
    vTraceEmitInfo( "SRP correctness demo complete" );

    vTaskSuspend( NULL );
}

static void vScenario1_Correctness( void )
{
    printf( "\n=== Scenario 1: SRP Correctness ===\n" );
    printf( "tau1: T=D=100, C=20, uses R1 (CS=5)\n" );
    printf( "tau2: T=D=200, C=30, uses R1 (CS=8) + R2 (CS=10)\n" );
    printf( "tau3: T=D=500, C=40, uses R2 (CS=15)\n\n" );

    vTraceInit();

    /* Create SRP binary semaphores with max CS lengths. */
    xR1 = xSemaphoreCreateBinarySRP( pdMS_TO_TICKS( 8 ) );   /* max CS = τ2's 8 */
    xR2 = xSemaphoreCreateBinarySRP( pdMS_TO_TICKS( 15 ) );  /* max CS = τ3's 15 */

    configASSERT( xR1 != NULL );
    configASSERT( xR2 != NULL );

    /* Create EDF tasks.  τ1 has shortest deadline → highest π. */
    TaskHandle_t hTau1, hTau2, hTau3;

    xS1Params[ 0 ] = ( SrpTaskParams_t ){ "tau1", pdMS_TO_TICKS( 5 ),
        pdMS_TO_TICKS( 5 ), pdMS_TO_TICKS( 10 ), xR1, NULL, 0 };
    xS1Params[ 1 ] = ( SrpTaskParams_t ){ "tau2", pdMS_TO_TICKS( 4 ),
        pdMS_TO_TICKS( 8 ), pdMS_TO_TICKS( 8 ), xR1, xR2, pdMS_TO_TICKS( 10 ) };
    xS1Params[ 2 ] = ( SrpTaskParams_t ){ "tau3", pdMS_TO_TICKS( 5 ),
        pdMS_TO_TICKS( 15 ), pdMS_TO_TICKS( 20 ), NULL, xR2, pdMS_TO_TICKS( 15 ) };

    xTaskCreateEDF( vSrpPeriodicTask, "tau1", 512, &xS1Params[ 0 ],
                    pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 20 ),
                    &hTau1 );
    vTraceRegisterTask( "tau1", 100, 100, 20, "periodic" );

    xTaskCreateEDF( vSrpPeriodicTask, "tau2", 512, &xS1Params[ 1 ],
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 30 ),
                    &hTau2 );
    vTraceRegisterTask( "tau2", 200, 200, 30, "periodic" );

    xTaskCreateEDF( vSrpPeriodicTask, "tau3", 512, &xS1Params[ 2 ],
                    pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 40 ),
                    &hTau3 );
    vTraceRegisterTask( "tau3", 500, 500, 40, "periodic" );

    /* Declare resource usage (updates ceilings). */
    xTaskSrpDeclareUsage( hTau1, xR1 );
    xTaskSrpDeclareUsage( hTau2, xR1 );
    xTaskSrpDeclareUsage( hTau2, xR2 );
    xTaskSrpDeclareUsage( hTau3, xR2 );

    /* Finalize admission with blocking terms. */
    BaseType_t xAdm = xTaskSrpFinalizeAdmission();
    printf( "  Admission (with SRP blocking): %s\n\n",
            xAdm == pdPASS ? "FEASIBLE" : "INFEASIBLE" );

    /* Monitor task (non-EDF, low priority). */
    xTaskCreate( vSrpMonitor, "Mon", 512, NULL, 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 2: Admission Control with Blocking
 *
 *  Task set where U = sum(C_i/T_i) ≈ 0.85 (feasible without blocking).
 *  With SRP blocking terms: sum((C_i + B_i)/T_i) > 1.0 → rejected.
 * ═══════════════════════════════════════════════════════════════════════ */

static void vDummyTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vTaskEdfWaitForNextPeriod();
    }
}

static void vScenario2_AdmissionBlocking( void )
{
    printf( "\n=== Scenario 2: Admission Control with SRP Blocking ===\n" );

    /* Design: 3 tasks + 1 resource with a large critical section held by
     * the lowest-preemption-level task.  Without blocking, U < 1.0.
     * With blocking, the high-preemption-level tasks pick up the large
     * B_i that pushes total effective utilization above 1.0.
     *
     * tau1: T=D=100, C=30  (U=0.30)
     * tau2: T=D=200, C=50  (U=0.25)
     * tau3: T=D=500, C=150 (U=0.30)   uses R with CS=80
     * Total U = 0.85 (feasible without blocking).
     *
     * C(R) = π3 (only τ3 uses R), but for blocking to affect τ1 and τ2,
     * C(R) must be >= their π.  So we make τ1 also use R:
     *   C(R) = max(π1, π3) = π1.
     *   B for τ2 = max CS of lower-π tasks on R with C(R) >= π2 = 80
     *   Effective U for τ2 = (50+80)/200 = 0.65
     *   Total effective = 0.30 + 0.65 + 0.30 = 1.25 → REJECTED
     */

    printf( "tau1: T=D=100, C=30  (U=0.30) uses R (CS=5)\n" );
    printf( "tau2: T=D=200, C=50  (U=0.25)\n" );
    printf( "tau3: T=D=500, C=150 (U=0.30) uses R (CS=80)\n" );
    printf( "Without blocking: U = 0.85 (feasible)\n" );
    printf( "With SRP blocking: tau2 gets B=80 → effective U > 1.0\n\n" );

    /* Create the SRP resource. */
    SemaphoreHandle_t xR = xSemaphoreCreateBinarySRP( pdMS_TO_TICKS( 80 ) );
    configASSERT( xR != NULL );

    TaskHandle_t hT1, hT2, hT3;

    xTaskCreateEDF( vDummyTask, "tau1", 256, NULL,
                    pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 30 ),
                    &hT1 );

    xTaskCreateEDF( vDummyTask, "tau2", 256, NULL,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 50 ),
                    &hT2 );

    xTaskCreateEDF( vDummyTask, "tau3", 256, NULL,
                    pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 150 ),
                    &hT3 );

    /* Declare resource usage — only τ1 and τ3 use R. */
    xTaskSrpDeclareUsage( hT1, xR );
    xTaskSrpDeclareUsage( hT3, xR );

    /* Finalize — should reject. */
    BaseType_t xAdm = xTaskSrpFinalizeAdmission();

    printf( "  xTaskSrpFinalizeAdmission(): %s\n",
            xAdm == pdPASS ? "FEASIBLE (unexpected)" : "INFEASIBLE (expected)" );

    if( xAdm == pdFAIL )
    {
        printf( "  >> Blocking from τ3's 80-tick CS pushes effective U > 1.0\n" );
    }

    printf( "\n=== Admission blocking demo complete ===\n\n" );
    vTraceEmitInfo( "SRP admission blocking demo complete" );

    /* Don't start scheduler — just halt. */
    for( ;; )
    {
        tight_loop_contents();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 3: Stack Sharing Quantitative Study (100 tasks)
 *
 *  100 tasks with deadlines drawn from ~15 distinct values.
 *  Tasks with the same deadline share a preemption level → same stack.
 *  Report: groups, shared memory, individual memory, % savings.
 * ═══════════════════════════════════════════════════════════════════════ */

#define NUM_STACK_TASKS  100

static void vScenario3_StackSharing( void )
{
    printf( "\n=== Scenario 3: Stack Sharing Study (100 tasks) ===\n" );

    /* 15 deadline classes. Tasks are distributed across them. */
    static const TickType_t xDeadlines[ 15 ] = {
        50, 75, 100, 125, 150, 200, 250, 300, 400, 500,
        600, 750, 1000, 1500, 2000
    };

    /* Simple PRNG for reproducibility. */
    uint32_t ulPrng = 42;

    #define PRNG_NEXT() ( ulPrng = ulPrng * 1103515245U + 12345U, ( ulPrng >> 16 ) & 0x7FFF )

    printf( "  Creating %d EDF tasks with shared stacks...\n", NUM_STACK_TASKS );

    UBaseType_t uxCreated = 0;

    for( int i = 0; i < NUM_STACK_TASKS; i++ )
    {
        /* Pick a deadline class. */
        TickType_t xD = xDeadlines[ PRNG_NEXT() % 15 ];
        TickType_t xT = xD;  /* implicit deadline */
        TickType_t xC = 1 + ( PRNG_NEXT() % 3 ); /* tiny WCET so admission passes */

        /* Stack depth varies: 256-768 words. */
        configSTACK_DEPTH_TYPE uxDepth = 256 + ( PRNG_NEXT() % 513 );

        char cName[ configMAX_TASK_NAME_LEN ];
        snprintf( cName, sizeof( cName ), "s%03d", i );

        BaseType_t xResult = xTaskCreateEDFSharedStack(
            vDummyTask, cName, uxDepth, NULL,
            pdMS_TO_TICKS( xT ), pdMS_TO_TICKS( xD ), pdMS_TO_TICKS( xC ),
            NULL );

        if( xResult == pdPASS )
        {
            uxCreated++;
        }
        else
        {
            printf( "  Task %d rejected (admission or memory)\n", i );
            break;
        }
    }

    printf( "  %lu tasks created successfully.\n\n",
            ( unsigned long ) uxCreated );

    /* Query stack stats. */
    UBaseType_t uxGroups, uxShared, uxIndividual;
    vTaskSrpGetStackStats( &uxGroups, &uxShared, &uxIndividual );

    printf( "  Stack groups:          %lu\n", ( unsigned long ) uxGroups );
    printf( "  Memory with sharing:   %lu bytes\n", ( unsigned long ) uxShared );
    printf( "  Memory without sharing:%lu bytes\n", ( unsigned long ) uxIndividual );

    if( uxIndividual > 0 )
    {
        uint32_t ulSaved = ( ( uxIndividual - uxShared ) * 100U ) / uxIndividual;
        printf( "  Savings:               %lu%%\n", ( unsigned long ) ulSaved );
    }

    printf( "\n=== Stack sharing study complete ===\n\n" );
    vTraceEmitInfo( "SRP stack sharing study complete" );

    for( ;; )
    {
        tight_loop_contents();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 4: Nested Resource Locking
 *
 *  A task acquires R1, then R2 inside (nested lock).  Ceiling stack:
 *  push R1's ceiling, push R2's ceiling, pop R2, pop R1.
 *  Verifies ordering constraint and trace output.
 * ═══════════════════════════════════════════════════════════════════════ */

static SemaphoreHandle_t xNR1, xNR2;

static void vNestedTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();

        printf( "  [nest] t=%lu  begin job\n",
                ( unsigned long ) xTaskGetTickCount() );

        /* Acquire R1 (outer). */
        xSemaphoreTake( xNR1, portMAX_DELAY );
        printf( "  [nest] t=%lu  LOCK R1 (outer)\n",
                ( unsigned long ) xTaskGetTickCount() );

        vBusyWait( pdMS_TO_TICKS( 3 ) );

        /* Acquire R2 (inner, nested). */
        xSemaphoreTake( xNR2, portMAX_DELAY );
        printf( "  [nest] t=%lu  LOCK R2 (inner)\n",
                ( unsigned long ) xTaskGetTickCount() );

        vBusyWait( pdMS_TO_TICKS( 5 ) );

        /* Release R2 first (LIFO order). */
        xSemaphoreGive( xNR2 );
        printf( "  [nest] t=%lu  UNLOCK R2 (inner)\n",
                ( unsigned long ) xTaskGetTickCount() );

        vBusyWait( pdMS_TO_TICKS( 2 ) );

        /* Release R1. */
        xSemaphoreGive( xNR1 );
        printf( "  [nest] t=%lu  UNLOCK R1 (outer)\n",
                ( unsigned long ) xTaskGetTickCount() );

        printf( "  [nest] t=%lu  job done (start=%lu)\n",
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned long ) xStart );

        vTaskEdfWaitForNextPeriod();
    }
}

static void vOtherTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vBusyWait( pdMS_TO_TICKS( 10 ) );
        vTaskEdfWaitForNextPeriod();
    }
}

static void vNestedMonitor( void * pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    printf( "\n=== Nested Locking Report (after 2s) ===\n" );
    printf( "  Check #RL/#RU trace events:\n" );
    printf( "  - #RL for R1 (ceiling push)\n" );
    printf( "  - #RL for R2 (ceiling push, stack depth=2)\n" );
    printf( "  - #RU for R2 (ceiling pop)\n" );
    printf( "  - #RU for R1 (ceiling pop, stack empty)\n" );
    printf( "  Verify LIFO ordering is maintained.\n" );
    printf( "=== End of nested locking demo ===\n\n" );
    vTraceEmitInfo( "SRP nested locking demo complete" );

    vTaskSuspend( NULL );
}

static void vScenario4_NestedLocking( void )
{
    printf( "\n=== Scenario 4: Nested Resource Locking ===\n" );
    printf( "nest: T=D=200, C=20, acquires R1 then R2 (nested)\n" );
    printf( "other: T=D=300, C=10, uses R2 only\n" );
    printf( "Ceiling stack: push R1, push R2, pop R2, pop R1\n\n" );

    vTraceInit();

    /* R1: max CS = 10 (nest holds it for 10 total: 3 + 5 + 2) */
    xNR1 = xSemaphoreCreateBinarySRP( pdMS_TO_TICKS( 10 ) );
    /* R2: max CS = 5 */
    xNR2 = xSemaphoreCreateBinarySRP( pdMS_TO_TICKS( 5 ) );

    configASSERT( xNR1 != NULL );
    configASSERT( xNR2 != NULL );

    TaskHandle_t hNest, hOther;

    xTaskCreateEDF( vNestedTask, "nest", 512, NULL,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 20 ),
                    &hNest );
    vTraceRegisterTask( "nest", 200, 200, 20, "periodic" );

    xTaskCreateEDF( vOtherTask, "other", 512, NULL,
                    pdMS_TO_TICKS( 300 ), pdMS_TO_TICKS( 300 ), pdMS_TO_TICKS( 10 ),
                    &hOther );
    vTraceRegisterTask( "other", 300, 300, 10, "periodic" );

    /* Declare resource usage. */
    xTaskSrpDeclareUsage( hNest, xNR1 );
    xTaskSrpDeclareUsage( hNest, xNR2 );
    xTaskSrpDeclareUsage( hOther, xNR2 );

    /* Finalize admission. */
    BaseType_t xAdm = xTaskSrpFinalizeAdmission();
    printf( "  Admission (with SRP blocking): %s\n\n",
            xAdm == pdPASS ? "FEASIBLE" : "INFEASIBLE" );

    /* Monitor (non-EDF). */
    xTaskCreate( vNestedMonitor, "NMon", 512, NULL, 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario launcher task
 * ═══════════════════════════════════════════════════════════════════════ */

#ifndef DEMO_SCENARIO
#define DEMO_SCENARIO 1
#endif

static void vScenarioTask( void *pvParameters )
{
    (void)pvParameters;

    test_harness_init();
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    vTraceEmitId( "srp", DEMO_SCENARIO );

    #if DEMO_SCENARIO == 1
        vScenario1_Correctness();
    #elif DEMO_SCENARIO == 2
        vScenario2_AdmissionBlocking();
    #elif DEMO_SCENARIO == 3
        vScenario3_StackSharing();
    #elif DEMO_SCENARIO == 4
        vScenario4_NestedLocking();
    #else
        #error "Invalid DEMO_SCENARIO for srp_demo (expected 1-4)"
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
