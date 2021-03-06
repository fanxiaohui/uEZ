/*-------------------------------------------------------------------------*
 * File:  I2C.c
 *-------------------------------------------------------------------------*
 * Description:
 *      HAL implementation of the LPC1768 I2C Interface.
 * Implementation:
 *      A single I2C0 is created, but the code can be easily changed for
 *      other processors to use multiple I2C busses.
 *-------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 * uEZ(R) - Copyright (C) 2007-2010 Future Designs, Inc.
 *--------------------------------------------------------------------------
 * This file is part of the uEZ(R) distribution.  See the included
 * uEZLicense.txt or visit http://www.teamfdi.com/uez for details.
 *
 *    *===============================================================*
 *    |  Future Designs, Inc. can port uEZ(tm) to your own hardware!  |
 *    |             We can get you up and running fast!               |
 *    |      See http://www.teamfdi.com/uez for more details.         |
 *    *===============================================================*
 *
 *-------------------------------------------------------------------------*/
#include <uEZ.h>
#include <HAL/Interrupt.h>
#include <uEZProcessor.h>
#include <Source/Processor/NXP/LPC1768/LPC1768_I2C.h>
#include <uEZPlatformAPI.h>

// Setup Master mode only.  Slave mode is a future version but we'll
// leave some of the code in so we don't have to write it later.
#define COMPILE_I2C_MASTER_MODE 1
#define COMPILE_I2C_SLAVE_MODE  0

/*---------------------------------------------------------------------------*
 * Types:
 *---------------------------------------------------------------------------*/
// I2CONSET:
    // Assert acknowledge flag
    #define I2CONSET_AA     (1<<2)
    // I2C interrupt flag
    #define I2CONSET_SI     (1<<3)
    // STOP flag
    #define I2CONSET_STO    (1<<4)
    // START flag
    #define I2CONSET_STA    (1<<5)
    // I2C interface enable
    #define I2CONSET_I2EN   (1<<6)
// I2STAT:
    // Status
    #define I2STAT_STATUS_MASK  (0x1F<<3)
// I2DAT:
// I2ADR:
    #define I2ADR_GC        (1<<0)
// I2SCLH:
// I2SCLL:
// I2CONCLR:
    // Assert acknowledge Clear bit
    #define I2CONCLR_AAC    (1<<2)
    // I2C interrupt Clear bit
    #define I2CONCLR_SIC    (1<<3)
    // START flag Clear bit
    #define I2CONCLR_STAC   (1<<5)
    // I2C interface Disable bit
    #define I2CONCLR_I2ENC  (1<<6)

typedef struct {
    const HAL_I2CBus *iHAL;
    I2C_TypeDef *iRegs;
    I2C_Request *iRequest;
    TUInt8 iAddress;
    TUInt8 *iData;
    TUInt8 iDataLen;
    TUInt8 iIndex;
    I2CRequestCompleteCallback iCompleteFunc;
    void *iCompleteWorkspace;
    T_uezGPIOPortPin iSDAPin;
    T_uezGPIOPortPin iSCLPin;

    volatile TBool iDoneFlag;
#if COMPILE_I2C_SLAVE_MODE
    I2CSlaveIsLastReceiveByte iI2CSlaveIsLastReceiveByte;
    I2CSlaveIsLastTransmitByte iI2CSlaveIsLastTransmitByte;
    I2CTransferComplete iI2CTransferComplete;
    I2CSlaveReceiveByte iI2CSlaveReceiveByte;
    I2CSlaveGetTransmitByte iI2CSlaveGetTransmitByte;
#endif
} T_LPC1768_I2C_Workspace;

typedef TUInt8 T_lpc1768_i2cMode;
#define I2C_MODE_READ           1
#define I2C_MODE_WRITE          0

/*-------------------------------------------------------------------------*
 * Macros:
 *-------------------------------------------------------------------------*/
