#include "EPD_Test.h"   // Examples
#include "run_File.h"

#include "led.h"
#include "waveshare_PCF85063.h" // RTC
#include "DEV_Config.h"

#include <time.h>

extern const char *fileList;
extern char pathName[];

// Leave the GPIO6 RTC route disabled: S1 OFF selects manual refresh only.
// S1 ON enables timed refresh through the power-wakeup / BAT_STATE circuit.
#define enChargingRtc 0

/*
Mode 0: Automatically get pic folder names and sort them
Mode 1: Automatically get pic folder names but not sorted
Mode 2: pic folder name is not automatically obtained, users need to create fileList.txt file and write the picture name in TF card by themselves
*/
#define Mode 0


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
    // Rearm even when GPIO6 polling is disabled, so S1 ON keeps refreshing.
    rtcRunAlarm(Time, alarmTime);
    printf("RTC rearmed: %02u:%02u:%02u -> %02u:%02u:%02u\r\n",
           (unsigned)Time.hours, (unsigned)Time.minutes, (unsigned)Time.seconds,
           (unsigned)alarmTime.hours, (unsigned)alarmTime.minutes, (unsigned)alarmTime.seconds);
}

int main(void)
{
    Time_data Time = {2024-2000, 3, 31, 0, 0, 0};
    Time_data alarmTime = Time;
    // alarmTime.seconds += 30;
    alarmTime.minutes += 5; 
    // alarmTime.hours +=1;
    char isCard = 0;
  
    printf("Init...\r\n");
    if(DEV_Module_Init() != 0) {  // DEV init
        return -1;
    }
    
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

    if(!sdTest()) 
    {
        isCard = 1;
        if(Mode == 0)
        {
            printf("Mode 0\r\n");
            sdScanDir();
            file_sort();
        }
        if(Mode == 1)
        {
            printf("Mode 1\r\n");
            sdScanDir();
        }
        if(Mode == 2)
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
        while(DEV_Digital_Read(VBUS)) {
            // measureVBAT();
            printf("RTC_INT = %d, BAT_STATE = %d\r\n",DEV_Digital_Read(RTC_INT),DEV_Digital_Read(BAT_STATE));
            #if enChargingRtc
                if(!DEV_Digital_Read(RTC_INT)) {
                    printf("rtc interrupt\r\n");
                    run_display(Time, alarmTime, isCard);
                }
            #endif

            if(!DEV_Digital_Read(BAT_STATE)) {  // K2, or RTC power wakeup with S1 ON
                printf("key interrupt\r\n");
                run_display(Time, alarmTime, isCard);
            }
            DEV_Delay_ms(1000);
        }
    }
    
    printf("power off ...\r\n");
    powerOff(); // BAT off

    return 0;
}
