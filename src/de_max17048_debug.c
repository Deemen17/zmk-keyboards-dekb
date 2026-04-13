#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * MAX17048 Debug Module
 * 
 * Purpose: Verify MAX17048 fuel gauge is communicating correctly over I2C
 * Add to firmware to debug battery reading issues
 * 
 * Compile with: CONFIG_I2C_LOG_LEVEL_DBG=y
 * Output on RTT Terminal or serial monitor
 */

#define MAX17048_ADDR 0x36
#define MAX17048_VCELL_REG 0x02
#define MAX17048_SOC_REG 0x04
#define MAX17048_VERSION_REG 0x08
#define MAX17048_CONFIG_REG 0x0C
#define MAX17048_STATUS_REG 0x1A

struct max17048_debug_data {
    uint8_t vcell_reg[2];
    uint8_t soc_reg[2];
    uint8_t version_reg[2];
    uint8_t config_reg[2];
    uint8_t status_reg[2];
    int i2c_error;
    bool initialized;
};

static struct max17048_debug_data debug_data = {0};
static const struct device *i2c_dev;

/**
 * Thử đọc MAX17048 registers qua I2C
 * Return: 0 nếu thành công, <0 nếu lỗi
 */
static int max17048_debug_read_registers(void) {
    int ret = 0;
    
    // Verify I2C device exists
    i2c_dev = DEVICE_DT_GET(DT_N_S_soc_s_i2c_40000_0);
    if (!i2c_dev) {
        i2c_dev = device_get_binding("I2C_0");
    }
    
    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready!");
        debug_data.i2c_error = -ENODEV;
        return -ENODEV;
    }
    
    LOG_INF("I2C device ready: %s", i2c_dev->name);
    
    // Đọc VCELL register (0x02-0x03)
    ret = i2c_burst_read(i2c_dev, MAX17048_ADDR, MAX17048_VCELL_REG, 
                         debug_data.vcell_reg, sizeof(debug_data.vcell_reg));
    if (ret < 0) {
        LOG_ERR("Failed to read VCELL: %d", ret);
        debug_data.i2c_error = ret;
        return ret;
    }
    LOG_INF("VCELL reg: 0x%02X 0x%02X", debug_data.vcell_reg[0], debug_data.vcell_reg[1]);
    
    // Đọc SOC register (0x04-0x05)
    ret = i2c_burst_read(i2c_dev, MAX17048_ADDR, MAX17048_SOC_REG, 
                         debug_data.soc_reg, sizeof(debug_data.soc_reg));
    if (ret < 0) {
        LOG_ERR("Failed to read SOC: %d", ret);
        debug_data.i2c_error = ret;
        return ret;
    }
    LOG_INF("SOC reg: 0x%02X 0x%02X (%%)", debug_data.soc_reg[0], debug_data.soc_reg[1]);
    
    // Đọc VERSION register (0x08)
    ret = i2c_burst_read(i2c_dev, MAX17048_ADDR, MAX17048_VERSION_REG, 
                         debug_data.version_reg, sizeof(debug_data.version_reg));
    if (ret < 0) {
        LOG_ERR("Failed to read VERSION: %d", ret);
        debug_data.i2c_error = ret;
        return ret;
    }
    LOG_INF("VERSION reg: 0x%02X 0x%02X", debug_data.version_reg[0], debug_data.version_reg[1]);
    
    // Đọc CONFIG register (0x0C)
    ret = i2c_burst_read(i2c_dev, MAX17048_ADDR, MAX17048_CONFIG_REG, 
                         debug_data.config_reg, sizeof(debug_data.config_reg));
    if (ret < 0) {
        LOG_ERR("Failed to read CONFIG: %d", ret);
        debug_data.i2c_error = ret;
        return ret;
    }
    LOG_INF("CONFIG reg: 0x%02X 0x%02X", debug_data.config_reg[0], debug_data.config_reg[1]);
    
    // Đọc STATUS register (0x1A)
    ret = i2c_burst_read(i2c_dev, MAX17048_ADDR, MAX17048_STATUS_REG, 
                         debug_data.status_reg, sizeof(debug_data.status_reg));
    if (ret < 0) {
        LOG_ERR("Failed to read STATUS: %d", ret);
        debug_data.i2c_error = ret;
        return ret;
    }
    LOG_INF("STATUS reg: 0x%02X 0x%02X", debug_data.status_reg[0], debug_data.status_reg[1]);
    
    debug_data.initialized = true;
    return 0;
}

/**
 * Parse các giá trị từ registers
 */
static void max17048_debug_parse_values(void) {
    // VCELL: 12-bit ADC, 78.125µV per LSB
    uint16_t vcell_raw = ((uint16_t)debug_data.vcell_reg[0] << 8) | debug_data.vcell_reg[1];
    vcell_raw = vcell_raw >> 4;  // Right shift 4 bits (12-bit ADC)
    uint32_t vcell_mv = (uint32_t)vcell_raw * 78 / 1000;  // Convert to mV
    
    // SOC: Integer part (MSB)
    uint8_t soc_int = debug_data.soc_reg[0];
    
    // STATUS: Check for any error flags
    uint8_t status = debug_data.status_reg[0];
    bool por = (status & 0x01) != 0;  // Power-On Reset
    bool hac = (status & 0x02) != 0;  // Imbalanced cell
    bool dq = (status & 0x04) != 0;   // Long discharge
    
    LOG_INF("=== MAX17048 Parsed Values ===");
    LOG_INF("Voltage: %d mV", vcell_mv);
    LOG_INF("SOC: %d%%", soc_int);
    LOG_INF("POR flag: %s", por ? "SET" : "clear");
    LOG_INF("HAC flag: %s", hac ? "SET" : "clear");
    LOG_INF("DQ flag: %s", dq ? "SET" : "clear");
    LOG_INF("============================");
}

/**
 * Init thread - chạy 2 giây sau boot để test MAX17048
 */
void max17048_debug_thread(void) {
    k_sleep(K_MSEC(2000));  // Wait for I2C/MAX17048 startup
    
    LOG_INF("=== Starting MAX17048 Debug Test ===");
    
    int ret = max17048_debug_read_registers();
    if (ret == 0) {
        LOG_INF("✓ MAX17048 I2C communication SUCCESS");
        max17048_debug_parse_values();
    } else {
        LOG_ERR("✗ MAX17048 I2C communication FAILED (error: %d)", ret);
        LOG_ERR("Check: I2C address 0x36, pull-up resistors, power supply");
    }
    
    LOG_INF("=== MAX17048 Debug Test Complete ===");
}

// Define debug thread - Enable/disable by uncommenting
// Uncomment line below để enable debug testing
// #define ENABLE_MAX17048_DEBUG 1

#ifdef ENABLE_MAX17048_DEBUG
K_THREAD_DEFINE(max17048_debug_tid, 
                512, 
                max17048_debug_thread, 
                NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 
                0, 
                2000);  // Start 2 seconds after boot
#endif
