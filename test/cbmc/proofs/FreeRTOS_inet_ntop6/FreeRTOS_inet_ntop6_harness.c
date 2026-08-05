/* Standard includes. */
#include <stdint.h>
#include <stdio.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_IPv6_Sockets.h"

/* CBMC includes. */
#include "cbmc.h"

/* Bound on the destination buffer size explored by the proof. */
#ifndef MAX_BUFFER_SIZE
    #define MAX_BUFFER_SIZE    42U
#endif

void harness()
{
    uint8_t ucSource[ ipSIZE_OF_IPv6_ADDRESS ];
    socklen_t uxSize;
    char * pcDestination;

    /* The longest IPv6 text form is 39 characters plus a NUL terminator, so 40
     * bytes. The bound is a little above that so the proof covers the exact-fit
     * and has-room cases as well as the short ones. */
    __CPROVER_assume( uxSize <= MAX_BUFFER_SIZE );

    /* Back pcDestination with exactly uxSize bytes so that any write at an
     * index greater than or equal to uxSize is reported. */
    pcDestination = safeMalloc( uxSize );
    __CPROVER_assume( pcDestination != NULL );

    ( void ) FreeRTOS_inet_ntop6( ucSource, pcDestination, uxSize );
}