#define I2C_ENABLE(p)       (((p)->iRegs)->I2CONSET) = I2CONSET_I2EN
#define I2C_DISABLE(p)      (((p)->iRegs)->I2CONCLR) = I2CONCLR_I2ENC
#define STA_SET(p)          (((p)->iRegs)->I2CONSET) = I2CONSET_STA
#define STA_CLEAR(p)        (((p)->iRegs)->I2CONCLR) = I2CONCLR_STAC
#define STO_SET(p)          (((p)->iRegs)->I2CONSET) = I2CONSET_STO
#define STO_CLEAR(p)        /* automatically cleared by hardware */
#define AA_SET(p)           (((p)->iRegs)->I2CONSET) = I2CONSET_AA
#define AA_CLEAR(p)         (((p)->iRegs)->I2CONCLR) = I2CONCLR_AAC
#define SI_CLEAR(p)         (((p)->iRegs)->I2CONCLR) = I2CONCLR_SIC
#define I2DAT_WRITE(p, v)   (((p)->iRegs)->I2DAT) = (v)
#define I2DAT_READ(p)       (((p)->iRegs)->I2DAT)

/*-------------------------------------------------------------------------*
 * Globals:
 *-------------------------------------------------------------------------*/
static T_LPC1768_I2C_Workspace *G_lpc1768_i2c0Workspace;
static T_LPC1768_I2C_Workspace *G_lpc1768_i2c1Workspace;
static T_LPC1768_I2C_Workspace *G_lpc1768_i2c2Workspace;

static const T_LPC1768_IOCON_ConfigList G_sda1[] = {
    {GPIO_P0_0,     IOCON_I_DEFAULT(3) },
    {GPIO_P0_19,    IOCON_I_DEFAULT(3) },
};
static const T_LPC1768_IOCON_ConfigList G_scl1[] = {
    {GPIO_P0_1,     IOCON_I_DEFAULT(3) },
    {GPIO_P0_20,    IOCON_I_DEFAULT(3) },
};

static const T_LPC1768_IOCON_ConfigList G_sda2[] = {
    {GPIO_P0_10,    IOCON_I_DEFAULT(2) },
};
static const T_LPC1768_IOCON_ConfigList G_scl2[] = {
    {GPIO_P0_11,    IOCON_I_DEFAULT(2) },
};

/*-------------------------------------------------------------------------*
 * Function Prototypes:
 *-------------------------------------------------------------------------*/
IRQ_ROUTINE(ILPC1768_I2C0InterruptHandler);
IRQ_ROUTINE(ILPC1768_I2C1InterruptHandler);
IRQ_ROUTINE(ILPC1768_I2C2InterruptHandler);
void ILPC1768_ProcessState(T_LPC1768_I2C_Workspace *p);

/*-------------------------------------------------------------------------*/
#if 0
void I2CTest(T_LPC1768_I2C_Workspace *p)
{
    unsigned int addr;
    unsigned int key;
    volatile unsigned int I2STAT;

    // Set clock rate to 100 kHz (based on 208 MHz)
    p->I2SCLH = (520*((100*Fpclk)/208))/I2C_SPEED;
    p->I2SCLL = (520*((100*Fpclk)/208))/I2C_SPEED;
    p->I2CONSET = 0x1;

    // Look through all the addresses
    for (addr=1; addr<127; addr++)  {
//        I2CWrite(p, (addr<<1)|I2C_START);
        p->I2CONSET |= 0x01;
        I2CStart(p, addr, I2C_MODE_READ);
        if (I2CWait(p))
            ConsolePrintf("\nDevice @ addr 0x%02X", addr<<1);

    }
}
#endif

#if COMPILE_I2C_SLAVE_MODE
static TBool ILPC1768_I2CSlaveIsLastReceiveByte(T_LPC1768_I2C_Workspace *aWorkspace)
{
    if (aWorkspace->iIndex >= aWorkspace->iDataLen)
        return ETrue;
    return EFalse;
}

static TBool ILPC1768_I2CSlaveIsLastTransmitByte(T_LPC1768_I2C_Workspace *aWorkspace)
{
    if (aWorkspace->iIndex >= aWorkspace->iDataLen)
        return ETrue;
    return EFalse;
}

