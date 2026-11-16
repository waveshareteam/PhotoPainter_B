// USB disk mode: exposes the SD card to the host as a USB Mass Storage drive,
// so pictures can be copied without removing the card.
//
// While in this mode the firmware never mounts FatFs itself; the host owns the
// file system. SD access happens from the main loop (tud_task() is called from
// thread context, not from an IRQ) because the SD driver uses mutexes and DMA
// interrupts that must not be used from interrupt context.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#if PICO_RP2350
#include "hardware/structs/powman.h"
#endif
#include "tusb.h"

#include "DEV_Config.h"
#include "hw_config.h"
#include "sd_card.h"
#include "diskio.h"
#include "usb_disk.h"

#if !CFG_TUD_MSC
#error "usb_msc/tusb_config.h is not being used (CFG_TUD_MSC is 0)"
#endif

// watchdog scratch[0..3] survive a watchdog reboot; scratch[4..7] belong to the SDK/bootrom.
#define SCRATCH_IDX          0               // what to do on the next boot
#define SCRATCH_EXIT_IDX     1               // why USB disk mode was left
#define SCRATCH_ACTIVE_IDX   2               // set while USB disk mode runs
#define SCRATCH_ERRORS_IDX   3               // SD read/write error counters
#define ERRORS_MAGIC         0x45520000u     // "ER" + write errs << 8 + read errs
#define MAGIC_ENTER_DISK     0x55534244u     // "USBD"
#define MAGIC_REFRESH        0x52465348u     // "RFSH"
#define MAGIC_DISK_ACTIVE    0x41435456u     // "ACTV"
#define EXIT_MAGIC           0x45580000u     // "EX" + reason in the low half

typedef enum {
    EXIT_VBUS = 1,
    EXIT_EJECT,
    EXIT_KEY,
    EXIT_NO_HOST,
} exit_reason_t;

static const char *const EXIT_REASON_STR[] = {
    [EXIT_VBUS]    = "USB power lost (VBUS low for 300 ms)",
    [EXIT_EJECT]   = "ejected by the computer",
    [EXIT_KEY]     = "NEXT key pressed",
    [EXIT_NO_HOST] = "no USB host for 15 s",
};

#define BLOCK_SIZE           512
#define NO_HOST_TIMEOUT_MS   15000           // leave if no host enumerates us
#define EJECT_REBOOT_DELAY_MS 300            // let the host see the eject complete
#define VBUS_LOST_MS         300             // ignore short VBUS dips (e.g. during SD writes)
#define KEY_DEBOUNCE_MS      80

bool usb_disk_active = false;

static sd_card_t *sd;
static bool disk_ready;
static uint32_t block_count;
static volatile bool ejected;
static absolute_time_t eject_time;
static absolute_time_t last_io;
static char last_exit_msg[128];
static uint32_t read_errors, write_errors;

static void count_sd_error(bool write)
{
    uint32_t *n = write ? &write_errors : &read_errors;
    if (*n < 0xff) {
        (*n)++;
    }
    watchdog_hw->scratch[SCRATCH_ERRORS_IDX] = ERRORS_MAGIC | (write_errors << 8) | read_errors;
}

const char *usb_disk_last_exit(void)
{
    return last_exit_msg[0] ? last_exit_msg : NULL;
}

// Work out how the previous run ended (called once, early in boot).
static void record_last_exit(void)
{
    uint32_t exitInfo = watchdog_hw->scratch[SCRATCH_EXIT_IDX];
    bool wasActive = watchdog_hw->scratch[SCRATCH_ACTIVE_IDX] == MAGIC_DISK_ACTIVE;
    uint32_t errInfo = watchdog_hw->scratch[SCRATCH_ERRORS_IDX];
    uint32_t stageInfo = watchdog_hw->scratch[SCRATCH_ACTIVE_IDX];   // normal mode: DIAG_STAGE_MAGIC
    watchdog_hw->scratch[SCRATCH_EXIT_IDX] = 0;
    watchdog_hw->scratch[SCRATCH_ACTIVE_IDX] = 0;
    watchdog_hw->scratch[SCRATCH_ERRORS_IDX] = 0;

    uint32_t why = exitInfo & 0xffffu;
    if ((exitInfo & 0xffff0000u) == EXIT_MAGIC && why >= EXIT_VBUS && why <= EXIT_NO_HOST) {
        snprintf(last_exit_msg, sizeof last_exit_msg, "USB disk mode ended: %s", EXIT_REASON_STR[why]);
    } else if (wasActive && watchdog_enable_caused_reboot()) {
        snprintf(last_exit_msg, sizeof last_exit_msg, "USB disk mode ended: watchdog timeout (firmware stuck > 8 s)");
    } else if (wasActive) {
        snprintf(last_exit_msg, sizeof last_exit_msg, "USB disk mode ended: unexpected reset");
    } else if ((stageInfo & 0xff000000u) == DIAG_STAGE_MAGIC && watchdog_enable_caused_reboot()) {
        static const char *const STAGE_STR[] = {
            "?", "boot", "SD card scan", "e-Paper init", "BMP read",
            "JPG decode", "e-Paper refresh", "idle (charging loop)",
        };
        uint32_t stage = (stageInfo >> 16) & 0xff;
        snprintf(last_exit_msg, sizeof last_exit_msg, "watchdog timeout (stuck > 8 s) during %s, detail %lu",
                 stage < count_of(STAGE_STR) ? STAGE_STR[stage] : "?", (unsigned long)(stageInfo & 0xffff));
    }
    if (last_exit_msg[0] && (errInfo & 0xffff0000u) == ERRORS_MAGIC) {
        size_t n = strlen(last_exit_msg);
        snprintf(last_exit_msg + n, sizeof last_exit_msg - n, ", SD errors: write %lu, read %lu",
                 (unsigned long)((errInfo >> 8) & 0xff), (unsigned long)(errInfo & 0xff));
    }
#if PICO_RP2350
    // A brown-out wipes the watchdog scratch registers, so it is reported on its own.
    else if (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_BOR_BITS) {
        snprintf(last_exit_msg, sizeof last_exit_msg, "boot after brown-out (supply voltage dropped)");
    }
#endif
}

