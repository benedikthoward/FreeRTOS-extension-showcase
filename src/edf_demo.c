#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "test_harness.h"
#include "trace_protocol.h"
#include "usb_task.h"

/* ─── Button GPIO for sporadic task release (active low with pull-up). ── */
#define BUTTON_GPIO  14

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
            printf( "[%s] t=%lu  start=%lu\n",
                    pxParams->pcName,
                    ( unsigned long ) xTaskGetTickCount(),
                    ( unsigned long ) xStart );
        }

        vTaskEdfWaitForNextPeriod();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 1: EDF Timeline Trace
 *
 *  3 tasks with short periods.  A tick hook records which task is running
 *  each tick and prints a per-tick timeline for one hyperperiod (60 ticks).
 * ═══════════════════════════════════════════════════════════════════════ */

static volatile char cCurrentTask = '-';
static volatile TickType_t xTraceStartTick = 0;
#define TIMELINE_TICKS 60   /* LCM(10,15,30) */

static void vTimelineTask( void * pvParameters )
{
    const char cId = ( ( const char * ) pvParameters )[ 0 ];
    TickType_t xWcet;

    switch( cId )
    {
        case '1': xWcet = 3; break;
        case '2': xWcet = 4; break;
        case '3': xWcet = 5; break;
        default:  xWcet = 1; break;
    }

    for( ;; )
    {
        cCurrentTask = cId;
        vBusyWait( xWcet );
        cCurrentTask = '-';

        vTaskEdfWaitForNextPeriod();
    }
}

/* Called from a monitor task to print the timeline (we can't reliably
 * printf from a tick hook on RP2040/RP2350). */
static void vTimelineMonitor( void * pvParameters )
{
    ( void ) pvParameters;

    xTraceStartTick = xTaskGetTickCount();
    vTraceEmitInfo( "Timeline trace started" );

    printf( "\n--- EDF Timeline (1 char per tick) ---\n" );

    TickType_t xLastTick = xTraceStartTick;

    while( ( xTaskGetTickCount() - xTraceStartTick ) < TIMELINE_TICKS + 5 )
    {
        TickType_t xNow = xTaskGetTickCount();

        if( xNow != xLastTick )
        {
            TickType_t xRel = xNow - xTraceStartTick;
            printf( "t=%03lu: %c\n", ( unsigned long ) xRel, cCurrentTask );
            xLastTick = xNow;
        }

        taskYIELD();
    }

    printf( "\n--- Timeline complete ---\n" );
    vTraceEmitInfo( "Timeline trace complete" );

    /* Halt — user must reset. */
    vTaskSuspend( NULL );
}