static void ILPC1768_I2CTransferComplete(T_LPC1768_I2C_Workspace *aWorkspace)
{
    // Do nothing
//    PARAM_NOT_USED(aWorkspace);
}

static void ILPC1768_I2CSlaveReceiveByte(T_LPC1768_I2C_Workspace *aWorkspace, TUInt8 aByte)
{
    // Only store if there is room
    if (aWorkspace->iIndex < aWorkspace->iDataLen)
        aWorkspace->iData[aWorkspace->iIndex] = aByte;
}

static TUInt8 ILPC1768_I2CSlaveGetTransmitByte(T_LPC1768_I2C_Workspace *aWorkspace)
{
    // Only read from memory if in bounds
    if (aWorkspace->iIndex < aWorkspace->iDataLen)
        return aWorkspace->iData[aWorkspace->iIndex];
    return 0;
}
#endif

void ILPC1768_I2CSetSpeed(I2C_TypeDef *aRegs, TUInt16 aSpeed)
{
    // Calculate the speed value to use
    TUInt16 v;

    // Calculate from kHz to cycles (based on PCLK)
    v = (PROCESSOR_OSCILLATOR_FREQUENCY/1000) / aSpeed;

    // Set 50% duty cycle
    aRegs->I2SCLL = v/2;
    aRegs->I2SCLH = v/2;
}

void ILPC1768_I2CInit(T_LPC1768_I2C_Workspace *p)
{
    // Clear all the flags
    p->iRegs->I2CONCLR = I2CONSET_AA|I2CONSET_SI|I2CONSET_STA|I2CONSET_I2EN;

    // Program the speed
    ILPC1768_I2CSetSpeed(p->iRegs, p->iRequest->iSpeed);

    // Enable the I2C
    I2C_ENABLE(p);
}

static void ILPC1768_I2C_StartWrite(
        void *aWorkspace,
        I2C_Request *iRequest,
        void *aCallbackWorkspace,
        I2CRequestCompleteCallback aCallbackFunc)
{
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;

    // Use an address (and make sure the lowest bit is zero)
    p->iRequest = iRequest;
    p->iCompleteFunc = aCallbackFunc;
    p->iCompleteWorkspace = aCallbackWorkspace;
    p->iAddress = (p->iRequest->iAddr<<1)|I2C_MODE_WRITE;
    p->iData = (TUInt8 *)p->iRequest->iWriteData;
    p->iDataLen = p->iRequest->iWriteLength;
    p->iIndex = 0;
    p->iDoneFlag = EFalse;
    p->iRequest->iStatus = I2C_BUSY;

    // Initialize the I2C
    ILPC1768_I2CInit(p);

    // Kick off the transfer with a start bit
    STA_SET(p);
}

static void ILPC1768_I2C_StartRead(
        void *aWorkspace,
        I2C_Request *iRequest,
        void *aCallbackWorkspace,
        I2CRequestCompleteCallback aCallbackFunc)
{
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;

    // Use an address (and make sure the lowest bit is zero)
    p->iRequest = iRequest;
    p->iCompleteFunc = aCallbackFunc;
    p->iCompleteWorkspace = aCallbackWorkspace;
    p->iAddress = (p->iRequest->iAddr<<1)|I2C_MODE_READ;
    p->iData = p->iRequest->iReadData;
    p->iDataLen = p->iRequest->iReadLength;
    p->iIndex = 0;
    p->iDoneFlag = EFalse;
    p->iRequest->iStatus = I2C_BUSY;

    // Initialize the I2C
    ILPC1768_I2CInit(p);

    // Kick off the transfer with a start bit
    STA_SET(p);
}

/*-------------------------------------------------------------------------*
 * Routine:  ILPC1768_ProcessState
 *-------------------------------------------------------------------------*
 * Description:
 *      Core I2C processing (typically done in interrupt routine)
 *-------------------------------------------------------------------------*/
