/*
 * Some constants, hardware definitions and comments taken from ST's HAL driver
 * library, COPYRIGHT(c) 2015 STMicroelectronics.
 */

/*
 * FreeRTOS+TCP V2.3.2
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_DNS.h"
#include "FreeRTOS_Routing.h"
#include "NetworkBufferManagement.h"
#include "NetworkInterface.h"

#include "phyHandling.h"

/* ST includes. */
#include "stm32h7xx_hal.h"

#ifndef STM32Hxx_HAL_ETH_H

/*
 * The ST HAL library provides stm32h7xx_hal_eth.{c,h}.
 * This FreeRTOS+TCP driver renamed these files to stm32hxx_hal_eth.{c,h}
 * by removing the '7'.
 * Please make sure that "portable/NetworkInterface/STM32Hxx" is included
 * in the include paths earlier than "STM32H7xx_HAL_Driver/Inc".
 * and also make sure that you have defined 'HAL_ETH_MODULE_ENABLED'
 * in your copy of "stm32h7xx_hal_conf".
 */
    #error stm32hxx_hal_eth.h is possibly not included
#endif

/* Interrupt events to process: reception, transmission and error handling. */
#define EMAC_IF_RX_EVENT                1U
#define EMAC_IF_TX_EVENT                2U
#define EMAC_IF_ERR_EVENT               4U

/*
 * Enable either Hash or Perfect Filter, Multicast filter - None,
 * Enable Hash Multicast (HMC), and Enable Hash Unicast (HUC).
 */
#define ENABLE_HASH_FILTER_SETTINGS     ( ( uint32_t ) 0x00000416U )

#ifndef niEMAC_HANDLER_TASK_NAME
    #define niEMAC_HANDLER_TASK_NAME    "EMAC-task"
#endif

#ifndef niEMAC_HANDLER_TASK_STACK_SIZE
    #define niEMAC_HANDLER_TASK_STACK_SIZE    ( 4 * configMINIMAL_STACK_SIZE )
#endif

#ifndef niEMAC_HANDLER_TASK_PRIORITY
    #define niEMAC_HANDLER_TASK_PRIORITY    configMAX_PRIORITIES - 1
#endif

#ifndef ipconfigEMAC_TASK_HOOK
    #define ipconfigEMAC_TASK_HOOK()
#endif

/* Bit map of outstanding ETH interrupt events for processing. */
static volatile uint32_t ulISREvents;

typedef enum
{
    eMACInit,   /* Must initialise MAC. */
    eMACPass,   /* Initialisation was successful. */
    eMACFailed, /* Initialisation failed. */
} eMAC_INIT_STATUS_TYPE;

static eMAC_INIT_STATUS_TYPE xMacInitStatus = eMACInit;

/* xTXDescriptorSemaphore is shared with stm32h7xx_hal_eth.c. */
SemaphoreHandle_t xTXDescriptorSemaphore = NULL;

/* Both the IP-task and the EMAC task use the TX channel.  Use
 * a mutex to protect it against synchronous access by both tasks. */
static SemaphoreHandle_t xTransmissionMutex;

/* Global Ethernet handle */
static ETH_HandleTypeDef xEthHandle;
static ETH_TxPacketConfig xTxConfig;

static NetworkInterface_t * pxMyInterface = NULL;

/*
 * About the section ".ethernet_data" : the DMA wants the descriptors and buffers allocated in the
 * RAM3 memory, which can be added to the .LD file as follows::
 *
 * RAM3 (xrw)      : ORIGIN = 0x24040000, LENGTH = 0x8000
 *
 * .ethernet_data :
 * {
 *  PROVIDE_HIDDEN (__ethernet_data_start = .);
 *  KEEP (*(SORT(.ethernet_data.*)))
 *  KEEP (*(.ethernet_data*))
 *  PROVIDE_HIDDEN (__ethernet_data_end = .);
 * } >RAM3
 *
 */
/* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMARxDscrTab[ ETH_RX_DESC_CNT ]    __attribute__( ( section( ".ethernet_data" ), aligned( 32 ) ) );

/* Ethernet Receive Buffer */
#if ( ipconfigZERO_COPY_TX_DRIVER == 0 )
    uint8_t Rx_Buff[ ETH_RX_DESC_CNT ][ ETH_RX_BUF_SIZE ] __attribute__( ( section( ".ethernet_data" ), aligned( 32 ) ) );
#endif

/* Ethernet Tx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ ETH_TX_DESC_CNT ]    __attribute__( ( section( ".ethernet_data" ), aligned( 32 ) ) );

/* Ethernet Transmit Buffer */
#if ( ipconfigZERO_COPY_TX_DRIVER == 0 )
    uint8_t Tx_Buff[ ETH_TX_DESC_CNT ][ ETH_TX_BUF_SIZE ]                __attribute__( ( section( ".ethernet_data" ), aligned( 32 ) ) );
#endif

/* This function binds PHY IO functions, then inits and configures */
static void prvMACBProbePhy( void );

/* Force a negotiation with the Switch or Router and wait for LS. */
static void prvEthernetUpdateConfig( BaseType_t xForce );

/* Holds the handle of the task used as a deferred interrupt processor.  The
 * handle is used so direct notifications can be sent to the task for all EMAC/DMA
 * related interrupts. */
static TaskHandle_t xEMACTaskHandle = NULL;

/*
 * A deferred interrupt handler task that processes
 */
static void prvEMACHandlerTask( void * pvParameters );

/*
 * See if there is a new packet and forward it to the IP-task.
 */