static void __attribute__((noreturn)) reboot_with(uint32_t magic)
{
    watchdog_hw->scratch[SCRATCH_IDX] = magic;
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

usb_disk_boot_t usb_disk_boot_check(void)
{
    // Re-assert the battery power latch first: a (watchdog) reboot resets all
    // pins, which releases BAT_OFF until DEV_GPIO_Init() runs much later.
    gpio_init(BAT_OFF);
    gpio_put(BAT_OFF, 1);
    gpio_set_dir(BAT_OFF, GPIO_OUT);

    record_last_exit();

    uint32_t magic = watchdog_hw->scratch[SCRATCH_IDX];
    watchdog_hw->scratch[SCRATCH_IDX] = 0;

    if (magic == MAGIC_ENTER_DISK) {
        return USB_DISK_BOOT_ENTER;
    }
    if (magic == MAGIC_REFRESH) {
        return USB_DISK_BOOT_REFRESH;
    }

    // Holding NEXT while powering up over USB also enters disk mode.
    gpio_init(VBUS);
    gpio_set_dir(VBUS, GPIO_IN);
    gpio_init(BAT_STATE);
    gpio_set_dir(BAT_STATE, GPIO_IN);
    gpio_pull_up(BAT_STATE);
    sleep_ms(1);

    if (!gpio_get(VBUS)) {
        return USB_DISK_BOOT_NORMAL;    // on battery: NEXT is just the wake-up key
    }
    for (int held = 0; held < USB_DISK_HOLD_MS; held += 10) {
        if (gpio_get(BAT_STATE)) {
            return USB_DISK_BOOT_NORMAL;
        }
        sleep_ms(10);
    }
    return USB_DISK_BOOT_ENTER;
}

void usb_disk_request_and_reboot(void)
{
    printf("rebooting into USB disk mode ...\r\n");
    stdio_flush();
    sleep_ms(50);
    reboot_with(MAGIC_ENTER_DISK);
}

static void __attribute__((noreturn)) leave_disk_mode(bool refresh, exit_reason_t why)
{
    watchdog_hw->scratch[SCRATCH_EXIT_IDX] = EXIT_MAGIC | why;
    watchdog_hw->scratch[SCRATCH_ACTIVE_IDX] = 0;
    DEV_Digital_Write(LED_ACT, 0);
    tud_disconnect();
    sleep_ms(100);  // let the card finish any internal programming
    reboot_with(refresh ? MAGIC_REFRESH : 0);
}

static void update_charge_led(void)
{
    // Same meaning as in normal mode: PWR LED on while charging.
    DEV_Digital_Write(LED_PWR, !DEV_Digital_Read(CHARGE_STATE));
}

void usb_disk_run(void)
{
    usb_disk_active = true;

    DEV_Hardware_Init();                // pins/SPI/ADC, without USB stdio
    // Keep EPD_POWER_EN high like the normal firmware does: the SD card may sit
    // on the same switched rail, and the reboot into this mode just reset the
    // pin (rail off), so give the card time to power up before talking to it.
    DEV_Digital_Write(LED_ACT, 1);      // ACT LED steady on = USB disk mode
    watchdog_enable(8000, 1);
    sleep_ms(250);

    sd = sd_get_by_num(0);
    for (int attempt = 0; attempt < 3 && !disk_ready; attempt++) {
        if (attempt) {
            sleep_ms(200);
        }
        watchdog_update();
        disk_ready = (sd_init_card(sd) & (STA_NOINIT | STA_NODISK)) == 0;
    }
    if (disk_ready) {
        uint64_t sectors = sd_sectors(sd);
        block_count = sectors > UINT32_MAX ? UINT32_MAX : (uint32_t)sectors;
    }

    tusb_init();
    watchdog_hw->scratch[SCRATCH_ERRORS_IDX] = ERRORS_MAGIC;   // 0 errors so far
    watchdog_hw->scratch[SCRATCH_ACTIVE_IDX] = MAGIC_DISK_ACTIVE;

    absolute_time_t last_mounted = get_absolute_time();
    absolute_time_t key_released_at = nil_time;
    absolute_time_t key_pressed_at = nil_time;
    absolute_time_t vbus_low_since = nil_time;
    bool key_armed = false;     // NEXT may still be held from entering this mode
    last_io = nil_time;

    while (true) {
        tud_task();
        watchdog_update();
        update_charge_led();

        absolute_time_t now = get_absolute_time();

        // ACT LED: steady on = idle, fast blink = reading/writing the card,
        // 5 Hz blink with no transfer = SD card failed to initialise.
        uint32_t ms = to_ms_since_boot(now);
        if (!disk_ready) {
            DEV_Digital_Write(LED_ACT, (ms / 100) & 1);
        } else if (!is_nil_time(last_io) && absolute_time_diff_us(last_io, now) < 100 * 1000) {
            DEV_Digital_Write(LED_ACT, (ms / 60) & 1);
        } else {
            DEV_Digital_Write(LED_ACT, 1);
        }

        if (tud_mounted()) {
            last_mounted = now;
        } else if (absolute_time_diff_us(last_mounted, now) > NO_HOST_TIMEOUT_MS * 1000) {
            leave_disk_mode(false, EXIT_NO_HOST);   // e.g. a charger without data lines
        }

        // Unplugged: VBUS must stay low for a while, a single low sample is
        // not enough (heavy SD writes can make VBUS sag for a moment).
        if (!DEV_Digital_Read(VBUS)) {
            if (is_nil_time(vbus_low_since)) {
                vbus_low_since = now;
            } else if (absolute_time_diff_us(vbus_low_since, now) > VBUS_LOST_MS * 1000) {
                leave_disk_mode(false, EXIT_VBUS);
            }
        } else {
            vbus_low_since = nil_time;
        }

        if (ejected && absolute_time_diff_us(eject_time, now) > EJECT_REBOOT_DELAY_MS * 1000) {
            leave_disk_mode(true, EXIT_EJECT);      // host ejected the drive
        }

        // NEXT press: leave disk mode (eject on the computer first!).
        if (DEV_Digital_Read(BAT_STATE)) {
            if (is_nil_time(key_released_at)) {
                key_released_at = now;
            }
            if (absolute_time_diff_us(key_released_at, now) > 100 * 1000) {
                key_armed = true;
            }
            key_pressed_at = nil_time;
        } else {
            key_released_at = nil_time;
            if (key_armed) {
                // time-based debounce (a loop pass can take long during transfers)
                if (is_nil_time(key_pressed_at)) {
                    key_pressed_at = now;
                } else if (absolute_time_diff_us(key_pressed_at, now) > KEY_DEBOUNCE_MS * 1000) {
                    leave_disk_mode(true, EXIT_KEY);
                }
            }
        }

        sleep_us(1000);
    }
}

//--------------------------------------------------------------------+
// TinyUSB MSC callbacks (only invoked in USB disk mode)
//--------------------------------------------------------------------+
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char vid[] = "PhotoPnt";
    const char pid[] = "SD Card";
    const char rev[] = "1.0";
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!disk_ready || ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size)
{
    (void)lun;
    *count = disk_ready ? block_count : 0;
    *size = BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        ejected = true;
        eject_time = get_absolute_time();
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!disk_ready || offset != 0 || bufsize % BLOCK_SIZE) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        return -1;
    }
    watchdog_update();
    last_io = get_absolute_time();
    if (sd_read_blocks(sd, buffer, lba, bufsize / BLOCK_SIZE) != SD_BLOCK_DEVICE_ERROR_NONE) {
        count_sd_error(false);
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);    // unrecovered read error
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!disk_ready || offset != 0 || bufsize % BLOCK_SIZE) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        return -1;
    }
    watchdog_update();
    last_io = get_absolute_time();
    if (sd_write_blocks(sd, buffer, lba, bufsize / BLOCK_SIZE) != SD_BLOCK_DEVICE_ERROR_NONE) {
        count_sd_error(true);
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);    // write error
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    // Everything the host needs is handled by TinyUSB's built-in commands.
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);    // invalid command
    return -1;
}