void ILPC1768_ProcessState(T_LPC1768_I2C_Workspace *p)
{
    TUInt8 c;
    TUInt8 status = p->iRegs->I2STAT;

//SerialSendChar(SERIAL_PORT_UART0, '$');
//SerialSendHex(SERIAL_PORT_UART0, status);
//ConsolePrintf("$%02X", status);
    switch(status)
    {
#if COMPILE_I2C_MASTER_MODE
        // Start condition tranmitted
        case 0x08:

        // Repeat start condition transmitted
        case 0x10:
            // Send the slave address
            I2DAT_WRITE(p, p->iAddress);
//            (((p)->iRegs)->I2DAT) = (p->iAddress);
            STA_CLEAR(p);
            STO_CLEAR(p);
            p->iIndex = 0;
            break;

        // MASTER TRANSMITTER
        // Slave Address + Write transmitted, and got ACK from slave
        case 0x18:
            // Transmit a byte
            I2DAT_WRITE(p, p->iData[p->iIndex]);
            STA_CLEAR(p);
            STO_CLEAR(p);
            break;

        // Slave Address transmitted, no ACK received
        case 0x20:
            // Generate a stop condition and report error status
            STA_CLEAR(p);
            STO_SET(p);
            p->iRequest->iStatus = I2C_ERROR;
            p->iDoneFlag = ETrue;
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;

        // Data byte written and ACK received.
        case 0x28:
            // Last byte?
            p->iIndex++;
            if (p->iIndex >= p->iDataLen) {
                // Yes, last.  Generate a stop condition
                STA_CLEAR(p);
                STO_SET(p);
                p->iRequest->iStatus = I2C_OK;
                p->iDoneFlag = ETrue;
                p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            } else {
                // Send the next byte
                I2DAT_WRITE(p, p->iData[p->iIndex]);
                STA_CLEAR(p);
                STO_CLEAR(p);
            }
            break;

        // Data byte written but no ACK received
        case 0x30:
            // Generate a stop condition
            STA_CLEAR(p);
            STO_SET(p);
            p->iRequest->iStatus = I2C_ERROR;
            p->iDoneFlag = ETrue;
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;

        // Arbitration was lost
        case 0x38:
            // Generate a start condition and try again
            STA_SET(p);
            STO_CLEAR(p);
            break;

        // MASTER RECEIVER
        // Slave address + Read sent, ACK received
        case 0x40:
            // Are we to receive more?
            STA_CLEAR(p);
            STO_CLEAR(p);
            if ((p->iIndex+1) >= p->iDataLen)  {
                // No more needed.
                // return NACK for data byte
                AA_CLEAR(p);
            } else {
                // More bytes needed.
                // return ACK for data byte
                AA_SET(p);
            }
            break;

        // Slave address + Read sent, NO ACK received
        case 0x48:
            // Generate a stop condition
            STA_CLEAR(p);
            STO_SET(p);
            p->iRequest->iStatus = I2C_ERROR;
            p->iDoneFlag = ETrue;
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;

        // Data byte received with ACK
        case 0x50:
            // Store received byte
            c = I2DAT_READ(p);
            if (p->iIndex < p->iDataLen)
                p->iData[p->iIndex++] = c;

            // Are we to receive more?
            STA_CLEAR(p);
            STO_CLEAR(p);
            if ((p->iIndex+1) >= p->iDataLen) {
                // return NACK for next data byte
                AA_CLEAR(p);
            } else {
                // return ACK for next data byte
                AA_SET(p);
            }
            break;

        // Data byte received with NACK (last byte)
        case 0x58:
            // Store last byte
            c = I2DAT_READ(p);
            if (p->iIndex < p->iDataLen)
                p->iData[p->iIndex++] = c;

            // Generate a stop condition, but report OK
            STA_CLEAR(p);
            STO_SET(p);
            p->iRequest->iStatus = I2C_OK;
            p->iDoneFlag = ETrue;
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;
#endif // COMPILE_I2C_MASTER_MODE

#if COMPILE_I2C_SLAVE_MODE
        // SLAVE RECEIVER
        case 0x60:
        case 0x68:
        case 0x70:
        case 0x78:
            // Start of slave transaction
            // slave address + W received, or general call address received,
            // or lost arbitration, slave address + W received, or
            // lost arbitration, general call address received.
            STA_CLEAR(p);
            STO_CLEAR(p);
            p->iIndex = 0;
            p->iDoneFlag = EFalse;
            // Is this the last byte?
            if (p->iI2CSlaveIsLastReceiveByte(p))  {
                // Not allowed to send more bytes
                AA_CLEAR(p);
            } else {
                // Allowed to send more bytes
                AA_SET(p);
            }
            break;
        case 0x80:
        case 0x90:
            // Data received
            p->iI2CSlaveReceiveByte(p, I2DAT_READ(p));
            p->iIndex++;
            STA_CLEAR(p);
            STO_CLEAR(p);
            // Is this the last byte?
            if (p->iI2CSlaveIsLastReceiveByte(p))  {
                // Not allowed to send more bytes
                AA_CLEAR(p);
            } else {
                // Allowed to send more bytes
                AA_SET(p);
            }
            break;
        case 0x88:
        case 0x98:
            // Data received, NACK returned (signaling last byte)
            p->iI2CSlaveReceiveByte(p, I2DAT_READ(p));
            p->iIndex++;
            p->iRequest->iStatus = I2C_OK;
            STA_CLEAR(p);
            STO_CLEAR(p);
            AA_SET(p);
            p->iDoneFlag = ETrue;
            p->iI2CTransferComplete(p);
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;
        case 0xA0:
            // Stop condition received
            p->iRequest->iStatus = I2C_OK;
            STA_CLEAR(p);
            STO_CLEAR(p);
            AA_SET(p);
            p->iDoneFlag = ETrue;
            p->iI2CTransferComplete(p);
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;

        // SLAVE TRANSMITTER
        // ACK returned on Slave Address + R received; or
        // arbitration lost, slave address + R received, ACK returned; or
        // data byte transmitted, ACK received
        case 0xA8:
        case 0xB0:
            p->iIndex = 0;
        case 0xB8:
            c = p->iI2CSlaveGetTransmitByte(p);
            p->iIndex++;
            I2DAT_WRITE(p, c);
            STA_CLEAR(p);
            STO_CLEAR(p);
            if (p->iI2CSlaveIsLastTransmitByte(p))  {
                // No more data
                AA_CLEAR(p);
            } else {
                // more data bytes to transmit
                AA_SET(p);
            }
            break;
        case 0xC0:
        case 0xC8:
            // data byte transmitted, NACK received
            // last data byte transmitted, NACK received
            p->iRequest->iStatus = I2C_OK;
            STA_CLEAR(p);
            STO_CLEAR(p);
            AA_SET(p);
            p->iDoneFlag = ETrue;
            p->iI2CTransferComplete(p);
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;
#endif // #if COMPILE_I2C_SLAVE_MODE

        // Unhandled state
        default:
            // Generate a stop condition
            STA_CLEAR(p);
            STO_CLEAR(p);
            AA_SET(p);
            p->iRequest->iStatus = I2C_ERROR;
            p->iDoneFlag = ETrue;
#if COMPILE_I2C_SLAVE_MODE
            p->iI2CTransferComplete(p);
#endif
            p->iCompleteFunc(p->iCompleteWorkspace, p->iRequest);
            break;
    }

    // clear interrupt flag
    SI_CLEAR(p);
}

