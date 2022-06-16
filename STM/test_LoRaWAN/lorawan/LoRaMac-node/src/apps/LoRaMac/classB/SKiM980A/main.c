/*!
 * \file      main.c
 *
 * \brief     LoRaMac classB device implementation
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 *
 * \author    Andreas Pella (IMST GmbH)
 */

/*! \file classB/SKiM980A/main.c */

#include <stdio.h>
#include "utilities.h"
#include "board.h"
#include "gpio.h"
#include "LoRaMac.h"
#include "Commissioning.h"
#include "NvmCtxMgmt.h"

#ifndef ACTIVE_REGION

#warning "No active region defined, LORAMAC_REGION_EU868 will be used as default."

#define ACTIVE_REGION LORAMAC_REGION_EU868

#endif

/*!
 * Uncomment to use the deprecated BeaconTiming MAC command
 */
//#define USE_BEACON_TIMING

/*!
 * Enables/Disables the setup of a local Multicast Channel Setup.
 */
#define LOCAL_MULTICAST_SETUP_ENABLED               0

/*!
 * Enables/Disables the ping slot frequency hopping.
 */
#define LOCAL_MULTICAST_SETUP_DISABLE_SLOT_HOP      0

/*!
 * Defines the application data transmission duty cycle. 30s, value in [ms].
 */
#define APP_TX_DUTYCYCLE                            30000

/*!
 * Defines a random delay for application data transmission duty cycle. 5s,
 * value in [ms].
 */
#define APP_TX_DUTYCYCLE_RND                        5000

/*!
 * Default datarate
 */
#define LORAWAN_DEFAULT_DATARATE                    DR_0

/*!
 * Default ping slots periodicity
 *
 * \remark periodicity is equal to 2^LORAWAN_DEFAULT_PING_SLOT_PERIODICITY seconds
 *         example: 2^3 = 8 seconds. The end-device will open an Rx slot every 8 seconds.
 */
#define LORAWAN_DEFAULT_PING_SLOT_PERIODICITY       0

/*!
 * LoRaWAN confirmed messages
 */
#define LORAWAN_CONFIRMED_MSG_ON                    false

/*!
 * LoRaWAN Adaptive Data Rate
 *
 * \remark Please note that when ADR is enabled the end-device should be static
 */
#define LORAWAN_ADR_ON                              1

#if defined( REGION_EU868 ) || defined( REGION_RU864 ) || defined( REGION_CN779 ) || defined( REGION_EU433 )

#include "LoRaMacTest.h"

/*!
 * LoRaWAN ETSI duty cycle control enable/disable
 *
 * \remark Please note that ETSI mandates duty cycled transmissions. Use only for test purposes
 */
#define LORAWAN_DUTYCYCLE_ON                        true

#endif

/*!
 * LoRaWAN application port
 */
#define LORAWAN_APP_PORT                            3

#if( OVER_THE_AIR_ACTIVATION == 0 )
/*!
 * Device address
 */
static uint32_t DevAddr = LORAWAN_DEVICE_ADDRESS;
#endif

/*!
 * Application port
 */
static uint8_t AppPort = LORAWAN_APP_PORT;

/*!
 * User application data size
 */
static uint8_t AppDataSize = 4;
static uint8_t AppDataSizeBackup = 4;

/*!
 * User application data buffer size
 */
#define LORAWAN_APP_DATA_MAX_SIZE                           242

/*!
 * User application data
 */
static uint8_t AppDataBuffer[LORAWAN_APP_DATA_MAX_SIZE];

/*!
 * Indicates if the node is sending confirmed or unconfirmed messages
 */
static uint8_t IsTxConfirmed = LORAWAN_CONFIRMED_MSG_ON;

/*!
 * Defines the application data transmission duty cycle
 */
static uint32_t TxDutyCycleTime;

/*!
 * Timer to handle the application data transmission duty cycle
 */
static TimerEvent_t TxNextPacketTimer;

/*!
 * Specifies the state of the application LED
 */
static bool AppLedStateOn = false;

/*!
 * Timer to handle the state of LED4
 */
static TimerEvent_t Led4Timer;

/*!
 * Timer to handle the state of LED2
 */
static TimerEvent_t Led2Timer;

/*!
 * Timer to handle the state of LED beacon indicator
 */
static TimerEvent_t LedBeaconTimer;

/*!
 * Indicates if a new packet can be sent
 */
static bool NextTx = true;

/*!
 * Indicates if LoRaMacProcess call is pending.
 * 
 * \warning If variable is equal to 0 then the MCU can be set in low power mode
 */
static uint8_t IsMacProcessPending = 0;

/*!
 * Device states
 */
static enum eDeviceState
{
    DEVICE_STATE_RESTORE,
    DEVICE_STATE_START,
    DEVICE_STATE_JOIN,
    DEVICE_STATE_SEND,
    DEVICE_STATE_REQ_DEVICE_TIME,
    DEVICE_STATE_REQ_PINGSLOT_ACK,
    DEVICE_STATE_REQ_BEACON_TIMING,
    DEVICE_STATE_BEACON_ACQUISITION,
    DEVICE_STATE_SWITCH_CLASS,
    DEVICE_STATE_CYCLE,
    DEVICE_STATE_SLEEP
}DeviceState, WakeUpState;

/*!
 * LoRaWAN compliance tests support data
 */
struct ComplianceTest_s
{
    bool Running;
    uint8_t State;
    bool IsTxConfirmed;
    uint8_t AppPort;
    uint8_t AppDataSize;
    uint8_t *AppDataBuffer;
    uint16_t DownLinkCounter;
    bool LinkCheck;
    uint8_t DemodMargin;
    uint8_t NbGateways;
}ComplianceTest;

/*!
 *
 */
typedef enum
{
    LORAMAC_HANDLER_UNCONFIRMED_MSG = 0,
    LORAMAC_HANDLER_CONFIRMED_MSG = !LORAMAC_HANDLER_UNCONFIRMED_MSG
}LoRaMacHandlerMsgTypes_t;

/*!
 * Application data structure
 */
typedef struct LoRaMacHandlerAppData_s
{
    LoRaMacHandlerMsgTypes_t MsgType;
    uint8_t Port;
    uint8_t BufferSize;
    uint8_t *Buffer;
}LoRaMacHandlerAppData_t;

LoRaMacHandlerAppData_t AppData =
{
    .MsgType = LORAMAC_HANDLER_UNCONFIRMED_MSG,
    .Buffer = NULL,
    .BufferSize = 0,
    .Port = 0
};

/*!
 * LED GPIO pins objects
 */
extern Gpio_t Led4; // Tx
extern Gpio_t Led2; // Rx and blinks every 5 seconds when beacon is acquired
extern Gpio_t Led3; // App

/*!
 * MAC status strings
 */