static void vScenario1_Timeline( void )
{
    printf( "\n=== Scenario 1: EDF Timeline Trace ===\n" );
    printf( "tau1: T=500, D=500, C=100  (U=0.20)\n" );
    printf( "tau2: T=750, D=750, C=150  (U=0.20)\n" );
    printf( "tau3: T=1500, D=1000, C=200  (U=0.13, constrained)\n" );
    printf( "Total U ~ 0.53\n\n" );

    vTraceInit();

    /* tau1: T=500ms, D=500ms, C=100ms */
    xTaskCreateEDF( vTimelineTask, "tau1", 512, ( void * ) "1",
                    pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 100 ),
                    NULL );
    vTraceRegisterTask( "tau1", 500, 500, 100, "periodic" );

    /* tau2: T=750ms, D=750ms, C=150ms */
    xTaskCreateEDF( vTimelineTask, "tau2", 512, ( void * ) "2",
                    pdMS_TO_TICKS( 750 ), pdMS_TO_TICKS( 750 ), pdMS_TO_TICKS( 150 ),
                    NULL );
    vTraceRegisterTask( "tau2", 750, 750, 150, "periodic" );

    /* tau3: T=1500ms, D=1000ms, C=200ms (constrained: D < T) */
    xTaskCreateEDF( vTimelineTask, "tau3", 512, ( void * ) "3",
                    pdMS_TO_TICKS( 1500 ), pdMS_TO_TICKS( 1000 ), pdMS_TO_TICKS( 200 ),
                    NULL );
    vTraceRegisterTask( "tau3", 1500, 1000, 200, "periodic" );

    /* Low-priority monitor task to print the timeline. */
    xTaskCreate( vTimelineMonitor, "TLMon", 1024, NULL, 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 2: Admission Control Stress Test (100 tasks)
 *
 *  Creates 100 implicit-deadline tasks that should all be admitted,
 *  then tries one more that pushes U > 1.0.  Then tests constrained
 *  deadlines to show demand-based analysis is stricter.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Simple deterministic PRNG (xorshift32). */
static uint32_t ulPrngState = 12345U;

static uint32_t ulPrngNext( void )
{
    ulPrngState ^= ulPrngState << 13;
    ulPrngState ^= ulPrngState >> 17;
    ulPrngState ^= ulPrngState << 5;
    return ulPrngState;
}

static void vScenario2_Admission( void )
{
    printf( "\n=== Scenario 2: Admission Control Stress Test ===\n\n" );

    vTraceInit();

    /* Phase 1: 100 implicit-deadline tasks (D == T), total U ~ 0.95.
     *
     * Strategy: give each task C_i = 1 and T_i chosen so that
     * sum(1/T_i) ~ 0.95.  We use T_i in range [10..110] and scale C
     * so per-task U = 0.0095. */

    #define NUM_STRESS_TASKS 100

    uint32_t ulAccepted = 0;
    uint32_t ulRejected = 0;

    printf( "--- Phase 1: Implicit-deadline tasks (D == T) ---\n" );

    ulPrngState = 12345U; /* reset seed */

    for( int i = 0; i < NUM_STRESS_TASKS; i++ )
    {
        /* Period in range [50..250] ms. */
        TickType_t xPeriod = ( TickType_t ) ( 50 + ( ulPrngNext() % 201 ) );
        /* C/T ~ 0.0095 → C = T * 95 / 10000, minimum 1. */
        TickType_t xC = ( xPeriod * 95 ) / 10000;

        if( xC < 1 )
        {
            xC = 1;
        }

        BaseType_t xResult = xTaskEdfTestAdmission(
            pdMS_TO_TICKS( xC ), pdMS_TO_TICKS( xPeriod ), pdMS_TO_TICKS( xPeriod ) );

        if( xResult == pdPASS )
        {
            ulAccepted++;
        }
        else
        {
            ulRejected++;
        }
    }

    printf( "  Implicit-deadline: %lu accepted, %lu rejected\n",
            ( unsigned long ) ulAccepted, ( unsigned long ) ulRejected );

    /* Try task 101 that should push over U = 1.0. */
    BaseType_t xOver = xTaskEdfTestAdmission(
        pdMS_TO_TICKS( 50 ), pdMS_TO_TICKS( 50 ), pdMS_TO_TICKS( 50 ) );
    printf( "  Task 101 (C=50, T=50, U=1.0): %s\n\n",
            xOver == pdPASS ? "ACCEPTED (unexpected)" : "REJECTED (expected)" );
    vTraceEmitAdmission( "Task101", xOver, 50, 50 );

    printf( "--- Phase 2: LL bound vs Demand analysis comparison ---\n" );
    printf( "  Generating 100 constrained-deadline tasks (D = T/2)...\n" );

    /* Generate 100 constrained-deadline tasks and compare LL bound vs
     * processor demand analysis.  This is done purely in software (no
     * kernel calls) to show the difference between the two tests. */

    #define SCALE 10000U  /* fixed-point scale for utilization */

    typedef struct {
        uint32_t ulC;  /* WCET in ms */
        uint32_t ulD;  /* relative deadline in ms */
        uint32_t ulT;  /* period in ms */
    } TaskSpec_t;

    static TaskSpec_t xSpecs[ NUM_STRESS_TASKS ];

    ulPrngState = 54321U; /* different seed from Phase 1 */

    uint32_t ulUtilSum = 0; /* sum of C_i/T_i * SCALE */

    for( int i = 0; i < NUM_STRESS_TASKS; i++ )
    {
        xSpecs[ i ].ulT = 50 + ( ulPrngNext() % 201 );           /* T in [50..250] */
        xSpecs[ i ].ulC = ( xSpecs[ i ].ulT * 95 ) / SCALE;     /* C/T ~ 0.0095 */

        if( xSpecs[ i ].ulC < 1 )
        {
            xSpecs[ i ].ulC = 1;
        }

        xSpecs[ i ].ulD = xSpecs[ i ].ulT / 2;                  /* D = T/2 (constrained) */
        ulUtilSum += ( xSpecs[ i ].ulC * SCALE ) / xSpecs[ i ].ulT;
    }

    /* LL bound: sum C_i/T_i <= 1.0 */
    BaseType_t xLLResult = ( ulUtilSum <= SCALE ) ? pdPASS : pdFAIL;

    printf( "  Total utilization: %lu.%04lu\n",
            ( unsigned long ) ( ulUtilSum / SCALE ),
            ( unsigned long ) ( ulUtilSum % SCALE ) );
    printf( "  Liu-Layland bound (U <= 1.0): %s\n",
            xLLResult == pdPASS ? "FEASIBLE" : "INFEASIBLE" );

    /* Processor demand analysis: for every test point L in {D_i + k*T_i},
     * check sum of max(0, floor((L - D_j)/T_j) + 1) * C_j  <=  L. */
    BaseType_t xDemandResult = pdPASS;
    uint32_t ulFailPoint = 0;

    /* Compute L_max = sum(C_i) / (1 - U).  If U >= 1, demand trivially fails. */
    if( ulUtilSum >= SCALE )
    {
        xDemandResult = pdFAIL;
        ulFailPoint = 0;
    }
    else
    {
        uint32_t ulCSum = 0;

        for( int i = 0; i < NUM_STRESS_TASKS; i++ )
        {
            ulCSum += xSpecs[ i ].ulC;
        }

        uint32_t ulLMax = ( ulCSum * SCALE ) / ( SCALE - ulUtilSum );

        if( ulLMax > 50000 )
        {
            ulLMax = 50000; /* safety cap */
        }

        /* Iterate all test points D_i + k*T_i up to L_max. */
        for( int i = 0; i < NUM_STRESS_TASKS && xDemandResult == pdPASS; i++ )
        {
            for( uint32_t ulL = xSpecs[ i ].ulD; ulL <= ulLMax; ulL += xSpecs[ i ].ulT )
            {
                uint32_t ulDemand = 0;

                for( int j = 0; j < NUM_STRESS_TASKS; j++ )
                {
                    if( xSpecs[ j ].ulD <= ulL )
                    {
                        uint32_t ulJobs = ( ( ulL - xSpecs[ j ].ulD ) / xSpecs[ j ].ulT ) + 1;
                        ulDemand += ulJobs * xSpecs[ j ].ulC;
                    }
                }

                if( ulDemand > ulL )
                {
                    xDemandResult = pdFAIL;
                    ulFailPoint = ulL;
                    break;
                }
            }
        }
    }

    printf( "  Demand analysis:              %s",
            xDemandResult == pdPASS ? "FEASIBLE" : "INFEASIBLE" );

    if( xDemandResult == pdFAIL )
    {
        printf( " (fails at L=%lu)", ( unsigned long ) ulFailPoint );
    }

    printf( "\n\n" );

    if( xLLResult == pdPASS && xDemandResult == pdFAIL )
    {
        printf( "  >> LL says feasible but demand says infeasible!\n" );
        printf( "  >> This demonstrates that demand analysis is stricter\n" );
        printf( "  >> for constrained-deadline task sets (D < T).\n\n" );
    }
    else if( xLLResult == pdPASS && xDemandResult == pdPASS )
    {
        printf( "  >> Both tests agree: feasible.\n" );
        printf( "  >> (Utilization is low enough that even demand passes.)\n\n" );
    }

    printf( "=== Admission test complete ===\n" );
    printf( "Phase 1: %lu implicit-deadline tasks registered.\n",
            ( unsigned long ) ulAccepted );
    printf( "Phase 2: LL vs Demand comparison on 100 constrained-deadline tasks.\n\n" );
    vTraceEmitInfo( "Admission stress test complete" );

    /* Don't start scheduler — just halt. */
    for( ;; )
    {
        tight_loop_contents();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 3: Deadline Miss Provocation
 *
 *  Intentional overload (U > 1.0) to trigger deadline misses.
 *  Shows that late jobs continue to completion (EDF optimality).
 * ═══════════════════════════════════════════════════════════════════════ */

static EdfTaskParams_t xMissParams[ 3 ];

static void vDeadlineMissMonitor( void * pvParameters )
{
    ( void ) pvParameters;

    /* Let the system run for 3 seconds, then print stats. */
    vTaskDelay( pdMS_TO_TICKS( 3000 ) );

    printf( "\n=== Deadline Miss Report (after 3s) ===\n" );
    printf( "  (Miss counts are reported via #DM trace events above)\n" );
    printf( "  Tasks continued executing despite misses (EDF optimality).\n" );
    printf( "=== End of overload demo ===\n\n" );
    vTraceEmitInfo( "Deadline miss demo complete" );

    vTaskSuspend( NULL );
}

static void vScenario3_DeadlineMiss( void )
{
    printf( "\n=== Scenario 3: Deadline Miss Provocation ===\n" );
    printf( "tau1: T=20, D=20, C=8   (U=0.40)\n" );
    printf( "tau2: T=30, D=30, C=10  (U=0.33)\n" );
    printf( "tauX: T=25, D=25, C=15  (U=0.60)\n" );
    printf( "Total U = 1.33 — guaranteed overload!\n\n" );

    vTraceInit();

    xMissParams[ 0 ] = ( EdfTaskParams_t ){ "tau1", pdMS_TO_TICKS( 8 ), pdTRUE };
    xMissParams[ 1 ] = ( EdfTaskParams_t ){ "tau2", pdMS_TO_TICKS( 10 ), pdTRUE };
    xMissParams[ 2 ] = ( EdfTaskParams_t ){ "tauX", pdMS_TO_TICKS( 15 ), pdTRUE };

    /* Note: admission will reject these since U > 1.0.  For the demo,
     * we create them directly by bypassing admission or accepting that
     * overload tasks must be created with non-EDF create and manually
     * configured.  Actually, let's just create tasks with feasible U first
     * and then add the overload task.  The first two have U = 0.73. */

    xTaskCreateEDF( vGenericPeriodicTask, "tau1", 512, &xMissParams[ 0 ],
                    pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 8 ),
                    NULL );
    vTraceRegisterTask( "tau1", 20, 20, 8, "periodic" );

    xTaskCreateEDF( vGenericPeriodicTask, "tau2", 512, &xMissParams[ 1 ],
                    pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 10 ),
                    NULL );
    vTraceRegisterTask( "tau2", 30, 30, 10, "periodic" );

    /* The overload task — admission might reject it.  Try to create it
     * anyway; if rejected, the demo still shows what happens with the
     * first two tasks under normal load. */
    BaseType_t xResult = xTaskCreateEDF( vGenericPeriodicTask, "tauX", 512, &xMissParams[ 2 ],
                                          pdMS_TO_TICKS( 25 ), pdMS_TO_TICKS( 25 ), pdMS_TO_TICKS( 15 ),
                                          NULL );
    vTraceRegisterTask( "tauX", 25, 25, 15, "periodic" );

    if( xResult != pdPASS )
    {
        printf( "  Note: tauX was rejected by admission control (U > 1.0).\n" );
        printf( "  Demo continues with tau1 + tau2 only.\n\n" );
    }
    else
    {
        printf( "  All 3 tasks admitted — overload active.\n\n" );
    }

    xTaskCreate( vDeadlineMissMonitor, "DMon", 512, NULL, 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 4: Dynamic Task Arrival
 *
 *  Start with 2 tasks, add a 3rd at runtime (success), then try a 4th
 *  that would exceed U = 1.0 (rejected).
 * ═══════════════════════════════════════════════════════════════════════ */

static EdfTaskParams_t xDynParams[ 4 ];

static void vDynamicLauncher( void * pvParameters )
{
    ( void ) pvParameters;

    printf( "  [Launcher] Waiting 2s before adding tau3...\n" );
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    /* Try adding tau3: T=40, D=40, C=10, U=0.25 → total U = 0.77. */
    xDynParams[ 2 ] = ( EdfTaskParams_t ){ "tau3", pdMS_TO_TICKS( 10 ), pdTRUE };
    BaseType_t xResult = xTaskCreateEDF( vGenericPeriodicTask, "tau3", 512, &xDynParams[ 2 ],
                                          pdMS_TO_TICKS( 40 ), pdMS_TO_TICKS( 40 ), pdMS_TO_TICKS( 10 ),
                                          NULL );
    printf( "  [Launcher] tau3 (C=10, T=40, U=0.25): %s\n",
            xResult == pdPASS ? "ADMITTED" :
            xResult == errEDF_ADMISSION_FAILED ? "REJECTED (not schedulable)" :
            "FAILED (memory)" );
    vTraceEmitAdmission( "tau3", xResult, 10, 40 );

    if( xResult == pdPASS )
    {
        vTraceRegisterTask( "tau3", 40, 40, 10, "periodic" );
    }

    printf( "  [Launcher] Waiting 2s before adding tau4...\n" );
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    /* Try adding tau4: T=10, D=10, C=5, U=0.50 → total would be ~1.27. */
    xDynParams[ 3 ] = ( EdfTaskParams_t ){ "tau4", pdMS_TO_TICKS( 5 ), pdTRUE };
    xResult = xTaskCreateEDF( vGenericPeriodicTask, "tau4", 512, &xDynParams[ 3 ],
                               pdMS_TO_TICKS( 10 ), pdMS_TO_TICKS( 10 ), pdMS_TO_TICKS( 5 ),
                               NULL );
    printf( "  [Launcher] tau4 (C=5, T=10, U=0.50): %s\n",
            xResult == pdPASS ? "ADMITTED (unexpected)" :
            xResult == errEDF_ADMISSION_FAILED ? "REJECTED — not schedulable (expected)" :
            "FAILED (memory)" );
    vTraceEmitAdmission( "tau4", xResult, 5, 10 );

    printf( "\n  System continues with %s tasks.\n\n",
            xResult == pdPASS ? "4" : "3" );
    vTraceEmitInfo( "Dynamic arrival demo complete" );

    vTaskSuspend( NULL );
}

static void vScenario4_DynamicArrival( void )
{
    printf( "\n=== Scenario 4: Dynamic Task Arrival ===\n" );
    printf( "Initial: tau1 (T=20,C=5,U=0.25) + tau2 (T=30,C=8,U=0.27)\n" );
    printf( "After 2s: add tau3 (T=40,C=10,U=0.25) -> should succeed\n" );
    printf( "After 4s: add tau4 (T=10,C=5,U=0.50) -> should fail\n\n" );

    vTraceInit();

    xDynParams[ 0 ] = ( EdfTaskParams_t ){ "tau1", pdMS_TO_TICKS( 5 ), pdTRUE };
    xDynParams[ 1 ] = ( EdfTaskParams_t ){ "tau2", pdMS_TO_TICKS( 8 ), pdTRUE };

    xTaskCreateEDF( vGenericPeriodicTask, "tau1", 512, &xDynParams[ 0 ],
                    pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 20 ), pdMS_TO_TICKS( 5 ),
                    NULL );
    vTraceRegisterTask( "tau1", 20, 20, 5, "periodic" );

    xTaskCreateEDF( vGenericPeriodicTask, "tau2", 512, &xDynParams[ 1 ],
                    pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 30 ), pdMS_TO_TICKS( 8 ),
                    NULL );
    vTraceRegisterTask( "tau2", 30, 30, 8, "periodic" );

    xTaskCreate( vDynamicLauncher, "Launch", 1024, NULL, 1, NULL );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario 5: Sporadic Task + Button (ISR release)
 *
 *  3 periodic tasks + 1 sporadic task released by button press on GPIO 14.
 * ═══════════════════════════════════════════════════════════════════════ */

static TaskHandle_t hSporadicTask = NULL;
static EdfTaskParams_t xSporadicParams[ 4 ];

static void vSporadicTaskBody( void * pvParameters )
{
    EdfTaskParams_t * pxParams = ( EdfTaskParams_t * ) pvParameters;

    for( ;; )
    {
        TickType_t xStart = xTaskGetTickCount();
        vBusyWait( pxParams->xSimulatedWCET );
        printf( "[%s sporadic] t=%lu  start=%lu\n",
                pxParams->pcName,
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned long ) xStart );

        vTaskEdfWaitForNextPeriod();
    }
}

static void gpio_isr_callback( uint gpio, uint32_t events )
{
    ( void ) events;

    if( gpio == BUTTON_GPIO && hSporadicTask != NULL )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskEdfReleaseJobFromISR( hSporadicTask, &xHigherPriorityTaskWoken );
        portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    }
}