T_uezError ILPC1768_I2C_IsHung(void *aWorkspace, TBool *aBool)
{
    T_uezError error = UEZ_ERROR_NONE;
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;
    TBool sda, scl;

    sda = UEZGPIORead(p->iSDAPin);
    scl = UEZGPIORead(p->iSCLPin);

    *aBool = (!scl || !sda) ? ETrue : EFalse;

    return error;
}

T_uezError ILPC1768_I2C_ResetBus(void *aWorkspace)
{
    T_uezError error = UEZ_ERROR_NONE;
    //T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;
    //TODO: implement reset functionality.

    return error;
}

IRQ_ROUTINE(ILPC1768_I2C0InterruptHandler)
{
    IRQ_START();
    ILPC1768_ProcessState(G_lpc1768_i2c0Workspace);
    IRQ_END();
}

IRQ_ROUTINE(ILPC1768_I2C1InterruptHandler)
{
    IRQ_START();
    ILPC1768_ProcessState(G_lpc1768_i2c1Workspace);
    IRQ_END();
}

IRQ_ROUTINE(ILPC1768_I2C2InterruptHandler)
{
    IRQ_START();
    ILPC1768_ProcessState(G_lpc1768_i2c2Workspace);
    IRQ_END();
}

/*---------------------------------------------------------------------------*
 * Routine:  LPC1768_I2C_Bus0_InitializeWorkspace
 *---------------------------------------------------------------------------*
 * Description:
 *      Setup the LPC1768 I2C Bus0 workspace.
 * Inputs:
 *      void *aW                    -- Particular I2C workspace
 * Outputs:
 *      T_uezError                   -- Error code
 *---------------------------------------------------------------------------*/