const char* MacStatusStrings[] =
{
    "OK",                            // LORAMAC_STATUS_OK
    "Busy",                          // LORAMAC_STATUS_BUSY
    "Service unknown",               // LORAMAC_STATUS_SERVICE_UNKNOWN
    "Parameter invalid",             // LORAMAC_STATUS_PARAMETER_INVALID
    "Frequency invalid",             // LORAMAC_STATUS_FREQUENCY_INVALID
    "Datarate invalid",              // LORAMAC_STATUS_DATARATE_INVALID
    "Frequency or datarate invalid", // LORAMAC_STATUS_FREQ_AND_DR_INVALID
    "No network joined",             // LORAMAC_STATUS_NO_NETWORK_JOINED
    "Length error",                  // LORAMAC_STATUS_LENGTH_ERROR
    "Region not supported",          // LORAMAC_STATUS_REGION_NOT_SUPPORTED
    "Skipped APP data",              // LORAMAC_STATUS_SKIPPED_APP_DATA
    "Duty-cycle restricted",         // LORAMAC_STATUS_DUTYCYCLE_RESTRICTED
    "No channel found",              // LORAMAC_STATUS_NO_CHANNEL_FOUND
    "No free channel found",         // LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND
    "Busy beacon reserved time",     // LORAMAC_STATUS_BUSY_BEACON_RESERVED_TIME
    "Busy ping-slot window time",    // LORAMAC_STATUS_BUSY_PING_SLOT_WINDOW_TIME
    "Busy uplink collision",         // LORAMAC_STATUS_BUSY_UPLINK_COLLISION
    "Crypto error",                  // LORAMAC_STATUS_CRYPTO_ERROR
    "FCnt handler error",            // LORAMAC_STATUS_FCNT_HANDLER_ERROR
    "MAC command error",             // LORAMAC_STATUS_MAC_COMMAD_ERROR
    "ClassB error",                  // LORAMAC_STATUS_CLASS_B_ERROR
    "Confirm queue error",           // LORAMAC_STATUS_CONFIRM_QUEUE_ERROR
    "Multicast group undefined",     // LORAMAC_STATUS_MC_GROUP_UNDEFINED
    "Unknown error",                 // LORAMAC_STATUS_ERROR
};

/*!
 * MAC event info status strings.
 */
const char* EventInfoStatusStrings[] =
{ 
    "OK",                            // LORAMAC_EVENT_INFO_STATUS_OK
    "Error",                         // LORAMAC_EVENT_INFO_STATUS_ERROR
    "Tx timeout",                    // LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT
    "Rx 1 timeout",                  // LORAMAC_EVENT_INFO_STATUS_RX1_TIMEOUT
    "Rx 2 timeout",                  // LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT
    "Rx1 error",                     // LORAMAC_EVENT_INFO_STATUS_RX1_ERROR
    "Rx2 error",                     // LORAMAC_EVENT_INFO_STATUS_RX2_ERROR
    "Join failed",                   // LORAMAC_EVENT_INFO_STATUS_JOIN_FAIL
    "Downlink repeated",             // LORAMAC_EVENT_INFO_STATUS_DOWNLINK_REPEATED
    "Tx DR payload size error",      // LORAMAC_EVENT_INFO_STATUS_TX_DR_PAYLOAD_SIZE_ERROR
    "Downlink too many frames loss", // LORAMAC_EVENT_INFO_STATUS_DOWNLINK_TOO_MANY_FRAMES_LOSS
    "Address fail",                  // LORAMAC_EVENT_INFO_STATUS_ADDRESS_FAIL
    "MIC fail",                      // LORAMAC_EVENT_INFO_STATUS_MIC_FAIL
    "Multicast fail",                // LORAMAC_EVENT_INFO_STATUS_MULTICAST_FAIL
    "Beacon locked",                 // LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED
    "Beacon lost",                   // LORAMAC_EVENT_INFO_STATUS_BEACON_LOST
    "Beacon not found"               // LORAMAC_EVENT_INFO_STATUS_BEACON_NOT_FOUND
};

/*!
 * Prints the provided buffer in HEX
 * 
 * \param buffer Buffer to be printed
 * \param size   Buffer size to be printed
 */
void PrintHexBuffer( uint8_t *buffer, uint8_t size )
{
    uint8_t newline = 0;

    for( uint8_t i = 0; i < size; i++ )
    {
        if( newline != 0 )
        {
            printf( "\n" );
            newline = 0;
        }

        printf( "%02X ", buffer[i] );

        if( ( ( i + 1 ) % 16 ) == 0 )
        {
            newline = 1;
        }
    }
    printf( "\n" );
}

/*!
 * Executes the network Join request
 */
static void JoinNetwork( void )
{
    LoRaMacStatus_t status;
    MlmeReq_t mlmeReq;
    mlmeReq.Type = MLME_JOIN;
    mlmeReq.Req.Join.Datarate = LORAWAN_DEFAULT_DATARATE;

    // Starts the join procedure
    status = LoRaMacMlmeRequest( &mlmeReq );
    printf( "\n###### ===== MLME-Request - MLME_JOIN ==== ######\n" );
    printf( "STATUS      : %s\n", MacStatusStrings[status] );

    if( status == LORAMAC_STATUS_OK )
    {
        printf( "###### ===== JOINING ==== ######\n" );
        DeviceState = DEVICE_STATE_SLEEP;
    }
    else
    {
        if( status == LORAMAC_STATUS_DUTYCYCLE_RESTRICTED )
        {
            printf( "Next Tx in  : %lu [ms]\n", mlmeReq.ReqReturn.DutyCycleWaitTime );
        }
        DeviceState = DEVICE_STATE_CYCLE;
    }
}

/*!
 * \brief   Prepares the payload of the frame
 */
static void PrepareTxFrame( uint8_t port )
{
    switch( port )
    {
    case 3:
        {
            uint8_t potiPercentage = 0;
            uint16_t vdd = 0;

            // Read the current potentiometer setting in percent
            potiPercentage = BoardGetPotiLevel( );

            // Read the current voltage level
            BoardGetBatteryLevel( ); // Updates the value returned by BoardGetBatteryVoltage( ) function.
            vdd = BoardGetBatteryVoltage( );

            AppDataSizeBackup = AppDataSize = 4;
            AppDataBuffer[0] = AppLedStateOn;
            AppDataBuffer[1] = potiPercentage;
            AppDataBuffer[2] = ( vdd >> 8 ) & 0xFF;
            AppDataBuffer[3] = vdd & 0xFF;
        }
        break;
    case 224:
        if( ComplianceTest.LinkCheck == true )
        {
            ComplianceTest.LinkCheck = false;
            AppDataSize = 3;
            AppDataBuffer[0] = 5;
            AppDataBuffer[1] = ComplianceTest.DemodMargin;
            AppDataBuffer[2] = ComplianceTest.NbGateways;
            ComplianceTest.State = 1;
        }
        else
        {
            switch( ComplianceTest.State )
            {
            case 4:
                ComplianceTest.State = 1;
                break;
            case 1:
                AppDataSize = 2;
                AppDataBuffer[0] = ComplianceTest.DownLinkCounter >> 8;
                AppDataBuffer[1] = ComplianceTest.DownLinkCounter;
                break;
            }
        }
        break;
    default:
        break;
    }
}

