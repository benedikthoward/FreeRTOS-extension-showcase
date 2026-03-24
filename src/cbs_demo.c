#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
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

/* ─── Generic periodic task body ───────────────────────────────────────── */

typedef struct
{
    const char * pcName;
    TickType_t   xSimulatedWCET;
    BaseType_t   xPrintEachJob;
} EdfTaskParams_t;

static void vGenericPeriodicTask( void * pvParameters )
{
    EdfTaskParams_t * pxParams = ( EdfTaskParams_t * ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();

        vBusyWait( pxParams->xSimulatedWCET );

        if( pxParams->xPrintEachJob )
        {
            printf( "[%s] t=%lu  job done (start=%lu)\n",
                    pxParams->pcName,
                    ( unsigned long ) xTaskGetTickCount(),
                    ( unsigned long ) xStart );
        }

        vTaskEdfWaitForNextPeriod();
    }
}

/* ─── Generic CBS (aperiodic) task body ───────────────────────────────── */

typedef struct
{
    const char * pcName;
    TickType_t   xWorkTicks;   /* simulated aperiodic execution time */
} CbsTaskParams_t;

static void vCbsAperiodicTask( void * pvParameters )
{
    CbsTaskParams_t * p = ( CbsTaskParams_t * ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();
        printf( "  [%s] t=%lu  job start\n",
                p->pcName, ( unsigned long ) xStart );

        vBusyWait( p->xWorkTicks );

        printf( "  [%s] t=%lu  job done (start=%lu)\n",
                p->pcName,
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned long ) xStart );

        vTaskCbsWaitForNextJob();
    }
}

/* ─── Job releaser task ───────────────────────────────────────────────── */

typedef struct
{
    TaskHandle_t   xCbsHandle;
    TickType_t     xReleaseInterval;  /* delay between releases */
    UBaseType_t    uxMaxJobs;         /* 0 = infinite */
} CbsReleaserParams_t;

static void vCbsReleaser( void * pvParameters )
{
    CbsReleaserParams_t * p = ( CbsReleaserParams_t * ) pvParameters;
    UBaseType_t uxCount = 0;

    for( ;; )
    {
        vTaskDelay( p->xReleaseInterval );

        BaseType_t xResult = xTaskCbsReleaseJob( p->xCbsHandle );
        printf( "  [releaser] t=%lu  release CBS → %s\n",
                ( unsigned long ) xTaskGetTickCount(),
                xResult == pdPASS ? "OK" : "BUSY" );

        uxCount++;

        if( p->uxMaxJobs > 0 && uxCount >= p->uxMaxJobs )
        {
            vTaskSuspend( NULL );
        }
    }
}

/* ─── Monitor task ────────────────────────────────────────────────────── */

static void vCbsMonitor( void * pvParameters )
{
    const char * pcScenario = ( const char * ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( 3000 ) );

    printf( "\n=== %s Report (after 3s) ===\n", pcScenario );
    printf( "  Check trace output for #CB (budget exhaustion) and\n" );
    printf( "  #CA (job arrival) events.\n" );
    printf( "  All periodic tasks should have 0 deadline misses.\n" );
    printf( "=== End of %s ===\n\n", pcScenario );
    vTraceEmitInfo( "CBS demo complete" );

    vTaskSuspend( NULL );
}

/* ─── Dummy task for admission tests ──────────────────────────────────── */

static void vDummyTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vTaskEdfWaitForNextPeriod();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 1: Single CBS + Periodic Tasks (Correctness)
 *
 *  | Task | Type     | T(ms) | D(ms) | C/Q(ms) | U    |
 *  |------|----------|-------|-------|---------|------|
 *  | tau1 | periodic | 100   | 100   | 30      | 0.30 |
 *  | tau2 | periodic | 200   | 200   | 40      | 0.20 |
 *  | CBS1 | CBS      | 100   | 100   | 20      | 0.20 |
 *  | Total|          |       |       |         | 0.70 |
 *
 *  A releaser sends jobs to CBS1 every 150ms (15ms of work each).
 *  Jobs fit within budget — no deadline postponement expected.
 * ═══════════════════════════════════════════════════════════════════════ */