T_uezError LPC1768_I2C_Bus0_InitializeWorkspace(void *aWorkspace)
{
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;
    p->iRegs = I2C0;
    p->iDoneFlag = ETrue;
    p->iCompleteFunc = 0;
    G_lpc1768_i2c0Workspace = p;

    // Setup interrupt vector
    InterruptRegister(
        I2C0_IRQn,
        ILPC1768_I2C0InterruptHandler,
        INTERRUPT_PRIORITY_NORMAL,
        "I2C0");
    InterruptEnable(I2C0_IRQn);

    return UEZ_ERROR_NONE;
}

/*---------------------------------------------------------------------------*
 * Routine:  LPC1768_I2C_Bus1_InitializeWorkspace
 *---------------------------------------------------------------------------*
 * Description:
 *      Setup the LPC1768 I2C Bus1 workspace.
 * Inputs:
 *      void *aW                    -- Particular I2C workspace
 * Outputs:
 *      T_uezError                   -- Error code
 *---------------------------------------------------------------------------*/
T_uezError LPC1768_I2C_Bus1_InitializeWorkspace(void *aWorkspace)
{
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;
    p->iRegs = I2C1;
    p->iDoneFlag = ETrue;
    p->iCompleteFunc = 0;
    G_lpc1768_i2c1Workspace = p;

    // Setup interrupt vector
    InterruptRegister(
        I2C1_IRQn,
        ILPC1768_I2C1InterruptHandler,
        INTERRUPT_PRIORITY_NORMAL,
        "I2C1");
    InterruptEnable(I2C1_IRQn);

    return UEZ_ERROR_NONE;
}

/*---------------------------------------------------------------------------*
 * Routine:  LPC1768_I2C_Bus2_InitializeWorkspace
 *---------------------------------------------------------------------------*
 * Description:
 *      Setup the LPC1768 I2C Bus2 workspace.
 * Inputs:
 *      void *aW                    -- Particular I2C workspace
 * Outputs:
 *      T_uezError                   -- Error code
 *---------------------------------------------------------------------------*/
T_uezError LPC1768_I2C_Bus2_InitializeWorkspace(void *aWorkspace)
{
    T_LPC1768_I2C_Workspace *p = (T_LPC1768_I2C_Workspace *)aWorkspace;
    p->iRegs = I2C2;
    p->iDoneFlag = ETrue;
    p->iCompleteFunc = 0;
    G_lpc1768_i2c2Workspace = p;

    // Setup interrupt vector
    InterruptRegister(
        I2C2_IRQn,
        ILPC1768_I2C2InterruptHandler,
        INTERRUPT_PRIORITY_NORMAL,
        "I2C2");
    InterruptEnable(I2C2_IRQn);

    return UEZ_ERROR_NONE;
}

/*---------------------------------------------------------------------------*
 * HAL Interface tables:
 *---------------------------------------------------------------------------*/
const HAL_I2CBus I2C_LPC1768_Bus0_Interface = {
	{
	    "LPC1768 I2C Bus0",
	    0x0100,
	    LPC1768_I2C_Bus0_InitializeWorkspace,
	    sizeof(T_LPC1768_I2C_Workspace),
    },

    ILPC1768_I2C_StartRead,
    ILPC1768_I2C_StartWrite,
};