/*!
 * \brief   Prepares the payload of the frame
 *
 * \retval  [0: frame could be send, 1: error]
 */
static bool SendFrame( void )
{
    McpsReq_t mcpsReq;
    LoRaMacTxInfo_t txInfo;

    if( LoRaMacQueryTxPossible( AppDataSize, &txInfo ) != LORAMAC_STATUS_OK )
    {
        // Send empty frame in order to flush MAC commands
        mcpsReq.Type = MCPS_UNCONFIRMED;
        mcpsReq.Req.Unconfirmed.fBuffer = NULL;
        mcpsReq.Req.Unconfirmed.fBufferSize = 0;
        mcpsReq.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
    }
    else
    {
        if( IsTxConfirmed == false )
        {
            mcpsReq.Type = MCPS_UNCONFIRMED;
            mcpsReq.Req.Unconfirmed.fPort = AppPort;
            mcpsReq.Req.Unconfirmed.fBuffer = AppDataBuffer;
            mcpsReq.Req.Unconfirmed.fBufferSize = AppDataSize;
            mcpsReq.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
        }
        else
        {
            mcpsReq.Type = MCPS_CONFIRMED;
            mcpsReq.Req.Confirmed.fPort = AppPort;
            mcpsReq.Req.Confirmed.fBuffer = AppDataBuffer;
            mcpsReq.Req.Confirmed.fBufferSize = AppDataSize;
            mcpsReq.Req.Confirmed.NbTrials = 8;
            mcpsReq.Req.Confirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
        }
    }

    // Update global variable
    AppData.MsgType = ( mcpsReq.Type == MCPS_CONFIRMED ) ? LORAMAC_HANDLER_CONFIRMED_MSG : LORAMAC_HANDLER_UNCONFIRMED_MSG;
    AppData.Port = mcpsReq.Req.Unconfirmed.fPort;
    AppData.Buffer = mcpsReq.Req.Unconfirmed.fBuffer;
    AppData.BufferSize = mcpsReq.Req.Unconfirmed.fBufferSize;

    LoRaMacStatus_t status;
    status = LoRaMacMcpsRequest( &mcpsReq );
    printf( "\n###### ===== MCPS-Request ==== ######\n" );
    printf( "STATUS      : %s\n", MacStatusStrings[status] );

    if( status == LORAMAC_STATUS_DUTYCYCLE_RESTRICTED )
    {
        printf( "Next Tx in  : %lu [ms]\n", mcpsReq.ReqReturn.DutyCycleWaitTime );
    }

    if( status == LORAMAC_STATUS_OK )
    {
        return false;
    }
    return true;
}

/*!
 * \brief Function executed on TxNextPacket Timeout event
 */
static void OnTxNextPacketTimerEvent( void* context )
{
    MibRequestConfirm_t mibReq;
    LoRaMacStatus_t status;

    TimerStop( &TxNextPacketTimer );

    mibReq.Type = MIB_NETWORK_ACTIVATION;
    status = LoRaMacMibGetRequestConfirm( &mibReq );

    if( status == LORAMAC_STATUS_OK )
    {
        if( mibReq.Param.NetworkActivation == ACTIVATION_TYPE_NONE )
        {
            // Network not joined yet. Try to join again
            JoinNetwork( );
        }
        else
        {
            DeviceState = WakeUpState;
            NextTx = true;
        }
    }
}

/*!
 * \brief Function executed on Led 4 Timeout event
 */
static void OnLed4TimerEvent( void* context )
{
    TimerStop( &Led4Timer );
    // Switch LED 4 OFF
    GpioWrite( &Led4, 0 );
}

/*!
 * \brief Function executed on Led 2 Timeout event
 */
static void OnLed2TimerEvent( void* context )
{
    TimerStop( &Led2Timer );
    // Switch LED 2 OFF
    GpioWrite( &Led2, 0 );
}

/*!
 * \brief Function executed on Beacon timer Timeout event
 */
static void OnLedBeaconTimerEvent( void* context )
{
    GpioWrite( &Led2, 1 );
    TimerStart( &Led2Timer );

    TimerStart( &LedBeaconTimer );
}

/*!
 * \brief   MCPS-Confirm event function
 *
 * \param   [IN] mcpsConfirm - Pointer to the confirm structure,
 *               containing confirm attributes.
 */
static void McpsConfirm( McpsConfirm_t *mcpsConfirm )
{
    printf( "\n###### ===== MCPS-Confirm ==== ######\n" );
    printf( "STATUS      : %s\n", EventInfoStatusStrings[mcpsConfirm->Status] );
    if( mcpsConfirm->Status != LORAMAC_EVENT_INFO_STATUS_OK )
    {
    }
    else
    {
        switch( mcpsConfirm->McpsRequest )
        {
            case MCPS_UNCONFIRMED:
            {
                // Check Datarate
                // Check TxPower
                break;
            }
            case MCPS_CONFIRMED:
            {
                // Check Datarate
                // Check TxPower
                // Check AckReceived
                // Check NbTrials
                break;
            }
            case MCPS_PROPRIETARY:
            {
                break;
            }
            default:
                break;
        }

        // Switch LED 4 ON
        GpioWrite( &Led4, 1 );
        TimerStart( &Led4Timer );
    }
    MibRequestConfirm_t mibGet;
    MibRequestConfirm_t mibReq;

    mibReq.Type = MIB_DEVICE_CLASS;
    LoRaMacMibGetRequestConfirm( &mibReq );

    printf( "\n###### ===== UPLINK FRAME %lu ==== ######\n", mcpsConfirm->UpLinkCounter );
    printf( "\n" );

    printf( "CLASS       : %c\n", "ABC"[mibReq.Param.Class] );
    printf( "\n" );
    printf( "TX PORT     : %d\n", AppData.Port );

    if( AppData.BufferSize != 0 )
    {
        printf( "TX DATA     : " );
        if( AppData.MsgType == LORAMAC_HANDLER_CONFIRMED_MSG )
        {
            printf( "CONFIRMED - %s\n", ( mcpsConfirm->AckReceived != 0 ) ? "ACK" : "NACK" );
        }
        else
        {
            printf( "UNCONFIRMED\n" );
        }
        PrintHexBuffer( AppData.Buffer, AppData.BufferSize );
    }

    printf( "\n" );
    printf( "DATA RATE   : DR_%d\n", mcpsConfirm->Datarate );

    mibGet.Type  = MIB_CHANNELS;
    if( LoRaMacMibGetRequestConfirm( &mibGet ) == LORAMAC_STATUS_OK )
    {
        printf( "U/L FREQ    : %lu\n", mibGet.Param.ChannelList[mcpsConfirm->Channel].Frequency );
    }

    printf( "TX POWER    : %d\n", mcpsConfirm->TxPower );

    mibGet.Type  = MIB_CHANNELS_MASK;
    if( LoRaMacMibGetRequestConfirm( &mibGet ) == LORAMAC_STATUS_OK )
    {
        printf("CHANNEL MASK: ");
#if defined( REGION_AS923 ) || defined( REGION_CN779 ) || \
    defined( REGION_EU868 ) || defined( REGION_IN865 ) || \
    defined( REGION_KR920 ) || defined( REGION_RU864 )

        for( uint8_t i = 0; i < 1; i++)

#elif defined( REGION_AU915 ) || defined( REGION_US915 )

        for( uint8_t i = 0; i < 5; i++)
#else

#error "Please define a region in the compiler options."

#endif
        {
            printf("%04X ", mibGet.Param.ChannelsMask[i] );
        }
        printf("\n");
    }

    printf( "\n" );
}