static BaseType_t prvNetworkInterfaceInput( void );

/* Private PHY IO functions and properties */
static int32_t ETH_PHY_IO_ReadReg( uint32_t DevAddr,
                                   uint32_t RegAddr,
                                   uint32_t * pRegVal );
static int32_t ETH_PHY_IO_WriteReg( uint32_t DevAddr,
                                    uint32_t RegAddr,
                                    uint32_t RegVal );

static void vClearOptionBit( volatile uint32_t * pulValue,
                             uint32_t ulValue );

#if ( ipconfigHAS_PRINTF != 0 )
    static size_t uxGetOwnCount( ETH_HandleTypeDef * heth );
#endif

/* FreeRTOS+TCP/multi :
 * Each network device has 3 access functions:
 * - Initialise the device
 * - Output a network packet
 * - Return the PHY Link-Status (LS)
 * They can be defined as static because the function addresses
 * addresses will be stored in struct NetworkInterface_t. */

static BaseType_t xSTM32H_NetworkInterfaceInitialise( NetworkInterface_t * pxInterface );

static BaseType_t xSTM32H_NetworkInterfaceOutput( NetworkInterface_t * pxInterface,
                                                  NetworkBufferDescriptor_t * const pxBuffer,
                                                  BaseType_t xReleaseAfterSend );

static BaseType_t xSTM32H_GetPhyLinkStatus( NetworkInterface_t * pxInterface );

NetworkInterface_t * pxSTM32H_FillInterfaceDescriptor( BaseType_t xEMACIndex,
                                                       NetworkInterface_t * pxInterface );
/*-----------------------------------------------------------*/

static EthernetPhy_t xPhyObject;
/* For local use only: describe the PHY's properties: */
const PhyProperties_t xPHYProperties =
{
    .ucSpeed  = PHY_SPEED_AUTO,
    .ucDuplex = PHY_DUPLEX_AUTO,
    .ucMDI_X  = PHY_MDIX_DIRECT
};
/*-----------------------------------------------------------*/

/* Reverse the bits of a 32 bit unsigned integer */
static uint32_t prvRevBits32( uint32_t ulValue )
{
    uint32_t ulRev32;
    int iIndex;

    ulRev32 = 0;

    for( iIndex = 0; iIndex < 32; iIndex++ )
    {
        if( ulValue & ( 1 << iIndex ) )
        {
            {
                ulRev32 |= 1 << ( 31 - iIndex );
            }
        }
    }

    return ulRev32;
}

/* Compute the CRC32 of the given MAC address as per IEEE 802.3 CRC32 */
static uint32_t prvComputeCRC32_MAC( const uint8_t * pucMAC )
{
    int iiIndex, ijIndex;
    uint32_t ulCRC32 = 0xFFFFFFFF;

    for( ijIndex = 0; ijIndex < 6; ijIndex++ )
    {
        ulCRC32 = ulCRC32 ^ ( uint32_t ) pucMAC[ ijIndex ];

        for( iiIndex = 0; iiIndex < 8; iiIndex++ )
        {
            if( ulCRC32 & 1 )
            {
                ulCRC32 = ( ulCRC32 >> 1 ) ^ prvRevBits32( 0x04C11DB7 ); /* IEEE 802.3 CRC32 polynomial - 0x04C11DB7 */
            }
            else
            {
                ulCRC32 = ( ulCRC32 >> 1 );
            }
        }
    }

    ulCRC32 = ~( ulCRC32 );
    return ulCRC32;
}

/* Compute the hash value of a given MAC address to index the bits in the Hash Table
 * Registers (ETH_MACHT0R and ETH_MACHT1R) */
static uint32_t prvComputeEthernet_MACHash( const uint8_t * pucMAC )
{
    uint32_t ulCRC32;
    uint32_t ulHash;

    /*  Calculate the 32-bit CRC for the MAC */
    ulCRC32 = prvComputeCRC32_MAC( pucMAC );

    /* Perform bitwise reversal on the CRC32 */
    ulHash = prvRevBits32( ulCRC32 );

    /* Take the upper 6 bits of the above result */
    return( ulHash >> 26 );
}

/* Update the Hash Table Registers
 * (ETH_MACHT0R and ETH_MACHT1R) with hash value of the given MAC address */
static void prvSetMAC_HashFilter( ETH_HandleTypeDef * pxEthHandle,
                                  const uint8_t * pucMAC )
{
    uint32_t ulHash;

    /* Compute the hash */
    ulHash = prvComputeEthernet_MACHash( pucMAC );

    /* Use the upper (MACHT1R) or lower (MACHT0R) Hash Table Registers
     * to set the required bit based on the ulHash */
    if( ulHash < 32 )
    {
        pxEthHandle->Instance->MACHT0R |= ( 1 << ulHash );
    }
    else
    {
        pxEthHandle->Instance->MACHT1R |= ( 1 << ( ulHash % 32 ) );
    }
}

/*-----------------------------------------------------------*/


/*******************************************************************************
*                      Network Interface API Functions
*******************************************************************************/

static uint8_t * pucGetRXBuffer( size_t uxSize )
{
    TickType_t uxBlockTimeTicks = ipMS_TO_MIN_TICKS( 10U );
    NetworkBufferDescriptor_t * pxBufferDescriptor;
    uint8_t * pucReturn = NULL;

    pxBufferDescriptor = pxGetNetworkBufferWithDescriptor( uxSize, uxBlockTimeTicks );

    if( pxBufferDescriptor != NULL )
    {
        pucReturn = pxBufferDescriptor->pucEthernetBuffer;
    }

    return pucReturn;
}
/*-----------------------------------------------------------*/