const HAL_I2CBus I2C_LPC1768_Bus1_Interface = {
	{
	    "LPC1768 I2C Bus1",
	    0x0100,
	    LPC1768_I2C_Bus1_InitializeWorkspace,
	    sizeof(T_LPC1768_I2C_Workspace),
    },

    ILPC1768_I2C_StartRead,
    ILPC1768_I2C_StartWrite,
    0,0,0,
    ILPC1768_I2C_IsHung, ILPC1768_I2C_ResetBus
};

const HAL_I2CBus I2C_LPC1768_Bus2_Interface = {
	{
	    "LPC1768 I2C Bus2",
	    0x0100,
	    LPC1768_I2C_Bus2_InitializeWorkspace,
	    sizeof(T_LPC1768_I2C_Workspace),
   	},

    ILPC1768_I2C_StartRead,
    ILPC1768_I2C_StartWrite,
    0,0,0,
    ILPC1768_I2C_IsHung, ILPC1768_I2C_ResetBus
};


/*---------------------------------------------------------------------------*
 * Requirement routines:
 *---------------------------------------------------------------------------*/
void LPC1768_I2C0_Require(
        T_uezGPIOPortPin aPinSDA0,
        T_uezGPIOPortPin aPinSCL0)
{
    T_LPC1768_I2C_Workspace *p;

    static const T_LPC1768_IOCON_ConfigList sda0[] = {
            {GPIO_P0_27,   IOCON_I_DEFAULT(1) },
    };
    static const T_LPC1768_IOCON_ConfigList scl0[] = {
            {GPIO_P0_28,   IOCON_I_DEFAULT(1) },
    };
    HAL_DEVICE_REQUIRE_ONCE();
    // Register I2C0 Bus driver
    HALInterfaceRegister("I2C0", (T_halInterface *)&I2C_LPC1768_Bus0_Interface,
            0, (T_halWorkspace **)&p);
    LPC1768_IOCON_ConfigPin(aPinSDA0, sda0, ARRAY_COUNT(sda0));
    LPC1768_IOCON_ConfigPin(aPinSCL0, scl0, ARRAY_COUNT(scl0));

    p->iSCLPin = aPinSCL0;
    p->iSDAPin = aPinSDA0;
}

void LPC1768_I2C1_Require(
        T_uezGPIOPortPin aPinSDA1,
        T_uezGPIOPortPin aPinSCL1)
{
    T_LPC1768_I2C_Workspace *p;

    HAL_DEVICE_REQUIRE_ONCE();
    // Register I2C1 Bus driver
    HALInterfaceRegister("I2C1", (T_halInterface *)&I2C_LPC1768_Bus1_Interface,
            0, (T_halWorkspace **)&p);
    LPC1768_IOCON_ConfigPin(aPinSDA1, G_sda1, ARRAY_COUNT(G_sda1));
    LPC1768_IOCON_ConfigPin(aPinSCL1, G_scl1, ARRAY_COUNT(G_scl1));

    p->iSCLPin = aPinSCL1;
    p->iSDAPin = aPinSDA1;
}

void LPC1768_I2C2_Require(
        T_uezGPIOPortPin aPinSDA2,
        T_uezGPIOPortPin aPinSCL2)
{
    T_LPC1768_I2C_Workspace *p;

    HAL_DEVICE_REQUIRE_ONCE();
    // Register I2C0 Bus driver
    HALInterfaceRegister("I2C2", (T_halInterface *)&I2C_LPC1768_Bus2_Interface,
            0, (T_halWorkspace **)&p);

    LPC1768_IOCON_ConfigPin(aPinSDA2, G_sda2, ARRAY_COUNT(G_sda2));
    LPC1768_IOCON_ConfigPin(aPinSCL2, G_scl2, ARRAY_COUNT(G_scl2));

    p->iSCLPin = aPinSCL2;
    p->iSDAPin = aPinSDA2;
}


/*-------------------------------------------------------------------------*
 * End of File:  I2C.c
 *-------------------------------------------------------------------------*/
