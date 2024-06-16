#include <stdio.h>
#include <stdlib.h>

#include "jerryscript.h"
#include "lfs.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/gpio.h"

volatile bool _stopTriggered = false;
volatile bool _threadRunning = false;

void thread_runner()
{
    printf("starting core1\r\n");

    _threadRunning = true;

    // Allow lockout of core1
    multicore_lockout_victim_init();

    // jerry_port_log(JERRY_LOG_LEVEL_TRACE, "thread_runner running core 1");

    // psj_repl_run_flash(vm_exec_stop_callback);

    int led_pin = 13;
    //const GPIO_OUT = true;

    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    while (!_stopTriggered) {
        gpio_put(led_pin, true);
        sleep_ms(1000);
        gpio_put(led_pin, false);
        sleep_ms(1000);
    }

    // jerry_port_log(JERRY_LOG_LEVEL_TRACE, "thread_runner exiting core 1");
    _threadRunning = false;
}

#define FLASH_TARGET_OFFSET (1024 * 1024)
#define FLASH_TARGET_SIZE (1024 * 1024)

const uint8_t *flash_buffer = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

// Read a region in a block. Negative error codes are propagated
// to the user.
int user_provided_block_device_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    jerry_port_log(JERRY_LOG_LEVEL_TRACE, "user_provided_block_device_read(block: %u, off: %u, buffer: %p, size: %u)", block, off, buffer, size);

    uint32_t offset = (c->block_size * block) + off;
    const uint8_t *pBuff = flash_buffer + offset;
    memcpy(buffer, pBuff, size);

    return LFS_ERR_OK;
}

// Program a region in a block. The block must have previously
// been erased. Negative error codes are propagated to the user.
// May return LFS_ERR_CORRUPT if the block should be considered bad.
int user_provided_block_device_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    jerry_port_log(JERRY_LOG_LEVEL_TRACE, "user_provided_block_device_prog(block: %u, off: %u, buffer: %p, size: %u)", block, off, buffer, size);

    // set a page of data
    uint32_t offset = (c->block_size * block) + off;
    flash_range_program(FLASH_TARGET_OFFSET + offset, buffer, size);

    return LFS_ERR_OK;
}

// Erase a block. A block must be erased before being programmed.
// The state of an erased block is undefined. Negative error codes
// are propagated to the user.
// May return LFS_ERR_CORRUPT if the block should be considered bad.
int user_provided_block_device_erase(const struct lfs_config *c, lfs_block_t block)
{
    jerry_port_log(JERRY_LOG_LEVEL_TRACE, "user_provided_block_device_erase(block: %u)", block);
    uint32_t offset = c->block_size * block;

    // Clear entire flash_buffer
    flash_range_erase(FLASH_TARGET_OFFSET + offset, c->block_size);

    return LFS_ERR_OK;
}

// Sync the state of the underlying block device. Negative error codes
// are propagated to the user.
int user_provided_block_device_sync(const struct lfs_config *c)
{
    jerry_port_log(JERRY_LOG_LEVEL_TRACE, "user_provided_block_device_sync()");
    return LFS_ERR_OK;
}

lfs_t lfs;

// configuration of the filesystem is provided by this struct
const struct lfs_config cfg = {
    // block device operations
    .read  = user_provided_block_device_read,
    .prog  = user_provided_block_device_prog,
    .erase = user_provided_block_device_erase,
    .sync  = user_provided_block_device_sync,

    // block device configuration
    .read_size = FLASH_PAGE_SIZE,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = (FLASH_TARGET_SIZE / FLASH_SECTOR_SIZE),
    .cache_size = FLASH_PAGE_SIZE,
    .lookahead_size = FLASH_PAGE_SIZE,
    .block_cycles = 500,
};

void do_flash_init()
{
    // mount the filesystem
    int err = lfs_mount(&lfs, &cfg);

    // reformat if we can't mount the filesystem
    // this should only happen on the first boot
    if (err) {
        err = lfs_format(&lfs, &cfg);
        err = lfs_mount(&lfs, &cfg);
    }
}

int do_flash_list(struct lfs_info *file_info, uint32_t max_file_info)
{
    lfs_dir_t root_dir;
    int err = lfs_dir_open(&lfs, &root_dir, "/");
    if (err < 0)
    {
        jerry_port_log(JERRY_LOG_LEVEL_ERROR, "Error opening root directory: %i", err);
        return 0;
    }

    int idx = 0;
    while (idx < max_file_info && (err = lfs_dir_read(&lfs, &root_dir, file_info + idx)) > 0) 
    {
        if (file_info[idx].name[0] != '.')
        {
            idx++;
        }
    }

    if (err < 0)
    {
        jerry_port_log(JERRY_LOG_LEVEL_ERROR, "Error reading root directory: %i", err);
    }

    err = lfs_dir_close(&lfs, &root_dir);
    if (err < 0)
    {
        jerry_port_log(JERRY_LOG_LEVEL_ERROR, "Error closing root directory: %i", err);
    }

    return idx;
}


int do_flash_test()
{
    struct lfs_info file_info[16];

    int file_count = do_flash_list(file_info, 16);

    for (int idx = 0; idx < file_count; idx++)
    {
        jerry_port_log(JERRY_LOG_LEVEL_TRACE, "File '%s': %d bytes", file_info[idx].name, file_info[idx].size);
    }
}

int main()
{
    stdio_init_all();

    jerry_init(JERRY_INIT_EMPTY);
    do_flash_init();

    sleep_ms(5000);

    printf("starting core0\r\n");

    // multicore_reset_core1();
    multicore_launch_core1(thread_runner);

    sleep_ms(5000);
    
    printf("start blocking core 1\r\n");

    multicore_lockout_start_blocking();

    sleep_ms(5000);

    do_flash_test();

    sleep_ms(5000);

    printf("end blocking core 1\r\n");

    // below line will raise isr_hardfault on core1
    multicore_lockout_end_blocking();

    sleep_ms(5000);

    printf("and wait...\r\n");

    while (true)
    {
        tight_loop_contents();
    }

    return 0;
}