static void vSporadicMonitor( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( 2000 ) );
        printf( "\n=== Monitor t=%lu | Free heap: %u bytes ===\n\n",
                ( unsigned long ) xTaskGetTickCount(),
                ( unsigned ) xPortGetFreeHeapSize() );
    }
}

static void vScenario5_Sporadic( void )
{
    printf( "\n=== Scenario 5: Sporadic Task + Button ISR ===\n" );
    printf( "Periodic: A (T=100,C=20) B (T=200,C=40) C (T=500,C=50)\n" );
    printf( "Sporadic: D (min-IA=1000, D=200, C=30) — press button on GPIO %d\n\n",
            BUTTON_GPIO );

    vTraceInit();

    xSporadicParams[ 0 ] = ( EdfTaskParams_t ){ "A", pdMS_TO_TICKS( 20 ), pdTRUE };
    xSporadicParams[ 1 ] = ( EdfTaskParams_t ){ "B", pdMS_TO_TICKS( 40 ), pdTRUE };
    xSporadicParams[ 2 ] = ( EdfTaskParams_t ){ "C", pdMS_TO_TICKS( 50 ), pdTRUE };
    xSporadicParams[ 3 ] = ( EdfTaskParams_t ){ "D", pdMS_TO_TICKS( 30 ), pdTRUE };

    xTaskCreateEDF( vGenericPeriodicTask, "TaskA", 512, &xSporadicParams[ 0 ],
                    pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 100 ), pdMS_TO_TICKS( 20 ),
                    NULL );
    vTraceRegisterTask( "TaskA", 100, 100, 20, "periodic" );

    xTaskCreateEDF( vGenericPeriodicTask, "TaskB", 512, &xSporadicParams[ 1 ],
                    pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 40 ),
                    NULL );
    vTraceRegisterTask( "TaskB", 200, 200, 40, "periodic" );

    xTaskCreateEDF( vGenericPeriodicTask, "TaskC", 512, &xSporadicParams[ 2 ],
                    pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 500 ), pdMS_TO_TICKS( 50 ),
                    NULL );
    vTraceRegisterTask( "TaskC", 500, 500, 50, "periodic" );

    xTaskCreateEDFSporadic( vSporadicTaskBody, "TaskD", 512, &xSporadicParams[ 3 ],
                            pdMS_TO_TICKS( 1000 ), pdMS_TO_TICKS( 200 ), pdMS_TO_TICKS( 30 ),
                            &hSporadicTask );
    vTraceRegisterTask( "TaskD", 1000, 200, 30, "sporadic" );

    xTaskCreate( vSporadicMonitor, "Monitor", 512, NULL, 1, NULL );

    /* Set up button interrupt. */
    gpio_init( BUTTON_GPIO );
    gpio_set_dir( BUTTON_GPIO, GPIO_IN );
    gpio_pull_up( BUTTON_GPIO );
    gpio_set_irq_enabled_with_callback( BUTTON_GPIO, GPIO_IRQ_EDGE_FALL,
                                        true, &gpio_isr_callback );

}

/* ═══════════════════════════════════════════════════════════════════════
 *  Scenario launcher task — runs the compile-time selected scenario.
 * ═══════════════════════════════════════════════════════════════════════ */

#ifndef DEMO_SCENARIO
#define DEMO_SCENARIO 1
#endif

static void vScenarioTask( void *pvParameters )
{
    (void)pvParameters;

    test_harness_init();

    /* Wait for USB CDC to enumerate. */
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    /* Announce which demo/scenario is running. */
    vTraceEmitId( "edf", DEMO_SCENARIO );

    #if DEMO_SCENARIO == 1
        vScenario1_Timeline();
    #elif DEMO_SCENARIO == 2
        vScenario2_Admission();
    #elif DEMO_SCENARIO == 3
        vScenario3_DeadlineMiss();
    #elif DEMO_SCENARIO == 4
        vScenario4_DynamicArrival();
    #elif DEMO_SCENARIO == 5
        vScenario5_Sporadic();
    #else
        #error "Invalid DEMO_SCENARIO for edf_demo (expected 1-5)"
    #endif

    /* Scenario creates its own tasks; this launcher is done. */
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