static BaseType_t xSTM32H_NetworkInterfaceInitialise( NetworkInterface_t * pxInterface )
{
    BaseType_t xResult = pdFAIL;
    NetworkEndPoint_t * pxEndPoint;
    HAL_StatusTypeDef xHalEthInitStatus;
    size_t uxIndex = 0;

    if( xMacInitStatus == eMACInit )
    {
        pxMyInterface = pxInterface;

        pxEndPoint = FreeRTOS_FirstEndPoint( pxInterface );
        configASSERT( pxEndPoint != NULL );

        /*
         * Initialize ETH Handler
         * It assumes that Ethernet GPIO and clock configuration
         * are already done in the ETH_MspInit()
         */
        xEthHandle.Instance = ETH;
        xEthHandle.Init.MACAddr = ( uint8_t * ) pxEndPoint->xMACAddress.ucBytes;
        xEthHandle.Init.MediaInterface = HAL_ETH_RMII_MODE;
        xEthHandle.Init.TxDesc = DMATxDscrTab;
        xEthHandle.Init.RxDesc = DMARxDscrTab;
        xEthHandle.Init.RxBuffLen = ( ETH_RX_BUF_SIZE - ipBUFFER_PADDING ) & ~( ( uint32_t ) 3U );

        /* Make sure that all unused fields are cleared. */
        memset( &( DMATxDscrTab ), '\0', sizeof( DMATxDscrTab ) );
        memset( &( DMARxDscrTab ), '\0', sizeof( DMARxDscrTab ) );

        xHalEthInitStatus = HAL_ETH_Init( &( xEthHandle ) );

        if( xHalEthInitStatus == HAL_OK )
        {
            /* Update MAC filter settings */
            xEthHandle.Instance->MACPFR |= ENABLE_HASH_FILTER_SETTINGS;

            /* Configuration for HAL_ETH_Transmit(_IT). */
            memset( &( xTxConfig ), 0, sizeof( ETH_TxPacketConfig ) );
            xTxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;

            #if ( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM != 0 )
            {
                /*xTxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC; */
                xTxConfig.Attributes |= ETH_TX_PACKETS_FEATURES_CSUM;
                xTxConfig.ChecksumCtrl = ETH_DMATXNDESCRF_CIC_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
            }
            #else
            {
                xTxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
            }
            #endif
            xTxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

            /* This counting semaphore will count the number of free TX DMA descriptors. */
            xTXDescriptorSemaphore = xSemaphoreCreateCounting( ( UBaseType_t ) ETH_TX_DESC_CNT, ( UBaseType_t ) ETH_TX_DESC_CNT );
            configASSERT( xTXDescriptorSemaphore );

            xTransmissionMutex = xSemaphoreCreateMutex();
            configASSERT( xTransmissionMutex );

            /* Assign Rx memory buffers to a DMA Rx descriptor */
            for( uxIndex = 0; uxIndex < ETH_RX_DESC_CNT; uxIndex++ )
            {
                uint8_t * pucBuffer;

                #if ( ipconfigZERO_COPY_RX_DRIVER != 0 )
                {
                    pucBuffer = pucGetRXBuffer( ETH_RX_BUF_SIZE );
                    configASSERT( pucBuffer != NULL );
                }
                #else
                {
                    pucBuffer = Rx_Buff[ uxIndex ];
                }
                #endif

                HAL_ETH_DescAssignMemory( &( xEthHandle ), uxIndex, pucBuffer, NULL );
            }

            #if ( ipconfigUSE_MDNS == 1 )
            {
                /* Program the MDNS address. */
                prvSetMAC_HashFilter( &xEthHandle, ( uint8_t * ) xMDNS_MacAddress.ucBytes );
            }
            #endif
            #if ( ( ipconfigUSE_MDNS == 1 ) && ( ipconfigUSE_IPv6 != 0 ) )
            {
                prvSetMAC_HashFilter( &xEthHandle, ( uint8_t * ) xMDNS_MacAddressIPv6.ucBytes );
            }
            #endif
            #if ( ipconfigUSE_LLMNR == 1 )
            {
                /* Program the LLMNR address. */
                prvSetMAC_HashFilter( &xEthHandle, ( uint8_t * ) xLLMNR_MacAddress.ucBytes );
            }
            #endif
            #if ( ( ipconfigUSE_LLMNR == 1 ) && ( ipconfigUSE_IPv6 != 0 ) )
            {
                prvSetMAC_HashFilter( &xEthHandle, ( uint8_t * ) xLLMNR_MacAddressIPv6.ucBytes );
            }
            #endif

            {
                /* The EMAC address of the first end-point has been registered in HAL_ETH_Init(). */
                for( ;
                     pxEndPoint != NULL;
                     pxEndPoint = FreeRTOS_NextEndPoint( pxMyInterface, pxEndPoint ) )
                {
                    switch( pxEndPoint->bits.bIPv6 )
                    {
                        #if ( ipconfigUSE_IPv4 != 0 )
                            case pdFALSE_UNSIGNED:

                                if( xEthHandle.Init.MACAddr != ( uint8_t * ) pxEndPoint->xMACAddress.ucBytes )
                                {
                                    prvSetMAC_HashFilter( &xEthHandle, pxEndPoint->xMACAddress.ucBytes );
                                }
                                break;
                        #endif /* ( ipconfigUSE_IPv4 != 0 ) */

                        #if ( ipconfigUSE_IPv6 != 0 )
                            case pdTRUE_UNSIGNED:
                               {
                                   uint8_t ucMACAddress[ 6 ] = { 0x33, 0x33, 0xff, 0, 0, 0 };

                                   ucMACAddress[ 3 ] = pxEndPoint->ipv6_settings.xIPAddress.ucBytes[ 13 ];
                                   ucMACAddress[ 4 ] = pxEndPoint->ipv6_settings.xIPAddress.ucBytes[ 14 ];
                                   ucMACAddress[ 5 ] = pxEndPoint->ipv6_settings.xIPAddress.ucBytes[ 15 ];

                                   /* Allow traffic destined to Solicited-Node multicast address of this endpoint
                                    * for Duplicate Address Detection (DAD) */
                                   prvSetMAC_HashFilter( &xEthHandle, ucMACAddress );
                               }
                               break;
                        #endif /* ( ipconfigUSE_IPv6 != 0 ) */

                        default:
                            /* MISRA 16.4 Compliance */
                            break;
                    }
                }
            }

            #if ( ipconfigUSE_IPv6 != 0 )
            {
                /* Allow traffic destined to IPv6 all nodes multicast MAC 33:33:00:00:00:01 */
                const uint8_t ucMACAddress[ 6 ] = { 0x33, 0x33, 0, 0, 0, 0x01 };
                prvSetMAC_HashFilter( &xEthHandle, ucMACAddress );
            }
            #endif /* ( ipconfigUSE_IPv6 != 0 ) */

            /* Initialize the MACB and set all PHY properties */
            prvMACBProbePhy();

            /* Force a negotiation with the Switch or Router and wait for LS. */
            prvEthernetUpdateConfig( pdTRUE );

            /* The deferred interrupt handler task is created at the highest
             *  possible priority to ensure the interrupt handler can return directly
             *  to it.  The task's handle is stored in xEMACTaskHandle so interrupts can
             *  notify the task when there is something to process. */
            if( xTaskCreate( prvEMACHandlerTask, niEMAC_HANDLER_TASK_NAME, niEMAC_HANDLER_TASK_STACK_SIZE, NULL, niEMAC_HANDLER_TASK_PRIORITY, &( xEMACTaskHandle ) ) == pdPASS )
            {
                /* The task was created successfully. */
                xMacInitStatus = eMACPass;
            }
            else
            {
                xMacInitStatus = eMACFailed;
            }
        }
        else
        {
            /* HAL_ETH_Init() returned an error, the driver gets into a fatal error sate. */
            xMacInitStatus = eMACFailed;
        }
    } /* ( xMacInitStatus == eMACInit ) */

    if( xMacInitStatus == eMACPass )
    {
        if( xPhyObject.ulLinkStatusMask != 0U )
        {
            xResult = pdPASS;
            FreeRTOS_printf( ( "Link Status is high\n" ) );
        }
        else
        {
            /* For now pdFAIL will be returned. But prvEMACHandlerTask() is running
             * and it will keep on checking the PHY and set 'ulLinkStatusMask' when necessary. */
        }
    }

    return xResult;
}
/*-----------------------------------------------------------*/

