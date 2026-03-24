#include "trace_protocol.h"

/* ─── Global enable flag ──────────────────────────────────────────────── */
static volatile int xTraceEnabled = 0;

void vTraceInit( void )
{
    xTraceEnabled = 1;
}

void vTraceEnable( void )
{
    xTraceEnabled = 1;
}

void vTraceDisable( void )
{
    xTraceEnabled = 0;
}

/* ─── Explicit event emitters ─────────────────────────────────────────── */

void vTraceRegisterTask( const char *pcName,
                         unsigned long ulPeriod,
                         unsigned long ulDeadline,
                         unsigned long ulWCET,
                         const char *pcType )
{
    printf( "#TR:%s,%lu,%lu,%lu,%s\n",
            pcName, ulPeriod, ulDeadline, ulWCET, pcType );
}

void vTraceEmitJobStart( const char *pcName,
                         unsigned long ulAbsDeadline )
{
    if( xTraceEnabled )
    {
        printf( "#JS:%s,%lu\n", pcName, ulAbsDeadline );
    }
}

void vTraceEmitJobComplete( const char *pcName )
{
    if( xTraceEnabled )
    {
        printf( "#JC:%s\n", pcName );
    }
}

void vTraceEmitAdmission( const char *pcName,
                          int xResult,
                          unsigned long ulUtilNum,
                          unsigned long ulUtilDen )
{
    printf( "#AR:%s,%s,%lu,%lu\n",
            pcName,
            xResult ? "OK" : "FAIL",
            ulUtilNum, ulUtilDen );
}

void vTraceEmitInfo( const char *pcMsg )
{
    printf( "#IN:%s\n", pcMsg );
}

void vTraceEmitId( const char *pcTaskLabel, int xScenario )
{
    printf( "#ID:%s,%d\n", pcTaskLabel, xScenario );
}

/* ─── Trace hook helpers ──────────────────────────────────────────────── */

/* Return non-zero if pcName is a FreeRTOS / infrastructure task whose
 * context switches would flood the serial link without adding value.
 * The host monitor ignores unknown task names anyway, so this is purely
 * a bandwidth optimisation — it does NOT affect the data model. */
static inline int prvIsInfraTask( const char *pcName )
{
    const char c0 = pcName[0], c1 = pcName[1];
    return ( c0 == 'I' && c1 == 'D' )   /* IDLE    */
        || ( c0 == 'I' && c1 == 'n' )   /* Init    */
        || ( c0 == 'T' && c1 == 'm' )   /* Tmr Svc */
        || ( c0 == 'U' && c1 == 'S' )   /* USB     */
        || ( c0 == 'M' && c1 == 'e' )   /* Menu    */
        || ( c0 == 'T' && c1 == 'L' )   /* TLMon   */
        || ( c0 == 'D' && c1 == 'M' )   /* DMon    */
        || ( c0 == 'L' && c1 == 'a' );  /* Launch  */
}

void vTraceHookTaskSwitchedIn( const char *pcName,
                               unsigned long ulTick )
{
    if( xTraceEnabled && !prvIsInfraTask( pcName ) )
    {
        printf( "#TS:%lu,%s\n", ulTick, pcName );
    }
}

void vTraceHookDeadlineMissed( const char *pcName,
                               unsigned long ulTick,
                               unsigned long ulAbsDeadline,
                               unsigned long ulMissCount )
{
    if( xTraceEnabled )
    {
        printf( "#DM:%lu,%s,%lu,%lu\n",
                ulTick, pcName, ulAbsDeadline, ulMissCount );
    }
}

/* ─── SRP trace hook helpers ─────────────────────────────────────────── */

void vTraceHookResourceLock( const char *pcTaskName,
                             unsigned long ulTick,
                             unsigned long ulResIdx,
                             unsigned long ulCeiling )
{
    if( xTraceEnabled )
    {
        printf( "#RL:%lu,%s,%lu,%lu\n",
                ulTick, pcTaskName, ulResIdx, ulCeiling );
    }
}

void vTraceHookResourceUnlock( const char *pcTaskName,
                               unsigned long ulTick,
                               unsigned long ulResIdx,
                               unsigned long ulCeiling )
{
    if( xTraceEnabled )
    {
        printf( "#RU:%lu,%s,%lu,%lu\n",
                ulTick, pcTaskName, ulResIdx, ulCeiling );
    }
}

/* ─── CBS trace hook helpers ────────────────────────────────────────── */

void vTraceHookCbsBudgetExhausted( const char *pcName,
                                    unsigned long ulTick,
                                    unsigned long ulNewBudget,
                                    unsigned long ulNewDeadline )
{
    if( xTraceEnabled )
    {
        printf( "#CB:%lu,%s,%lu,%lu\n",
                ulTick, pcName, ulNewBudget, ulNewDeadline );
    }
}

void vTraceHookCbsJobArrival( const char *pcName,
                               unsigned long ulTick,
                               unsigned long ulBudget,
                               unsigned long ulDeadline )
{
    if( xTraceEnabled )
    {
        printf( "#CA:%lu,%s,%lu,%lu\n",
                ulTick, pcName, ulBudget, ulDeadline );
    }
}

/* ─── MP trace hook helpers ─────────────────────────────────────────── */

void vTraceHookTaskSwitchedInCore( const char *pcName,
                                    unsigned long ulTick,
                                    unsigned long ulCoreID )
{
    if( xTraceEnabled && !prvIsInfraTask( pcName ) )
    {
        /* Extended #TS with 3rd field (core ID). Backward-compatible:
         * old parsers that expect 2 fields will ignore the extra one. */
        printf( "#TS:%lu,%s,%lu\n",
                ulTick, pcName, ulCoreID );
    }
}

void vTraceHookMigration( const char *pcName,
                           unsigned long ulTick,
                           unsigned long ulFromCore,
                           unsigned long ulToCore )
{
    if( xTraceEnabled )
    {
        printf( "#MG:%lu,%s,%lu,%lu\n",
                ulTick, pcName, ulFromCore, ulToCore );
    }
}
