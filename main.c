#include "EPD_Test.h"   // Examples
#include "run_File.h"

#include "led.h"
#include "waveshare_PCF85063.h" // RTC
#include "DEV_Config.h"
#include "usb_disk.h"   // USB disk mode (SD card as a USB drive)

#include <time.h>

extern const char *fileList;
extern char pathName[];

#define enChargingRtc 0

/*
Mode 0: Automatically get pic folder names and sort them
Mode 1: Automatically get pic folder names but not sorted
Mode 2: pic folder name is not automatically obtained, users need to create fileList.txt file and write the picture name in TF card by themselves
Mode 3: Automatically get pic folder names and play randomly
*/
int PlayMode = 3;


float measureVBAT(void)
{
    float Voltage=0.0;
    const float conversion_factor = 3.3f / (1 << 12);
    uint16_t result = adc_read();
    Voltage = result * conversion_factor * 3;
    printf("Raw value: 0x%03x, voltage: %f V\n", result, Voltage);
    return Voltage;
}

void chargeState_callback() 
{
    if(DEV_Digital_Read(VBUS)) {
        if(!DEV_Digital_Read(CHARGE_STATE)) {  // is charging
            ledCharging();
        }
        else {  // charge complete
            ledCharged();
        }
    }
}

void run_display(Time_data Time, Time_data alarmTime, char hasCard)
{
    if(hasCard) {
        setFilePath();
        EPD_7in3e_display_BMP(pathName, measureVBAT());   // display bmp
    }
    else {
        printf("led_ON_PWR...\r\n");
        EPD_7in3e_display(measureVBAT());
    }
    DEV_Delay_ms(100);
    PCF85063_clear_alarm_flag();    // clear RTC alarm flag
    #if enChargingRtc
        rtcRunAlarm(Time, alarmTime);  // RTC run alarm
    #endif
}

int main(void)
{
    Time_data Time = {2024-2000, 3, 31, 0, 0, 0};
    Time_data alarmTime = Time;
    // alarmTime.seconds += 10;
    // alarmTime.minutes += 5; 
    alarmTime.hours +=1;
    char isCard = 0;

    // USB disk mode: hold NEXT for 3 s while on USB (or while plugging in USB)
    usb_disk_boot_t bootMode = usb_disk_boot_check();
    if(bootMode == USB_DISK_BOOT_ENTER) {
        usb_disk_run(); // never returns
    }
  
    printf("Init...\r\n");
    if(DEV_Module_Init() != 0) {  // DEV init
        return -1;
    }
    
    DEV_SetStage(STAGE_BOOT, 0);
    watchdog_enable(8*1000, 1);   // 8s
    DEV_Delay_ms(1000);
    PCF85063_init();    // RTC init
    rtcRunAlarm(Time, alarmTime);  // RTC run alarm
    gpio_set_irq_enabled_with_callback(CHARGE_STATE, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, chargeState_callback);

    if(measureVBAT() < 3.1) {   // battery power is low
        printf("low power ...\r\n");
        PCF85063_alarm_Time_Disable();
        ledLowPower();  // LED flash for Low power
        powerOff(); // BAT off
        return 0;
    }
    else {
        printf("work ...\r\n");
        ledPowerOn();
    }

    DEV_SetStage(STAGE_SD_SCAN, 0);
    const char *lastExit = usb_disk_last_exit();
    if(lastExit) {
        printf("%s\r\n", lastExit);
    }

    if(!sdTest()) 
    {
        isCard = 1;
        if(lastExit) {
            sdAppendLog(lastExit);
        }
        if(PlayMode == 0)
        {
            printf("Mode 0\r\n");
            sdScanDir();
            file_sort();
        }
        if(PlayMode == 1 || PlayMode == 3)
        {
            printf("Mode %d\r\n", PlayMode);
            sdScanDir();
        }
        if(PlayMode == 2)
        {
            printf("Mode 2\r\n");
            file_cat();
        }
        
    }
    else 
    {
        isCard = 0;
    }

    if(!DEV_Digital_Read(VBUS)) {    // no charge state
        run_display(Time, alarmTime, isCard);
    }
    else {  // charge state
        chargeState_callback();
        if(bootMode == USB_DISK_BOOT_REFRESH) {    // just left USB disk mode: show the new pictures
            printf("back from USB disk mode\r\n");
            run_display(Time, alarmTime, isCard);
        }
    }

    while(true) {
        DEV_SetStage(STAGE_IDLE, 0);
        while(DEV_Digital_Read(VBUS)) {
            measureVBAT();
            #if enChargingRtc
                if(!DEV_Digital_Read(RTC_INT)) {    // RTC interrupt trigger
                    printf("rtc interrupt\r\n");
                    run_display(Time, alarmTime, isCard);
                }
            #endif

            if(!DEV_Digital_Read(BAT_STATE)) {  // KEY pressed
                // short press: next picture; long press: USB disk mode
                int heldMs = 0;
                while(!DEV_Digital_Read(BAT_STATE) && heldMs < USB_DISK_HOLD_MS) {
                    DEV_Delay_ms(20);
                    heldMs += 20;
                }
                if(heldMs >= USB_DISK_HOLD_MS) {
                    usb_disk_request_and_reboot();  // never returns
                }
                printf("key interrupt\r\n");
                run_display(Time, alarmTime, isCard);
                DEV_SetStage(STAGE_IDLE, 0);
            }
            DEV_Delay_ms(200);
        }

        printf("power off ...\r\n");
        powerOff(); // BAT off: on battery this cuts our own power

        // Still running: USB was plugged in meanwhile (e.g. during the e-paper
        // refresh) and is powering the board. Don't return from main() (the
        // firmware would stop and the key would do nothing); re-latch the
        // battery and go back to the charging loop instead.
        DEV_Delay_ms(500);
        if(DEV_Digital_Read(VBUS)) {
            printf("USB plugged in during power off, staying on\r\n");
            DEV_Digital_Write(BAT_OFF, 1);
            chargeState_callback();
        }
    }

    return 0;
}