static EdfTaskParams_t xS1P1, xS1P2;
static CbsTaskParams_t xS1Cbs;
static CbsReleaserParams_t xS1Rel;

static void vScenario1_SingleCBS( void )
{
    printf( "\n=== Scenario 1: Single CBS + Periodic Tasks ===\n" );
    printf( "tau1: T=D=100, C=30 (periodic, U=0.30)\n" );
    printf( "tau2: T=D=200, C=40 (periodic, U=0.20)\n" );
    printf( "CBS1: Q=20, T=100 (U=0.20)\n" );
    printf( "Total U = 0.70\n\n" );

    vTraceInit();

    /* Periodic tasks */
    xS1P1 = ( EdfTaskParams_t ){ "tau1", pdMS_TO_TICKS( 30 ), pdTRUE };
    xS1P2 = ( EdfTaskParams_t ){ "tau2", pdMS_TO_TICKS( 40 ), pdTRUE };

    xTaskCreateEDF( vGenericPeriodicTask, "tau1", 512, &xS1P1,
                    pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 30 ),
                    NULL );
    vTraceRegisterTask( "tau1", 100, 100, 30, "periodic" );

    xTaskCreateEDF( vGenericPeriodicTask, "tau2", 512, &xS1P2,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 40 ),
                    NULL );
    vTraceRegisterTask( "tau2", 200, 200, 40, "periodic" );

    /* CBS server */
    TaskHandle_t hCbs1;
    xS1Cbs = ( CbsTaskParams_t ){ "CBS1", pdMS_TO_TICKS( 15 ) };

    BaseType_t xResult = xTaskCreateCBS( vCbsAperiodicTask, "CBS1", 512, &xS1Cbs,
                                          pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 100 ),
                                          &hCbs1 );
    printf( "  CBS1 creation: %s\n\n",
            xResult == pdPASS ? "OK" : "FAILED" );
    vTraceRegisterTask( "CBS1", 100, 100, 20, "cbs" );

    /* Releaser: sends job every 150ms, infinite */
    xS1Rel = ( CbsReleaserParams_t ){ hCbs1, pdMS_TO_TICKS( 150 ), 0 };
    xTaskCreate( vCbsReleaser, "Rel", 256, &xS1Rel, 1, NULL );

    /* Monitor */
    xTaskCreate( vCbsMonitor, "Mon", 512, ( void * ) "Scenario 1", 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 2: Budget Exhaustion and Deadline Postponement
 *
 *  | Task | Type     | T(ms) | D(ms) | C/Q(ms) | U    |
 *  |------|----------|-------|-------|---------|------|
 *  | tau1 | periodic | 200   | 200   | 50      | 0.25 |
 *  | CBS1 | CBS      | 100   | 100   | 30      | 0.30 |
 *  | Total|          |       |       |         | 0.55 |
 *
 *  A single CBS job requiring 80ms of work is released.
 *  CBS1 exhausts its budget (30) twice before completing.
 *  Expected: 2 × #CB events, job finishes after ~130ms.
 * ═══════════════════════════════════════════════════════════════════════ */

static EdfTaskParams_t xS2P1;
static CbsTaskParams_t xS2Cbs;
static CbsReleaserParams_t xS2Rel;

static void vScenario2_BudgetExhaustion( void )
{
    printf( "\n=== Scenario 2: Budget Exhaustion Demo ===\n" );
    printf( "tau1: T=D=200, C=50 (periodic, U=0.25)\n" );
    printf( "CBS1: Q=30, T=100 (U=0.30)\n" );
    printf( "CBS1 job requires 80ms of work (exceeds Q=30)\n" );
    printf( "Expected: 2 budget exhaustions, CBS runs in 3 bursts\n\n" );

    vTraceInit();

    /* Periodic task */
    xS2P1 = ( EdfTaskParams_t ){ "tau1", pdMS_TO_TICKS( 50 ), pdTRUE };
    xTaskCreateEDF( vGenericPeriodicTask, "tau1", 512, &xS2P1,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 50 ),
                    NULL );
    vTraceRegisterTask( "tau1", 200, 200, 50, "periodic" );

    /* CBS server with heavy workload */
    TaskHandle_t hCbs1;
    xS2Cbs = ( CbsTaskParams_t ){ "CBS1", pdMS_TO_TICKS( 80 ) };
    BaseType_t xResult = xTaskCreateCBS( vCbsAperiodicTask, "CBS1", 512, &xS2Cbs,
                                          pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 100 ),
                                          &hCbs1 );
    printf( "  CBS1 creation: %s\n\n",
            xResult == pdPASS ? "OK" : "FAILED" );
    vTraceRegisterTask( "CBS1", 100, 100, 30, "cbs" );

    /* Single job release after 10ms */
    xS2Rel = ( CbsReleaserParams_t ){ hCbs1, pdMS_TO_TICKS( 10 ), 1 };
    xTaskCreate( vCbsReleaser, "Rel", 256, &xS2Rel, 1, NULL );

    xTaskCreate( vCbsMonitor, "Mon", 512, ( void * ) "Scenario 2", 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 3: Multiple CBS — Bandwidth Isolation
 *
 *  | Task | Type     | T(ms) | D(ms) | C/Q(ms) | U    |
 *  |------|----------|-------|-------|---------|------|
 *  | tau1 | periodic | 200   | 200   | 40      | 0.20 |
 *  | CBS1 | CBS      | 100   | 100   | 20      | 0.20 |
 *  | CBS2 | CBS      | 150   | 150   | 30      | 0.20 |
 *  | Total|          |       |       |         | 0.60 |
 *
 *  CBS1 receives 100ms of work (5× budget — misbehaving).
 *  CBS2 receives 25ms of work (fits in budget).
 *  tau1 should have 0 deadline misses — isolation!
 * ═══════════════════════════════════════════════════════════════════════ */

static EdfTaskParams_t xS3P1;
static CbsTaskParams_t xS3Cbs1, xS3Cbs2;
static CbsReleaserParams_t xS3Rel1, xS3Rel2;

static void vScenario3_Isolation( void )
{
    printf( "\n=== Scenario 3: Multiple CBS — Bandwidth Isolation ===\n" );
    printf( "tau1: T=D=200, C=40 (periodic, U=0.20)\n" );
    printf( "CBS1: Q=20, T=100 (U=0.20) — 100ms work (misbehaving)\n" );
    printf( "CBS2: Q=30, T=150 (U=0.20) — 25ms work (well-behaved)\n" );
    printf( "Total U = 0.60\n\n" );

    vTraceInit();

    /* Periodic task */
    xS3P1 = ( EdfTaskParams_t ){ "tau1", pdMS_TO_TICKS( 40 ), pdTRUE };
    xTaskCreateEDF( vGenericPeriodicTask, "tau1", 512, &xS3P1,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 40 ),
                    NULL );
    vTraceRegisterTask( "tau1", 200, 200, 40, "periodic" );

    /* CBS1: misbehaving server */
    TaskHandle_t hCbs1;
    xS3Cbs1 = ( CbsTaskParams_t ){ "CBS1", pdMS_TO_TICKS( 100 ) };
    xTaskCreateCBS( vCbsAperiodicTask, "CBS1", 512, &xS3Cbs1,
                    pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 100 ), &hCbs1 );
    vTraceRegisterTask( "CBS1", 100, 100, 20, "cbs" );

    /* CBS2: well-behaved server */
    TaskHandle_t hCbs2;
    xS3Cbs2 = ( CbsTaskParams_t ){ "CBS2", pdMS_TO_TICKS( 25 ) };
    xTaskCreateCBS( vCbsAperiodicTask, "CBS2", 512, &xS3Cbs2,
                    pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 150 ), &hCbs2 );
    vTraceRegisterTask( "CBS2", 150, 150, 30, "cbs" );

    /* Both released simultaneously at t≈10ms */
    xS3Rel1 = ( CbsReleaserParams_t ){ hCbs1, pdMS_TO_TICKS( 10 ), 1 };
    xS3Rel2 = ( CbsReleaserParams_t ){ hCbs2, pdMS_TO_TICKS( 10 ), 1 };
    xTaskCreate( vCbsReleaser, "Rel1", 256, &xS3Rel1, 1, NULL );
    xTaskCreate( vCbsReleaser, "Rel2", 256, &xS3Rel2, 1, NULL );

    xTaskCreate( vCbsMonitor, "Mon", 512, ( void * ) "Scenario 3 (Isolation)", 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 4: CBS Admission Control
 *
 *  Fill periodic utilization to 0.85, then test CBS admission:
 *  - CBS with U=0.20 → total 1.05 → REJECTED
 *  - CBS with U=0.10 → total 0.95 → ACCEPTED
 *  No scheduler started.
 * ═══════════════════════════════════════════════════════════════════════ */

static void vScenario4_Admission( void )
{
    printf( "\n=== Scenario 4: CBS Admission Control ===\n" );

    /* Create periodic tasks filling to U=0.85 */
    printf( "Creating periodic tasks (U=0.85)...\n" );

    xTaskCreateEDF( vDummyTask, "p1", 256, NULL,
                    pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 30 ),
                    NULL );    /* U=0.30 */

    xTaskCreateEDF( vDummyTask, "p2", 256, NULL,
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 50 ),
                    NULL );    /* U=0.25 */

    xTaskCreateEDF( vDummyTask, "p3", 256, NULL,
                    pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 150 ),
                    NULL );    /* U=0.30 */

    printf( "  Periodic U = 0.30 + 0.25 + 0.30 = 0.85\n\n" );

    /* Attempt CBS with U=0.20 → total 1.05 → should FAIL */
    printf( "Attempting CBS1: Q=20, T=100 (U=0.20) → total=1.05\n" );
    BaseType_t xR1 = xTaskCreateCBS( vDummyTask, "CBS1", 256, NULL,
                                      pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 100 ),
                                      NULL );
    printf( "  Result: %s\n\n",
            xR1 == pdPASS ? "ACCEPTED (unexpected)" :
            xR1 == errEDF_ADMISSION_FAILED ? "REJECTED (expected)" : "ERROR" );

    /* Attempt CBS with U=0.10 → total 0.95 → should PASS */
    printf( "Attempting CBS2: Q=10, T=100 (U=0.10) → total=0.95\n" );
    BaseType_t xR2 = xTaskCreateCBS( vDummyTask, "CBS2", 256, NULL,
                                      pdMS_TO_TICKS( 10 ), pdMS_TO_TICKS( 100 ),
                                      NULL );
    printf( "  Result: %s\n\n",
            xR2 == pdPASS ? "ACCEPTED (expected)" :
            xR2 == errEDF_ADMISSION_FAILED ? "REJECTED (unexpected)" : "ERROR" );

    printf( "=== Admission control demo complete ===\n\n" );

    for( ;; )
    {
        tight_loop_contents();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Main — scenario selector
 * ═══════════════════════════════════════════════════════════════════════ */

#ifndef DEMO_SCENARIO
#define DEMO_SCENARIO 1
#endif

static void vScenarioTask( void *pvParameters )
{
    (void)pvParameters;

    test_harness_init();
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    vTraceEmitId( "cbs", DEMO_SCENARIO );

    #if DEMO_SCENARIO == 1
        vScenario1_SingleCBS();
    #elif DEMO_SCENARIO == 2
        vScenario2_BudgetExhaustion();
    #elif DEMO_SCENARIO == 3
        vScenario3_Isolation();
    #elif DEMO_SCENARIO == 4
        vScenario4_Admission();
    #else
        #error "Invalid DEMO_SCENARIO for cbs_demo (expected 1-4)"
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
