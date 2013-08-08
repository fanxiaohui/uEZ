/*-------------------------------------------------------------------------*
 * File:  main.c
 *-------------------------------------------------------------------------*
 * Description:
 *
 * Implementation:
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
#include <string.h>
#include <stdio.h>
#include <uEZ.h>
#include <uEZLCD.h>
#include <uEZPlatform.h>
#include <Source/Library/GUI/FDI/SimpleUI/SimpleUI.h>
#include "NVSettings.h"
#include "Calibration.h"
#include "Hello_World.h"
#include "Audio.h"
#include <HAL/GPIO.h>

#if FREERTOS_PLUS_TRACE //LPC1788 only as of uEZ v2.04
#include <trcUser.h>
#endif

extern T_uezTask G_mainTask;
/*---------------------------------------------------------------------------*
 * Task:  Heartbeat
 *---------------------------------------------------------------------------*
 * Description:
 *      Blink heartbeat LED
 * Inputs:
 *      T_uezTask aMyTask            -- Handle to this task
 *      void *aParams               -- Parameters.  Not used.
 * Outputs:
 *      TUInt32                     -- Never returns.
 *---------------------------------------------------------------------------*/
TUInt32 Heartbeat(T_uezTask aMyTask, void *aParams)
{
    HAL_GPIOPort **p_gpio;
    TUInt8 heartbeatLED = 13;
    
    HALInterfaceFind("GPIO1", (T_halWorkspace **)&p_gpio);

    (*p_gpio)->SetOutputMode(p_gpio, 1<<heartbeatLED);
    (*p_gpio)->SetMux(p_gpio, heartbeatLED, 0); // set to GPIO
    // Blink
    for (;;) {
        (*p_gpio)->Set(p_gpio, 1<<heartbeatLED);
        UEZTaskDelay(250);
        (*p_gpio)->Clear(p_gpio, 1<<heartbeatLED);
        UEZTaskDelay(250);
    }
}

/*---------------------------------------------------------------------------*
 * Task:  main
 *---------------------------------------------------------------------------*
 * Description:
 *      In the uEZ system, main() is a task.  Do not exit this task
 *      unless you want to the board to reset.  This function should
 *      setup the system and then run the main loop of the program.
 * Outputs:
 *      int                     -- Output error code
 *---------------------------------------------------------------------------*/
int MainTask(void)
{
    T_uezDevice lcd;
    T_pixelColor *pixels;

    //Load the NVSetting from the EEPROM, includes calibration data
    if (NVSettingsLoad() != UEZ_ERROR_NONE) {
        printf("EEPROM Settings\n");
        NVSettingsInit();
        NVSettingsSave();
    }

    // Start up the heart beat of the LED
    UEZTaskCreate(Heartbeat, "Heart", 64, (void *)0, UEZ_PRIORITY_NORMAL, 0);
    // Force calibration?
    Calibrate(CalibrateTestIfTouchscreenHeld());

    // Open the LCD and get the pixel buffer
    if (UEZLCDOpen("LCD", &lcd) == UEZ_ERROR_NONE)  {
        UEZLCDGetFrame(lcd, 0, (void **)&pixels);
        UEZLCDOn(lcd);
        UEZLCDBacklight(lcd, 255);

        AudioStart();

        //Aplication code goes here:

        HelloWorld();
        //End aplication code.
        //Should not reach this point
        while(1);

        UEZLCDClose(lcd);
    }
    return 0;
}

/*---------------------------------------------------------------------------*
 * Routine:  uEZPlatformStartup
 *---------------------------------------------------------------------------*
 * Description:
 *      When uEZ starts, a special Startup task is created and called.
 *      This task brings up the all the hardware, reports any problems,
 *      starts the main task, and then exits.
 *---------------------------------------------------------------------------*/
TUInt32 uEZPlatformStartup(T_uezTask aMyTask, void *aParameters)
{
#if FREERTOS_PLUS_TRACE //LPC1788 only as of uEZ v2.04
    TUInt32 traceAddressInMemory = 0;
#endif
    
    UEZPlatform_Standard_Require();
    SUIInitialize(SIMPLEUI_DOUBLE_SIZED_ICONS, EFalse, EFalse); // SWIM not flipped

#if UEZ_ENABLE_USB_HOST_STACK
    #if USB_PORT_B_HOST_DETECT_ENABLED
    G_usbIsDevice = UEZPlatform_Host_Port_B_Detect();
    if (G_usbIsDevice) {
        // High for a device
        #if UEZ_ENABLE_USB_DEVICE_STACK
        UEZPlatform_USBDevice_Require();
        #endif
        UEZPlatform_USBHost_PortA_Require();
        UEZPlatform_USBFlash_Drive_Require(0);
    } else {
        // Low for a host
        UEZPlatform_USBHost_PortB_Require();
        UEZPlatform_USBFlash_Drive_Require(0);
    }
    #else
        #if UEZ_ENABLE_USB_DEVICE_STACK
        UEZPlatform_USBDevice_Require();
        #endif
        UEZPlatform_USBHost_PortA_Require();
        UEZPlatform_USBFlash_Drive_Require(0);
    #endif
#else 
        #if UEZ_ENABLE_USB_DEVICE_STACK
        UEZPlatform_USBDevice_Require();
        #endif
#endif

        UEZPlatform_SDCard_Drive_Require(1);
        
    // Network needed?
#if UEZ_ENABLE_WIRED_NETWORK
    UEZPlatform_WiredNetwork0_Require();
#endif

#if UEZ_ENABLE_GAINSPAN_ON_BOARD
    //Currently only supported by uEZGU-1788-56VI
#if (UEZ_SLIDESHOW_NAME != "uEZGUI-1788-56VI")
#error "Not a supported product for on board WiFi!"
#endif
    UEZPlatform_GainSpan_OnBoard_Require();
#endif

#if FREERTOS_PLUS_TRACE //LPC1788 only as of uEZ v2.04
    uiTraceStart();
    vTraceStartStatusMonitor();

    traceAddressInMemory = (TUInt32)vTraceGetTraceBuffer();
    printf("%x", traceAddressInMemory);
#endif
    // Create a main task (not running yet)
    UEZTaskCreate((T_uezTaskFunction)MainTask, "Main", MAIN_TASK_STACK_SIZE, 0,
            UEZ_PRIORITY_NORMAL, &G_mainTask);

    // Done with this task, fall out
    return 0;
}