/*!
 * \brief   MCPS-Indication event function
 *
 * \param   [IN] mcpsIndication - Pointer to the indication structure,
 *               containing indication attributes.
 */
static void McpsIndication( McpsIndication_t *mcpsIndication )
{
    printf( "\n###### ===== MCPS-Indication ==== ######\n" );
    printf( "STATUS      : %s\n", EventInfoStatusStrings[mcpsIndication->Status] );
    if( mcpsIndication->Status != LORAMAC_EVENT_INFO_STATUS_OK )
    {
        return;
    }

    switch( mcpsIndication->McpsIndication )
    {
        case MCPS_UNCONFIRMED:
        {
            break;
        }
        case MCPS_CONFIRMED:
        {
            break;
        }
        case MCPS_PROPRIETARY:
        {
            break;
        }
        case MCPS_MULTICAST:
        {
            break;
        }
        default:
            break;
    }

    // Check Multicast
    // Check Port
    // Check Datarate
    // Check FramePending
    if( mcpsIndication->FramePending == true )
    {
        // The server signals that it has pending data to be sent.
        // We schedule an uplink as soon as possible to flush the server.
        OnTxNextPacketTimerEvent( NULL );
    }
    // Check Buffer
    // Check BufferSize
    // Check Rssi
    // Check Snr
    // Check RxSlot

    if( ComplianceTest.Running == true )
    {
        ComplianceTest.DownLinkCounter++;
    }

    if( mcpsIndication->RxData == true )
    {
        switch( mcpsIndication->Port )
        {
        case 1: // The application LED can be controlled on port 1 or 2
        case 2:
            if( mcpsIndication->BufferSize == 1 )
            {
                AppLedStateOn = mcpsIndication->Buffer[0] & 0x01;
                GpioWrite( &Led3, ( ( AppLedStateOn & 0x01 ) != 0 ) ? 1 : 0 );
            }
            break;
        case 224:
            if( ComplianceTest.Running == false )
            {
                // Check compliance test enable command (i)
                if( ( mcpsIndication->BufferSize == 4 ) &&
                    ( mcpsIndication->Buffer[0] == 0x01 ) &&
                    ( mcpsIndication->Buffer[1] == 0x01 ) &&
                    ( mcpsIndication->Buffer[2] == 0x01 ) &&
                    ( mcpsIndication->Buffer[3] == 0x01 ) )
                {
                    IsTxConfirmed = false;
                    AppPort = 224;
                    AppDataSizeBackup = AppDataSize;
                    AppDataSize = 2;
                    ComplianceTest.DownLinkCounter = 0;
                    ComplianceTest.LinkCheck = false;
                    ComplianceTest.DemodMargin = 0;
                    ComplianceTest.NbGateways = 0;
                    ComplianceTest.Running = true;
                    ComplianceTest.State = 1;

                    MibRequestConfirm_t mibReq;
                    mibReq.Type = MIB_ADR;
                    mibReq.Param.AdrEnable = true;
                    LoRaMacMibSetRequestConfirm( &mibReq );

#if defined( REGION_EU868 ) || defined( REGION_RU864 ) || defined( REGION_CN779 ) || defined( REGION_EU433 )
                    LoRaMacTestSetDutyCycleOn( false );
#endif
                }
            }
            else
            {
                ComplianceTest.State = mcpsIndication->Buffer[0];
                switch( ComplianceTest.State )
                {
                case 0: // Check compliance test disable command (ii)
                    IsTxConfirmed = LORAWAN_CONFIRMED_MSG_ON;
                    AppPort = LORAWAN_APP_PORT;
                    AppDataSize = AppDataSizeBackup;
                    ComplianceTest.DownLinkCounter = 0;
                    ComplianceTest.Running = false;

                    MibRequestConfirm_t mibReq;
                    mibReq.Type = MIB_ADR;
                    mibReq.Param.AdrEnable = LORAWAN_ADR_ON;
                    LoRaMacMibSetRequestConfirm( &mibReq );
#if defined( REGION_EU868 ) || defined( REGION_RU864 ) || defined( REGION_CN779 ) || defined( REGION_EU433 )
                    LoRaMacTestSetDutyCycleOn( LORAWAN_DUTYCYCLE_ON );
#endif
                    break;
                case 1: // (iii, iv)
                    AppDataSize = 2;
                    break;
                case 2: // Enable confirmed messages (v)
                    IsTxConfirmed = true;
                    ComplianceTest.State = 1;
                    break;
                case 3:  // Disable confirmed messages (vi)
                    IsTxConfirmed = false;
                    ComplianceTest.State = 1;
                    break;
                case 4: // (vii)
                    AppDataSize = mcpsIndication->BufferSize;

                    AppDataBuffer[0] = 4;
                    for( uint8_t i = 1; i < MIN( AppDataSize, LORAWAN_APP_DATA_MAX_SIZE ); i++ )
                    {
                        AppDataBuffer[i] = mcpsIndication->Buffer[i] + 1;
                    }
                    break;
                case 5: // (viii)
                    {
                        MlmeReq_t mlmeReq;
                        mlmeReq.Type = MLME_LINK_CHECK;
                        LoRaMacStatus_t status = LoRaMacMlmeRequest( &mlmeReq );
                        printf( "\n###### ===== MLME-Request - MLME_LINK_CHECK ==== ######\n" );
                        printf( "STATUS      : %s\n", MacStatusStrings[status] );
                    }
                    break;
                case 6: // (ix)
                    {
                        // Disable TestMode and revert back to normal operation
                        IsTxConfirmed = LORAWAN_CONFIRMED_MSG_ON;
                        AppPort = LORAWAN_APP_PORT;
                        AppDataSize = AppDataSizeBackup;
                        ComplianceTest.DownLinkCounter = 0;
                        ComplianceTest.Running = false;

                        MibRequestConfirm_t mibReq;
                        mibReq.Type = MIB_ADR;
                        mibReq.Param.AdrEnable = LORAWAN_ADR_ON;
                        LoRaMacMibSetRequestConfirm( &mibReq );
#if defined( REGION_EU868 ) || defined( REGION_RU864 ) || defined( REGION_CN779 ) || defined( REGION_EU433 )
                        LoRaMacTestSetDutyCycleOn( LORAWAN_DUTYCYCLE_ON );
#endif

                        JoinNetwork( );
                    }
                    break;
                case 7: // (x)
                    {
                        if( mcpsIndication->BufferSize == 3 )
                        {
                            MlmeReq_t mlmeReq;
                            mlmeReq.Type = MLME_TXCW;
                            mlmeReq.Req.TxCw.Timeout = ( uint16_t )( ( mcpsIndication->Buffer[1] << 8 ) | mcpsIndication->Buffer[2] );
                            LoRaMacStatus_t status = LoRaMacMlmeRequest( &mlmeReq );
                            printf( "\n###### ===== MLME-Request - MLME_TXCW ==== ######\n" );
                            printf( "STATUS      : %s\n", MacStatusStrings[status] );
                        }
                        else if( mcpsIndication->BufferSize == 7 )
                        {
                            MlmeReq_t mlmeReq;
                            mlmeReq.Type = MLME_TXCW_1;
                            mlmeReq.Req.TxCw.Timeout = ( uint16_t )( ( mcpsIndication->Buffer[1] << 8 ) | mcpsIndication->Buffer[2] );
                            mlmeReq.Req.TxCw.Frequency = ( uint32_t )( ( mcpsIndication->Buffer[3] << 16 ) | ( mcpsIndication->Buffer[4] << 8 ) | mcpsIndication->Buffer[5] ) * 100;
                            mlmeReq.Req.TxCw.Power = mcpsIndication->Buffer[6];
                            LoRaMacStatus_t status = LoRaMacMlmeRequest( &mlmeReq );
                            printf( "\n###### ===== MLME-Request - MLME_TXCW1 ==== ######\n" );
                            printf( "STATUS      : %s\n", MacStatusStrings[status] );
                        }
                        ComplianceTest.State = 1;
                    }
                    break;
                case 8: // Send DeviceTimeReq
                    {
                        MlmeReq_t mlmeReq;

                        mlmeReq.Type = MLME_DEVICE_TIME;

                        LoRaMacMlmeRequest( &mlmeReq );
                        WakeUpState = DEVICE_STATE_SEND;
                        DeviceState = DEVICE_STATE_SEND;
                    }
                    break;
                case 9: // Switch end device Class
                    {
                        MibRequestConfirm_t mibReq;

                        mibReq.Type = MIB_DEVICE_CLASS;
                        // CLASS_A = 0, CLASS_B = 1, CLASS_C = 2
                        mibReq.Param.Class = ( DeviceClass_t )mcpsIndication->Buffer[1];;
                        LoRaMacMibSetRequestConfirm( &mibReq );

                        DeviceState = DEVICE_STATE_SEND;
                    }
                    break;
                case 10: // Send PingSlotInfoReq
                    {
                        MlmeReq_t mlmeReq;

                        mlmeReq.Type = MLME_PING_SLOT_INFO;

                        mlmeReq.Req.PingSlotInfo.PingSlot.Value = mcpsIndication->Buffer[1];

                        LoRaMacMlmeRequest( &mlmeReq );
                        WakeUpState = DEVICE_STATE_SEND;
                        DeviceState = DEVICE_STATE_SEND;
                    }
                    break;
                case 11: // Send BeaconTimingReq
                    {
                        MlmeReq_t mlmeReq;

                        mlmeReq.Type = MLME_BEACON_TIMING;

                        LoRaMacMlmeRequest( &mlmeReq );
                        WakeUpState = DEVICE_STATE_SEND;
                        DeviceState = DEVICE_STATE_SEND;
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

    // Switch LED 2 ON for each received downlink
    GpioWrite( &Led2, 1 );
    TimerStart( &Led2Timer );

    const char *slotStrings[] = { "1", "2", "C", "C Multicast", "B Ping-Slot", "B Multicast Ping-Slot" };

    printf( "\n###### ===== DOWNLINK FRAME %lu ==== ######\n", mcpsIndication->DownLinkCounter );

    printf( "RX WINDOW   : %s\n", slotStrings[mcpsIndication->RxSlot] );
    
    printf( "RX PORT     : %d\n", mcpsIndication->Port );

    if( mcpsIndication->BufferSize != 0 )
    {
        printf( "RX DATA     : \n" );
        PrintHexBuffer( mcpsIndication->Buffer, mcpsIndication->BufferSize );
    }

    printf( "\n" );
    printf( "DATA RATE   : DR_%d\n", mcpsIndication->RxDatarate );
    printf( "RX RSSI     : %d\n", mcpsIndication->Rssi );
    printf( "RX SNR      : %d\n", mcpsIndication->Snr );

    printf( "\n" );
}

/*!
 * \brief   MLME-Confirm event function
 *
 * \param   [IN] mlmeConfirm - Pointer to the confirm structure,
 *               containing confirm attributes.
 */
static void MlmeConfirm( MlmeConfirm_t *mlmeConfirm )
{
    MibRequestConfirm_t mibReq;

    printf( "\n###### ===== MLME-Confirm ==== ######\n" );
    printf( "STATUS      : %s\n", EventInfoStatusStrings[mlmeConfirm->Status] );
    if( mlmeConfirm->Status != LORAMAC_EVENT_INFO_STATUS_OK )
    {
    }
    switch( mlmeConfirm->MlmeRequest )
    {
        case MLME_JOIN:
        {
            if( mlmeConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
            {
                MibRequestConfirm_t mibGet;
                printf( "###### ===== JOINED ==== ######\n" );
                printf( "\nOTAA\n\n" );

                mibGet.Type = MIB_DEV_ADDR;
                LoRaMacMibGetRequestConfirm( &mibGet );
                printf( "DevAddr     : %08lX\n", mibGet.Param.DevAddr );

                printf( "\n\n" );
                mibGet.Type = MIB_CHANNELS_DATARATE;
                LoRaMacMibGetRequestConfirm( &mibGet );
                printf( "DATA RATE   : DR_%d\n", mibGet.Param.ChannelsDatarate );
                printf( "\n" );
                // Status is OK, node has joined the network
#if defined( USE_BEACON_TIMING )
                DeviceState = DEVICE_STATE_REQ_BEACON_TIMING;
#else
                DeviceState = DEVICE_STATE_REQ_DEVICE_TIME;
#endif
            }
            else
            {
                // Join was not successful. Try to join again
                JoinNetwork( );
            }
            break;
        }
        case MLME_LINK_CHECK:
        {
            if( mlmeConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
            {
                // Check DemodMargin
                // Check NbGateways
                if( ComplianceTest.Running == true )
                {
                    ComplianceTest.LinkCheck = true;
                    ComplianceTest.DemodMargin = mlmeConfirm->DemodMargin;
                    ComplianceTest.NbGateways = mlmeConfirm->NbGateways;
                }
            }
            break;
        }
        case MLME_DEVICE_TIME:
        {
            // Setup the WakeUpState to DEVICE_STATE_SEND. This allows the
            // application to initiate MCPS requests during a beacon acquisition
            WakeUpState = DEVICE_STATE_SEND;
            // Switch to the next state immediately
            DeviceState = DEVICE_STATE_BEACON_ACQUISITION;
            NextTx = true;
            break;
        }
        case MLME_BEACON_TIMING:
        {
            // Setup the WakeUpState to DEVICE_STATE_SEND. This allows the
            // application to initiate MCPS requests during a beacon acquisition
            WakeUpState = DEVICE_STATE_SEND;
            // Switch to the next state immediately
            DeviceState = DEVICE_STATE_BEACON_ACQUISITION;
            NextTx = true;
            break;
        }
        case MLME_BEACON_ACQUISITION:
        {
            if( mlmeConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
            {
                WakeUpState = DEVICE_STATE_REQ_PINGSLOT_ACK;
            }
            else
            {
#if defined( USE_BEACON_TIMING )
                WakeUpState = DEVICE_STATE_REQ_BEACON_TIMING;
#else
                WakeUpState = DEVICE_STATE_REQ_DEVICE_TIME;
#endif
            }
            break;
        }
        case MLME_PING_SLOT_INFO:
        {
            if( mlmeConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
            {
                mibReq.Type = MIB_DEVICE_CLASS;
                mibReq.Param.Class = CLASS_B;
                LoRaMacMibSetRequestConfirm( &mibReq );

                printf( "\n\n###### ===== Switch to Class B done. ==== ######\n\n" );

                WakeUpState = DEVICE_STATE_SEND;
                DeviceState = WakeUpState;
                NextTx = true;
            }
            else
            {
                WakeUpState = DEVICE_STATE_REQ_PINGSLOT_ACK;
            }
            break;
        }
        default:
            break;
    }
}

/*!
 * \brief   MLME-Indication event function
 *
 * \param   [IN] mlmeIndication - Pointer to the indication structure.
 */
static void MlmeIndication( MlmeIndication_t *mlmeIndication )
{
    MibRequestConfirm_t mibReq;

    if( mlmeIndication->Status != LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED )
    {
        printf( "\n###### ===== MLME-Indication ==== ######\n" );
        printf( "STATUS      : %s\n", EventInfoStatusStrings[mlmeIndication->Status] );
    }
    if( mlmeIndication->Status != LORAMAC_EVENT_INFO_STATUS_OK )
    {
    }
    switch( mlmeIndication->MlmeIndication )
    {
        case MLME_SCHEDULE_UPLINK:
        {// The MAC signals that we shall provide an uplink as soon as possible
            OnTxNextPacketTimerEvent( NULL );
            break;
        }
        case MLME_BEACON_LOST:
        {
            mibReq.Type = MIB_DEVICE_CLASS;
            mibReq.Param.Class = CLASS_A;
            LoRaMacMibSetRequestConfirm( &mibReq );

            printf( "\n\n###### ===== Switch to Class A done. ==== ######\n\n" );

            // Switch to class A again
#if defined( USE_BEACON_TIMING )
            WakeUpState = DEVICE_STATE_REQ_BEACON_TIMING;
#else
            WakeUpState = DEVICE_STATE_REQ_DEVICE_TIME;
#endif
            TimerStop( &LedBeaconTimer );
            printf( "\n###### ===== BEACON LOST ==== ######\n" );
            break;
        }
        case MLME_BEACON:
        {
            if( mlmeIndication->Status == LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED )
            {
                TimerStart( &LedBeaconTimer );
                printf( "\n###### ===== BEACON %lu ==== ######\n", mlmeIndication->BeaconInfo.Time.Seconds );
                printf( "GW DESC     : %d\n", mlmeIndication->BeaconInfo.GwSpecific.InfoDesc );
                printf( "GW INFO     : " );
                PrintHexBuffer( mlmeIndication->BeaconInfo.GwSpecific.Info, 6 );
                printf( "\n" );
                printf( "FREQ        : %lu\n", mlmeIndication->BeaconInfo.Frequency );
                printf( "DATA RATE   : DR_%d\n", mlmeIndication->BeaconInfo.Datarate );
                printf( "RX RSSI     : %d\n", mlmeIndication->BeaconInfo.Rssi );
                printf( "RX SNR      : %d\n", mlmeIndication->BeaconInfo.Snr );
                printf( "\n" );
            }
            else
            {
                TimerStop( &LedBeaconTimer );
                printf( "\n###### ===== BEACON NOT RECEIVED ==== ######\n" );
            }
            break;
        }
        default:
            break;
    }
}

void OnMacProcessNotify( void )
{
    IsMacProcessPending = 1;
}

/**
 * Main application entry point.
 */
int main( void )
{
    LoRaMacPrimitives_t macPrimitives;
    LoRaMacCallback_t macCallbacks;
    MibRequestConfirm_t mibReq;
    LoRaMacStatus_t status;
    uint8_t devEui[8] = { 0 };  // Automatically filed from secure-element
    uint8_t joinEui[8] = { 0 }; // Automatically filed from secure-element
    uint8_t sePin[4] = { 0 };   // Automatically filed from secure-element

    BoardInitMcu( );
    BoardInitPeriph( );

    macPrimitives.MacMcpsConfirm = McpsConfirm;
    macPrimitives.MacMcpsIndication = McpsIndication;
    macPrimitives.MacMlmeConfirm = MlmeConfirm;
    macPrimitives.MacMlmeIndication = MlmeIndication;
    macCallbacks.GetBatteryLevel = BoardGetBatteryLevel;
    macCallbacks.GetTemperatureLevel = NULL;
    macCallbacks.NvmContextChange = NvmCtxMgmtEvent;
    macCallbacks.MacProcessNotify = OnMacProcessNotify;

    status = LoRaMacInitialization( &macPrimitives, &macCallbacks, ACTIVE_REGION );
    if ( status != LORAMAC_STATUS_OK )
    {
        printf( "LoRaMac wasn't properly initialized, error: %s", MacStatusStrings[status] );
        // Fatal error, endless loop.
        while ( 1 )
        {
        }
    }

#if ( LOCAL_MULTICAST_SETUP_ENABLED == 1 )
    // Initialize local Multicast Channel

    // Multicast session keys
    uint8_t localMcAppSKey[] = LORAWAN_APP_S_KEY;
    uint8_t localMcNwkSKey[] = LORAWAN_NWK_S_ENC_KEY;

    /*!
     * Multicast address
     *
     * The multicast address should be different than the device address as it should
     * not use this address for transmission. But this address should be registered in the end device
     * to receive a downlink multicast message from the network.
     */
    uint32_t localMcAddress = 0x01020304;

    //                               AS923,     AU915,     CN470,     CN779,     EU433,     EU868,     KR920,     IN865,     US915,     RU864
#if( LOCAL_MULTICAST_SETUP_DISABLE_SLOT_HOP == 1 )
    const uint32_t frequencies[] = { 923200000, 923300000, 505300000, 786000000, 434665000, 869525000, 921900000, 866550000, 923300000, 869100000 };
#else
    const uint32_t frequencies[] = { 923200000, 0        , 0,         786000000, 434665000, 869525000, 921900000, 866550000, 0,         69100000 };
#endif
    const int8_t dataRates[]     = { DR_2,      DR_2,      DR_0,      DR_0,      DR_0,      DR_0,      DR_0,      DR_0,      DR_0,      DR_0 };

    McChannelParams_t channel =
    {
        .IsRemotelySetup = false,
        .Class = CLASS_B,
        .IsEnabled = true,
        .GroupID = MULTICAST_0_ADDR,
        .Address = localMcAddress,
        .McKeys =
        {
            .Session =
            {
                .McAppSKey = localMcAppSKey,
                .McNwkSKey = localMcNwkSKey,
            },
        },
        .FCountMin = 0,
        .FCountMax = UINT32_MAX,
        .RxParams.ClassB =
        {
            .Frequency = frequencies[ACTIVE_REGION],
            .Datarate = dataRates[ACTIVE_REGION],
            .Periodicity = REGION_COMMON_DEFAULT_PING_SLOT_PERIODICITY,
        }
    };

    status = LoRaMacMcChannelSetup( &channel );

    if( status == LORAMAC_STATUS_OK )
    {
        uint8_t mcChannelSetupStatus = 0x00;
        if( LoRaMacMcChannelSetupRxParams( channel.GroupID, &channel.RxParams, &mcChannelSetupStatus ) == LORAMAC_STATUS_OK )
        {
            if( ( mcChannelSetupStatus & 0xFC ) == 0x00 )
            {
                printf("MC #%d setup, OK\n", ( mcChannelSetupStatus & 0x03 ) );
            }
            else
            {
                printf("MC #%d setup, ERROR - ", ( mcChannelSetupStatus & 0x03 ) );
                if( ( mcChannelSetupStatus & 0x10 ) == 0x10 )
                {
                    printf("MC group UNDEFINED - ");
                }
                else
                {
                    printf("MC group OK - ");
                }

                if( ( mcChannelSetupStatus & 0x08 ) == 0x08 )
                {
                    printf("MC Freq ERROR - ");
                }
                else
                {
                    printf("MC Freq OK - ");
                }
                if( ( mcChannelSetupStatus & 0x04 ) == 0x04 )
                {
                    printf("MC datarate ERROR\n");
                }
                else
                {
                    printf("MC datarate OK\n");
                }
            }
        }
        else
        {
            printf( "MC Rx params setup, error: %s \n", MacStatusStrings[status] );
        }
    }
    else
    {
        printf( "MC setup, error: %s \n", MacStatusStrings[status] );
    }
#endif

    DeviceState = DEVICE_STATE_RESTORE;
    WakeUpState = DEVICE_STATE_START;

    printf( "###### ===== ClassB demo application v1.0.0 ==== ######\n\n" );

    while( 1 )
    {
        // Process Radio IRQ
        if( Radio.IrqProcess != NULL )
        {
            Radio.IrqProcess( );
        }
        // Processes the LoRaMac events
        LoRaMacProcess( );

        switch( DeviceState )
        {
            case DEVICE_STATE_RESTORE:
            {
                // Try to restore from NVM and query the mac if possible.
                if( NvmCtxMgmtRestore( ) == NVMCTXMGMT_STATUS_SUCCESS )
                {
                    printf( "\n###### ===== CTXS RESTORED ==== ######\n\n" );
                }
                else
                {
                    // Read secure-element DEV_EUI, JOI_EUI and SE_PIN values.
                    mibReq.Type = MIB_DEV_EUI;
                    LoRaMacMibGetRequestConfirm( &mibReq );
                    memcpy1( devEui, mibReq.Param.DevEui, 8 );

                    mibReq.Type = MIB_JOIN_EUI;
                    LoRaMacMibGetRequestConfirm( &mibReq );
                    memcpy1( joinEui, mibReq.Param.JoinEui, 8 );

                    mibReq.Type = MIB_SE_PIN;
                    LoRaMacMibGetRequestConfirm( &mibReq );
                    memcpy1( sePin, mibReq.Param.SePin, 4 );
#if( OVER_THE_AIR_ACTIVATION == 0 )
                    // Tell the MAC layer which network server version are we connecting too.
                    mibReq.Type = MIB_ABP_LORAWAN_VERSION;
                    mibReq.Param.AbpLrWanVersion.Value = ABP_ACTIVATION_LRWAN_VERSION;
                    LoRaMacMibSetRequestConfirm( &mibReq );

                    mibReq.Type = MIB_NET_ID;
                    mibReq.Param.NetID = LORAWAN_NETWORK_ID;
                    LoRaMacMibSetRequestConfirm( &mibReq );

                    // Choose a random device address if not already defined in Commissioning.h
#if( STATIC_DEVICE_ADDRESS != 1 )
                    // Random seed initialization
                    srand1( BoardGetRandomSeed( ) );
                    // Choose a random device address
                    DevAddr = randr( 0, 0x01FFFFFF );
#endif

                    mibReq.Type = MIB_DEV_ADDR;
                    mibReq.Param.DevAddr = DevAddr;
                    LoRaMacMibSetRequestConfirm( &mibReq );
#endif // #if( OVER_THE_AIR_ACTIVATION == 0 )
                }
                DeviceState = DEVICE_STATE_START;
                break;
            }

            case DEVICE_STATE_START:
            {
                TimerInit( &TxNextPacketTimer, OnTxNextPacketTimerEvent );

                TimerInit( &Led4Timer, OnLed4TimerEvent );
                TimerSetValue( &Led4Timer, 25 );

                TimerInit( &Led2Timer, OnLed2TimerEvent );
                TimerSetValue( &Led2Timer, 25 );

                TimerInit( &LedBeaconTimer, OnLedBeaconTimerEvent );
                TimerSetValue( &LedBeaconTimer, 5000 );

                mibReq.Type = MIB_PUBLIC_NETWORK;
                mibReq.Param.EnablePublicNetwork = LORAWAN_PUBLIC_NETWORK;
                LoRaMacMibSetRequestConfirm( &mibReq );

                mibReq.Type = MIB_ADR;
                mibReq.Param.AdrEnable = LORAWAN_ADR_ON;
                LoRaMacMibSetRequestConfirm( &mibReq );

#if defined( REGION_EU868 ) || defined( REGION_RU864 ) || defined( REGION_CN779 ) || defined( REGION_EU433 )
                LoRaMacTestSetDutyCycleOn( LORAWAN_DUTYCYCLE_ON );
#endif
                mibReq.Type = MIB_SYSTEM_MAX_RX_ERROR;
                mibReq.Param.SystemMaxRxError = 20;
                LoRaMacMibSetRequestConfirm( &mibReq );

                LoRaMacStart( );

                mibReq.Type = MIB_NETWORK_ACTIVATION;
                status = LoRaMacMibGetRequestConfirm( &mibReq );

                if( status == LORAMAC_STATUS_OK )
                {
                    if( mibReq.Param.NetworkActivation == ACTIVATION_TYPE_NONE )
                    {
                        DeviceState = DEVICE_STATE_JOIN;
                    }
                    else
                    {
                        DeviceState = DEVICE_STATE_SEND;
                        NextTx = true;
                    }
                }
                break;
            }
            case DEVICE_STATE_JOIN:
            {
                mibReq.Type = MIB_DEV_EUI;
                LoRaMacMibGetRequestConfirm( &mibReq );
                printf( "DevEui      : %02X", mibReq.Param.DevEui[0] );
                for( int i = 1; i < 8; i++ )
                {
                    printf( "-%02X", mibReq.Param.DevEui[i] );
                }
                printf( "\n" );
                mibReq.Type = MIB_JOIN_EUI;
                LoRaMacMibGetRequestConfirm( &mibReq );
                printf( "JoinEui     : %02X", mibReq.Param.JoinEui[0] );
                for( int i = 1; i < 8; i++ )
                {
                    printf( "-%02X", mibReq.Param.JoinEui[i] );
                }
                printf( "\n" );
                mibReq.Type = MIB_SE_PIN;
                LoRaMacMibGetRequestConfirm( &mibReq );
                printf( "Pin         : %02X", mibReq.Param.SePin[0] );
                for( int i = 1; i < 4; i++ )
                {
                    printf( "-%02X", mibReq.Param.SePin[i] );
                }
                printf( "\n\n" );
#if( OVER_THE_AIR_ACTIVATION == 0 )
                printf( "###### ===== JOINED ==== ######\n" );
                printf( "\nABP\n\n" );
                printf( "DevAddr     : %08lX\n", DevAddr );
                printf( "\n\n" );

                mibReq.Type = MIB_NETWORK_ACTIVATION;
                mibReq.Param.NetworkActivation = ACTIVATION_TYPE_ABP;
                LoRaMacMibSetRequestConfirm( &mibReq );

#if defined( USE_BEACON_TIMING )
                DeviceState = DEVICE_STATE_REQ_BEACON_TIMING;
#else
                DeviceState = DEVICE_STATE_REQ_DEVICE_TIME;
#endif
#else
                JoinNetwork( );
#endif
                break;
            }
            case DEVICE_STATE_REQ_DEVICE_TIME:
            {
                MlmeReq_t mlmeReq;

                if( NextTx == true )
                {
                    mlmeReq.Type = MLME_DEVICE_TIME;

                    if( LoRaMacMlmeRequest( &mlmeReq ) == LORAMAC_STATUS_OK )
                    {
                        WakeUpState = DEVICE_STATE_SEND;
                    }
                }
                DeviceState = DEVICE_STATE_SEND;
                break;
            }
            case DEVICE_STATE_REQ_BEACON_TIMING:
            {
                MlmeReq_t mlmeReq;

                if( NextTx == true )
                {
                    mlmeReq.Type = MLME_BEACON_TIMING;

                    if( LoRaMacMlmeRequest( &mlmeReq ) == LORAMAC_STATUS_OK )
                    {
                        WakeUpState = DEVICE_STATE_SEND;
                    }
                }
                DeviceState = DEVICE_STATE_SEND;
                break;
            }
            case DEVICE_STATE_BEACON_ACQUISITION:
            {
                MlmeReq_t mlmeReq;

                if( NextTx == true )
                {
                    mlmeReq.Type = MLME_BEACON_ACQUISITION;

                    LoRaMacMlmeRequest( &mlmeReq );
                    NextTx = false;
                }
                DeviceState = DEVICE_STATE_SEND;
                break;
            }
            case DEVICE_STATE_REQ_PINGSLOT_ACK:
            {
                MlmeReq_t mlmeReq;

                if( NextTx == true )
                {
                    mlmeReq.Type = MLME_LINK_CHECK;
                    LoRaMacMlmeRequest( &mlmeReq );

                    mlmeReq.Type = MLME_PING_SLOT_INFO;
                    mlmeReq.Req.PingSlotInfo.PingSlot.Fields.Periodicity = LORAWAN_DEFAULT_PING_SLOT_PERIODICITY;
                    mlmeReq.Req.PingSlotInfo.PingSlot.Fields.RFU = 0;

                    if( LoRaMacMlmeRequest( &mlmeReq ) == LORAMAC_STATUS_OK )
                    {
                        WakeUpState = DEVICE_STATE_SEND;
                    }
                }
                DeviceState = DEVICE_STATE_SEND;
                break;
            }
            case DEVICE_STATE_SEND:
            {
                if( NextTx == true )
                {
                    PrepareTxFrame( AppPort );

                    NextTx = SendFrame( );
                }
                DeviceState = DEVICE_STATE_CYCLE;
                break;
            }
            case DEVICE_STATE_CYCLE:
            {
                DeviceState = DEVICE_STATE_SLEEP;
                if( ComplianceTest.Running == true )
                {
                    // Schedule next packet transmission
                    TxDutyCycleTime = 5000; // 5000 ms
                }
                else
                {
                    // Schedule next packet transmission
                    TxDutyCycleTime = APP_TX_DUTYCYCLE + randr( -APP_TX_DUTYCYCLE_RND, APP_TX_DUTYCYCLE_RND );
                }

                // Schedule next packet transmission
                TimerSetValue( &TxNextPacketTimer, TxDutyCycleTime );
                TimerStart( &TxNextPacketTimer );
                break;
            }
            case DEVICE_STATE_SLEEP:
            {
                if( NvmCtxMgmtStore( ) == NVMCTXMGMT_STATUS_SUCCESS )
                {
                    printf( "\n###### ===== CTXS STORED ==== ######\n" );
                }

                CRITICAL_SECTION_BEGIN( );
                if( IsMacProcessPending == 1 )
                {
                    // Clear flag and prevent MCU to go into low power modes.
                    IsMacProcessPending = 0;
                }
                else
                {
                    // The MCU wakes up through events
                    BoardLowPowerHandler( );
                }
                CRITICAL_SECTION_END( );
                break;
            }
            default:
            {
                DeviceState = DEVICE_STATE_START;
                break;
            }
        }
    }
}