static BaseType_t xSTM32H_GetPhyLinkStatus( NetworkInterface_t * pxInterface )
{
    BaseType_t xReturn;

    if( xPhyObject.ulLinkStatusMask != 0U )
    {
        xReturn = pdPASS;
    }
    else
    {
        xReturn = pdFAIL;
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

#if ( ipconfigIPv4_BACKWARD_COMPATIBLE != 0 )

/* Do not call the following function directly. It is there for downward compatibility.
 * The function FreeRTOS_IPInit() will call it to initialice the interface and end-point
 * objects.  See the description in FreeRTOS_Routing.h. */
    NetworkInterface_t * pxFillInterfaceDescriptor( BaseType_t xEMACIndex,
                                                    NetworkInterface_t * pxInterface )
    {
        return pxSTM32H_FillInterfaceDescriptor( xEMACIndex, pxInterface );
    }

#endif
/*-----------------------------------------------------------*/

NetworkInterface_t * pxSTM32H_FillInterfaceDescriptor( BaseType_t xEMACIndex,
                                                       NetworkInterface_t * pxInterface )
{
    static char pcName[ 17 ];

/* This function pxSTM32Hxx_FillInterfaceDescriptor() adds a network-interface.
 * Make sure that the object pointed to by 'pxInterface'
 * is declared static or global, and that it will remain to exist. */

    snprintf( pcName, sizeof( pcName ), "eth%u", ( unsigned ) xEMACIndex );

    memset( pxInterface, '\0', sizeof( *pxInterface ) );
    pxInterface->pcName = pcName;                    /* Just for logging, debugging. */
    pxInterface->pvArgument = ( void * ) xEMACIndex; /* Has only meaning for the driver functions. */
    pxInterface->pfInitialise = xSTM32H_NetworkInterfaceInitialise;
    pxInterface->pfOutput = xSTM32H_NetworkInterfaceOutput;
    pxInterface->pfGetPhyLinkStatus = xSTM32H_GetPhyLinkStatus;

    FreeRTOS_AddNetworkInterface( pxInterface );

    return pxInterface;
}
/*-----------------------------------------------------------*/

static BaseType_t xSTM32H_NetworkInterfaceOutput( NetworkInterface_t * pxInterface,
                                                  NetworkBufferDescriptor_t * const pxBuffer,
                                                  BaseType_t xReleaseAfterSend )
{
    BaseType_t xResult = pdFAIL;
    TickType_t xBlockTimeTicks = pdMS_TO_TICKS( 100U );
    uint8_t * pucTXBuffer;

    if( xSTM32H_GetPhyLinkStatus( pxInterface ) == pdPASS )
    {
        #if ( ipconfigZERO_COPY_TX_DRIVER != 0 )
            /* Zero-copy method, pass the buffer. */
            pucTXBuffer = pxBuffer->pucEthernetBuffer;

            /* As the buffer is passed to the driver, it must exist.
             * The library takes care of this. */
            configASSERT( xReleaseAfterSend != pdFALSE );
        #else
            pucTXBuffer = Tx_Buff[ xEthHandle.TxDescList.CurTxDesc ];
            /* The copy method, left here for educational purposes. */
            configASSERT( pxBuffer->xDataLength <= sizeof( Tx_Buff[ 0 ] ) );
        #endif

        ETH_BufferTypeDef xTransmitBuffer =
        {
            .buffer = pucTXBuffer,
            .len    = pxBuffer->xDataLength,
            .next   = NULL /* FreeRTOS+TCP does not use linked buffers. */
        };
        /* This is the total length, which is equal to the buffer. */
        xTxConfig.Length = pxBuffer->xDataLength;
        xTxConfig.TxBuffer = &( xTransmitBuffer );

        /* This counting semaphore counts the number of free TX DMA descriptors. */
        if( xSemaphoreTake( xTXDescriptorSemaphore, xBlockTimeTicks ) != pdPASS )
        {
            /* If the logging routine is using the network, the following message
             * may cause a new error message. */
            FreeRTOS_printf( ( "emacps_send_message: Time-out waiting for TX buffer\n" ) );
        }
        else
        {
            /* Memory barrier: Make sure that the data written to the packet buffer got written. */
            __DSB();

            /* Get exclusive access to the TX process.
             * Both the IP-task and the EMAC task will work on the TX process. */
            if( xSemaphoreTake( xTransmissionMutex, xBlockTimeTicks ) != pdFAIL )
            {
                #if ( ipconfigZERO_COPY_TX_DRIVER != 0 )
                {
                    /* Do not release the buffer. */
                    xReleaseAfterSend = pdFALSE;
                }
                #else
                {
                    memcpy( pucTXBuffer, pxBuffer->pucEthernetBuffer, pxBuffer->xDataLength );

                    /* A memory barrier to make sure that the outgoing packets has been written
                     * to the physical memory. */
                    __DSB();
                }
                #endif /* if ( ipconfigZERO_COPY_TX_DRIVER != 0 ) */

                if( HAL_ETH_Transmit_IT( &( xEthHandle ), &( xTxConfig ) ) == HAL_OK )
                {
                    xResult = pdPASS;
                }
                else
                {
                    /* As the transmission packet was not queued,
                     * the counting semaphore should be given. */
                    xSemaphoreGive( xTXDescriptorSemaphore );
                }

                /* And release the mutex. */
                xSemaphoreGive( xTransmissionMutex );
            }

            /* Call the standard trace macro to log the send event. */
            iptraceNETWORK_INTERFACE_TRANSMIT();
        }
    }

    if( xReleaseAfterSend != pdFALSE )
    {
        vReleaseNetworkBufferAndDescriptor( pxBuffer );
    }

    return xResult;
}
/*-----------------------------------------------------------*/

/*******************************************************************************
*                      END Network Interface API Functions
*******************************************************************************/



/*******************************************************************************
*                      Network Interface Static Functions
*******************************************************************************/

static void prvMACBProbePhy( void )
{
    /* Bind the write and read access functions. */
    vPhyInitialise( &( xPhyObject ),
                    ( xApplicationPhyReadHook_t ) ETH_PHY_IO_ReadReg,
                    ( xApplicationPhyWriteHook_t ) ETH_PHY_IO_WriteReg );
    /* Poll the bus for all connected PHY's. */
    xPhyDiscover( &( xPhyObject ) );
    /* Configure them using the properties provided. */
    xPhyConfigure( &( xPhyObject ), &( xPHYProperties ) );
}
/*-----------------------------------------------------------*/

static void prvEthernetUpdateConfig( BaseType_t xForce )
{
    ETH_MACConfigTypeDef MACConf;
    uint32_t speed = 0, duplex = 0;

    FreeRTOS_printf( ( "prvEthernetUpdateConfig: LS mask %02lX Force %d\n",
                       xPhyObject.ulLinkStatusMask,
                       ( int ) xForce ) );

    if( ( xForce != pdFALSE ) || ( xPhyObject.ulLinkStatusMask != 0 ) )
    {
        /* Restart the auto-negotiation. */
        xPhyStartAutoNegotiation( &xPhyObject, xPhyGetMask( &( xPhyObject ) ) );

        /* Configure the MAC with the Duplex Mode fixed by the
         * auto-negotiation process. */
        if( xPhyObject.xPhyProperties.ucDuplex == PHY_DUPLEX_FULL )
        {
            duplex = ETH_FULLDUPLEX_MODE;
        }
        else
        {
            duplex = ETH_HALFDUPLEX_MODE;
        }

        /* Configure the MAC with the speed fixed by the
         * auto-negotiation process. */
        if( xPhyObject.xPhyProperties.ucSpeed == PHY_SPEED_10 )
        {
            speed = ETH_SPEED_10M;
        }
        else
        {
            speed = ETH_SPEED_100M;
        }

        /* Get MAC and configure it */
        HAL_ETH_GetMACConfig( &( xEthHandle ), &( MACConf ) );
        MACConf.DuplexMode = duplex;
        MACConf.Speed = speed;
        HAL_ETH_SetMACConfig( &( xEthHandle ), &( MACConf ) );
        #if ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM != 0 )
        {
            MACConf.ChecksumOffload = ENABLE;
        }
        #else
        {
            MACConf.ChecksumOffload = DISABLE;
        }
        #endif /* ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM != 0 ) */

        /* Restart MAC interface */
        HAL_ETH_Start_IT( &( xEthHandle ) );
    }
    else
    {
        /* Stop MAC interface */
        HAL_ETH_Stop_IT( &( xEthHandle ) );
    }
}
/*-----------------------------------------------------------*/

static BaseType_t prvNetworkInterfaceInput( void )
{
    BaseType_t xReturn = 0;

    /* For as long as a packet is immediately available. */
    for( ; ; )
    {
        NetworkBufferDescriptor_t * pxBufferDescriptor;
        NetworkBufferDescriptor_t * pxReceivedBuffer = NULL;
        ETH_BufferTypeDef data_buffer;
        size_t uxDataLength;
        size_t uxLength;

        uxDataLength = HAL_ETH_GetRxData( &( xEthHandle ), &( data_buffer ) );

        if( uxDataLength == 0U )
        {
            /* No more packets received. */
            break;
        }

        xReturn++;

        #if ( ipconfigZERO_COPY_RX_DRIVER != 0 )
        {
            /* Reserve the maximum length for the next reception. */
            uxLength = ETH_RX_BUF_SIZE;

            if( data_buffer.buffer != NULL )
            {
                pxReceivedBuffer = pxPacketBuffer_to_NetworkBuffer( data_buffer.buffer );
                #if ( ipconfigTCP_IP_SANITY != 0 )
                {
                    configASSERT( bIsValidNetworkDescriptor( pxReceivedBuffer ) != 0 );
                }
                #endif
            }

            if( pxReceivedBuffer == NULL )
            {
                FreeRTOS_printf( ( "Strange: no descriptor received\n" ) );
            }
        }
        #else /* if ( ipconfigZERO_COPY_RX_DRIVER != 0 ) */
        {
            /* Reserve the length of the packet that was just received. */
            uxLength = uxDataLength;
        }
        #endif /* if ( ipconfigZERO_COPY_RX_DRIVER != 0 ) */

        pxBufferDescriptor = pxGetNetworkBufferWithDescriptor( uxLength, 0u );

        if( pxBufferDescriptor == NULL )
        {
            /* The event was lost because a network buffer was not available.
             * Call the standard trace macro to log the occurrence. */
            iptraceETHERNET_RX_EVENT_LOST();
        }

        #if ( ipconfigZERO_COPY_RX_DRIVER != 0 )
        {
            if( pxBufferDescriptor == NULL )
            {
                /* Can not receive this packet. Buffer will be re-used. */
                pxReceivedBuffer = NULL;
            }
            else if( pxReceivedBuffer != NULL )
            {
                pxReceivedBuffer->xDataLength = uxDataLength;
            }
            else
            {
                /* Allocating a new buffer failed. */
            }
        }
        #else /* if ( ipconfigZERO_COPY_RX_DRIVER != 0 ) */
        {
            if( pxBufferDescriptor != NULL )
            {
                pxReceivedBuffer = pxBufferDescriptor;
                /* The copy method. */
                memcpy( pxReceivedBuffer->pucEthernetBuffer, data_buffer.buffer, uxDataLength );
                pxReceivedBuffer->xDataLength = uxDataLength;
                /* Make sure that the descriptor isn't used any more. */
                pxBufferDescriptor = NULL;
            }
        }
        #endif /* if ( ipconfigZERO_COPY_RX_DRIVER != 0 ) */

        {
            uint8_t * pucBuffer = NULL;

            if( pxBufferDescriptor != NULL )
            {
                pucBuffer = pxBufferDescriptor->pucEthernetBuffer;
            }

            /* Assign an RX buffer to the descriptor, so that
             * a next packet can be received. */
            HAL_ETH_BuildRxDescriptors( &( xEthHandle ), pucBuffer );
        }

        /* See if the data contained in the received Ethernet frame needs
        * to be processed.  NOTE! It is preferable to do this in
        * the interrupt service routine itself, which would remove the need
        * to unblock this task for packets that don't need processing. */

        if( pxReceivedBuffer != NULL )
        {
            BaseType_t xDoRelease = pdFALSE;

            if( eConsiderFrameForProcessing( pxReceivedBuffer->pucEthernetBuffer ) != eProcessBuffer )
            {
                /* The Ethernet frame can be dropped, but the Ethernet buffer must be released. */
                xDoRelease = pdTRUE;
            }
            else
            {
                /* The event about to be sent to the TCP/IP is an Rx event.
                 * pvData is used to point to the network buffer descriptor that
                 * now references the received data. */

                IPStackEvent_t xRxEvent =
                {
                    .eEventType = eNetworkRxEvent,
                    .pvData     = ( void * ) pxReceivedBuffer
                };

                pxReceivedBuffer->pxInterface = pxMyInterface;
                pxReceivedBuffer->pxEndPoint = FreeRTOS_MatchingEndpoint( pxMyInterface, pxReceivedBuffer->pucEthernetBuffer );

                /* Send the data to the TCP/IP stack. */
                if( xSendEventStructToIPTask( &( xRxEvent ), 0 ) != pdFALSE )
                {
                    /* The message was successfully sent to the TCP/IP stack.
                    * Call the standard trace macro to log the occurrence. */
                    iptraceNETWORK_INTERFACE_RECEIVE();
                }
                else
                {
                    xDoRelease = pdTRUE;

                    /* The buffer could not be sent to the IP task so the buffer
                     * must be released. */

                    /* Make a call to the standard trace macro to log the
                     * occurrence. */
                    iptraceETHERNET_RX_EVENT_LOST();
                }
            }

            if( xDoRelease != pdFALSE )
            {
                vReleaseNetworkBufferAndDescriptor( pxReceivedBuffer );
            }
        }
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

/*******************************************************************************
*                   END Network Interface Static Functions
*******************************************************************************/



/*******************************************************************************
*                   PHY IO Functions
*******************************************************************************/

/**
 * @brief  Read a PHY register through the MDIO interface.
 * @param  DevAddr: PHY port address
 * @param  RegAddr: PHY register address
 * @param  pRegVal: pointer to hold the register value
 * @retval 0 if OK -1 if Error
 */
static int32_t ETH_PHY_IO_ReadReg( uint32_t ulDevAddr,
                                   uint32_t ulRegAddr,
                                   uint32_t * pulRegVal )
{
    int32_t iResult = -1;

    if( HAL_ETH_ReadPHYRegister( &( xEthHandle ), ulDevAddr, ulRegAddr, pulRegVal ) == HAL_OK )
    {
        iResult = 0;
    }

    return iResult;
}
/*-----------------------------------------------------------*/

/**
 * @brief  Write a value to a PHY register through the MDIO interface.
 * @param  DevAddr: PHY port address
 * @param  RegAddr: PHY register address
 * @param  RegVal: Value to be written
 * @retval 0 if OK -1 if Error
 */
static int32_t ETH_PHY_IO_WriteReg( uint32_t ulDevAddr,
                                    uint32_t ulRegAddr,
                                    uint32_t pulRegVal )
{
    int32_t iResult = -1;

    if( HAL_ETH_WritePHYRegister( &( xEthHandle ), ulDevAddr, ulRegAddr, pulRegVal ) == HAL_OK )
    {
        iResult = 0;
    }

    return iResult;
}
/*-----------------------------------------------------------*/

/*******************************************************************************
*                   END PHY IO Functions
*******************************************************************************/



/*******************************************************************************
*                   Ethernet Handling Functions
*******************************************************************************/

void ETH_IRQHandler( void )
{
    HAL_ETH_IRQHandler( &( xEthHandle ) );
}
/*-----------------------------------------------------------*/

static void prvSetFlagsAndNotify( uint32_t ulFlags )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Ethernet RX-Complete callback function, elsewhere declared as weak.
     * No critical section needed, this function is called from an ISR. */
    ulISREvents |= ulFlags;

    /* Wakeup the prvEMACHandlerTask. */
    if( xEMACTaskHandle != NULL )
    {
        vTaskNotifyGiveFromISR( xEMACTaskHandle, &( xHigherPriorityTaskWoken ) );
        portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    }
}
/*-----------------------------------------------------------*/

void HAL_ETH_TxCpltCallback( ETH_HandleTypeDef * heth )
{
    ( void ) heth;
    prvSetFlagsAndNotify( EMAC_IF_TX_EVENT );
}
/*-----------------------------------------------------------*/

void HAL_ETH_RxCpltCallback( ETH_HandleTypeDef * heth )
{
    ( void ) heth;
    prvSetFlagsAndNotify( EMAC_IF_RX_EVENT );
}
/*-----------------------------------------------------------*/

void HAL_ETH_DMAErrorCallback( ETH_HandleTypeDef * heth )
{
    ( void ) heth;
    prvSetFlagsAndNotify( EMAC_IF_ERR_EVENT );
}
/*-----------------------------------------------------------*/

/*******************************************************************************
*                   END Ethernet Handling Functions
*******************************************************************************/

uint8_t ucNetworkPackets[ ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS * ETH_RX_BUF_SIZE ]
#if ( ipconfigZERO_COPY_RX_DRIVER != 0 || ipconfigZERO_COPY_TX_DRIVER != 0 )
    __attribute__( ( section( ".ethernet_data" ) ) )
#endif /* ( ipconfigZERO_COPY_RX_DRIVER != 0 || ipconfigZERO_COPY_TX_DRIVER != 0 ) */
__attribute__( ( aligned( 32 ) ) );

size_t uxNetworkInterfaceAllocateRAMToBuffers( NetworkBufferDescriptor_t pxNetworkBuffers[ ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS ] )
{
    uint8_t * ucRAMBuffer = ucNetworkPackets;
    uint32_t ul;

    for( ul = 0; ul < ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS; ul++ )
    {
        pxNetworkBuffers[ ul ].pucEthernetBuffer = ucRAMBuffer + ipBUFFER_PADDING;
        *( ( unsigned * ) ucRAMBuffer ) = ( unsigned ) ( &( pxNetworkBuffers[ ul ] ) );
        ucRAMBuffer += ETH_RX_BUF_SIZE;
    }

    return (ETH_RX_BUF_SIZE - ipBUFFER_PADDING);
}
/*-----------------------------------------------------------*/

static void vClearOptionBit( volatile uint32_t * pulValue,
                             uint32_t ulValue )
{
    portENTER_CRITICAL();
    *( pulValue ) &= ~( ulValue );
    portEXIT_CRITICAL();
}
/*-----------------------------------------------------------*/

#if ( ipconfigHAS_PRINTF != 0 )
    static size_t uxGetOwnCount( ETH_HandleTypeDef * heth )
    {
        BaseType_t xIndex;
        BaseType_t xCount = 0;
        ETH_RxDescListTypeDef * dmarxdesclist = &heth->RxDescList;

        /* Count the number of RX descriptors that are owned by DMA. */
        for( xIndex = 0; xIndex < ETH_RX_DESC_CNT; xIndex++ )
        {
            __IO const ETH_DMADescTypeDef * dmarxdesc =
                ( __IO const ETH_DMADescTypeDef * )dmarxdesclist->RxDesc[ xIndex ];

            if( ( dmarxdesc->DESC3 & ETH_DMARXNDESCWBF_OWN ) != 0U )
            {
                xCount++;
            }
        }

        return xCount;
    }
#endif /* if ( ipconfigHAS_PRINTF != 0 ) */
/*-----------------------------------------------------------*/

static void prvEMACHandlerTask( void * pvParameters )
{
/* When sending a packet, all descriptors in the transmission channel may
 * be occupied.  In stat case, the program will wait (block) for the counting
 * semaphore. */
    const TickType_t ulMaxBlockTime = pdMS_TO_TICKS( 100U );

    #if ( ipconfigHAS_PRINTF != 0 )
        size_t uxTXDescriptorsUsed = 0U;
        size_t uxRXDescriptorsUsed = ETH_RX_DESC_CNT;
    #endif

    ( void ) pvParameters;

    for( ; ; )
    {
        BaseType_t xResult = 0;

        #if ( ipconfigHAS_PRINTF != 0 )
        {
            size_t uxUsed;
            size_t uxOwnCount;

            /* Call a function that monitors resources: the amount of free network
             * buffers and the amount of free space on the heap.  See FreeRTOS_IP.c
             * for more detailed comments. */
            vPrintResourceStats();

            /* Some more statistics: number of free descriptors. */
            uxUsed = ETH_TX_DESC_CNT - uxSemaphoreGetCount( xTXDescriptorSemaphore );

            if( uxTXDescriptorsUsed < uxUsed )
            {
                uxTXDescriptorsUsed = uxUsed;
                FreeRTOS_printf( ( "TX descriptors %u/%u\n",
                                   uxTXDescriptorsUsed,
                                   ETH_TX_DESC_CNT ) );
            }

            uxOwnCount = uxGetOwnCount( &( xEthHandle ) );

            if( uxRXDescriptorsUsed > uxOwnCount )
            {
                uxRXDescriptorsUsed = uxOwnCount;
                FreeRTOS_printf( ( "RX descriptors %u/%u\n",
                                   uxRXDescriptorsUsed,
                                   ETH_RX_DESC_CNT ) );
            }
        }
        #endif /* ( ipconfigHAS_PRINTF != 0 ) */

        ulTaskNotifyTake( pdFALSE, ulMaxBlockTime );

        /* Wait for the Ethernet MAC interrupt to indicate that another packet
         * has been received. */
        if( ( ulISREvents & EMAC_IF_RX_EVENT ) != 0U )
        {
            vClearOptionBit( &( ulISREvents ), EMAC_IF_RX_EVENT );
            xResult = prvNetworkInterfaceInput();
        }

        /* When a packet has been transmitted, the descriptor must be
         * prepared for a next transmission.
         * When using zero-copy, the network buffer must be released
         * ( i.e. returned to the pool of network buffers ). */

        if( ( ulISREvents & EMAC_IF_TX_EVENT ) != 0U )
        {
            vClearOptionBit( &( ulISREvents ), EMAC_IF_TX_EVENT );

            if( xSemaphoreTake( xTransmissionMutex, 10000U ) != pdFAIL )
            {
                ETH_Clear_Tx_Descriptors( &( xEthHandle ) );
                xSemaphoreGive( xTransmissionMutex );
            }
        }

        /* Some error has occurred, possibly an overflow or an underflow. */
        if( ( ulISREvents & EMAC_IF_ERR_EVENT ) != 0U )
        {
            vClearOptionBit( &( ulISREvents ), EMAC_IF_ERR_EVENT );

            xEthHandle.gState = HAL_ETH_STATE_READY;
            /* Enable all interrupts */
            HAL_ETH_Start_IT( &( xEthHandle ) );
            xResult += prvNetworkInterfaceInput();
        }

        if( xPhyCheckLinkStatus( &xPhyObject, xResult ) != pdFALSE )
        {
            /*
             * The function xPhyCheckLinkStatus() returns pdTRUE if the
             * Link Status has changes since it was called the last time.
             */
            if( xSTM32H_GetPhyLinkStatus( pxMyInterface ) == pdFALSE )
            {
                #if ( ipconfigSUPPORT_NETWORK_DOWN_EVENT != 0 )
                {
                    FreeRTOS_NetworkDown( pxMyInterface );
                }
                #endif /* ( ipconfigSUPPORT_NETWORK_DOWN_EVENT != 0 ) */

                /* Stop the DMA transfer. */
                HAL_ETH_Stop_IT( &( xEthHandle ) );
                /* Clear the Transmit buffers. */
                memset( &( DMATxDscrTab ), '\0', sizeof( DMATxDscrTab ) );
                /* Since the link is down, clear the descriptors. */
                ETH_Clear_Tx_Descriptors( &( xEthHandle ) );
            }
            else
            {
                /* Something has changed to a Link Status, need re-check. */
                prvEthernetUpdateConfig( pdFALSE );
            }
        }

        ipconfigEMAC_TASK_HOOK();
    }
}

/*-----------------------------------------------------------*/
