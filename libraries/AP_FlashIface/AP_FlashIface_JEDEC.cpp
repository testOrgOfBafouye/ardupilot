/*
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Code by Siddharth Bharat Purohit
 */
/*
    Implements Common Flash Interface Driver based on following
    standard published by JEDEC
    * JEDEC Standard, JESD216D, Serial Flash Discoverable Parameters (SFDP)
*/

#include <AP_HAL/AP_HAL.h>
#include "AP_FlashIface_JEDEC.h"
#include <AP_Math/AP_Math.h>
#include <AP_HAL_ChibiOS/QSPIDevice.h>
#ifdef HAL_BOOTLOADER_BUILD
#include "../../Tools/AP_Bootloader/support.h"
#else
extern const AP_HAL::HAL& hal;
#endif

struct supported_device {
    const char* name;
    uint8_t manufacturer_id;
    uint8_t device_id;
};

static const struct supported_device supported_devices[] = {
    {"mt25q", 0x20, 0xBA}
};

// Vendor Specific Constants
// Following Commands Sets were found here:
// * JEDEC Standard JESD251-A1, Addendum No. 1 to JESD251, Optional x4 Quad I/O
//   With Data Strobe
/// NOTE: Except Read ID and Multiline Read ID, they seem to be
// constant across manufacturers, but can't find official standard on
// this.
#define CMD_READ_ID                         0x9F
#define CMD_MULTILINE_READ_ID               0xAF
#define CMD_PAGE_PROGRAM                    0x02
#define CMD_WRITE_DISABLE                   0x04
#define CMD_READ_STATUS                     0x05
#define CMD_MASS_ERASE                      0xC7
#define CMD_RESET_ENABLE                    0x66
#define CMD_RESET_MEMORY                    0x99
#define CMD_READ_SFDP                       0x5A

#define SFDP_MASK(lo, hi)                   (((1UL<<(hi)) - ((1UL<<(lo)))) + (1UL<<(hi)))
#define SFDP_GET_BITS(x, lo, hi)            (((x) & SFDP_MASK(lo, hi)) >> (lo))
#define SFDP_GET_BIT(x, bit)                ((x) & (1<<(bit)))

#define SFDP_HDR_NUM_PARAMS(x)              (SFDP_GET_BITS(x[1], 16, 19) + 1)
#define SFDP_HDR_PARAM_REV(x)               SFDP_GET_BITS(x[1], 0, 15)
#define SFDP_PARAM_ID(x)                    ((SFDP_GET_BITS(x[0], 0, 3) << 8) | SFDP_GET_BITS(x[1], 24, 31))
#define SFDP_PARAM_DWORD_LEN(x)             SFDP_GET_BITS(x[0], 24, 31)
#define SFDP_PARAM_POINTER(x)               SFDP_GET_BITS(x[1], 0, 23)

#define SFDP_REV_1_6                        0x0106

#ifdef HAL_BOOTLOADER_BUILD
#define Debug(fmt, args ...)  do {uprintf("JEDEC: " fmt "\n", ## args);} while(0)
#define Msg_Print(fmt, args ...)  do {uprintf("JEDEC: " fmt "\n", ## args);} while(0)
#else
#define Debug(fmt, args ...)  do {hal.console->printf("JEDEC: " fmt "\n", ## args);} while(0)
#define Msg_Print(fmt, args ...)  do {hal.console->printf("JEDEC: " fmt "\n", ## args);} while(0)
#endif
#define MAX_READ_SIZE       1024UL

static ChibiOS::QSPIDeviceManager qspi;

bool AP_FlashIface_JEDEC::init()
{
    // Get device bus by name
    _dev = nullptr;
    for (uint8_t i = 0; i < ARRAY_SIZE(supported_devices); i++) {
        _dev = qspi.get_device(supported_devices[i].name);
        if (_dev) {
            _dev_list_idx = i;
            break;
        }
    }
    // Reset Device involves trying to soft reset the chip
    // as when system reboots the device might not have.
    reset_device();

    // Detecting Device involves trying to read Device ID and matching
    // with what we expect. Along with extracting info from SFDP
    if (!detect_device()) {
        Msg_Print("Failed to detect flash device: %s", supported_devices[_dev_list_idx].name);
        return false;
    }

    // Configuring Device involved setting chip to correct WSPI mode
    // i.e. 4-4-4
    if (!configure_device()) {
        Msg_Print("Failed to config flash device: %s", supported_devices[_dev_list_idx].name);
        return false;
    }

    Msg_Print("Detected Flash Device: %s", supported_devices[_dev_list_idx].name);
    return true;
}

//////////////////////////////////////////////////////
////////////////// Internal Methods //////////////////
//////////////////////////////////////////////////////

// reset chip to known default power on state
void AP_FlashIface_JEDEC::reset_device()
{
    // Get chip out of XIP mode
    AP_HAL::QSPIDevice::CommandHeader cmd;
    uint8_t buf[1];
    _dev->get_semaphore()->take_blocking();

    /* Attempting a reset of the XIP mode, it could be in an unexpected state
     because a CPU reset does not reset the memory too.*/
    /* Resetting XIP mode by reading one byte without XIP confirmation bit.*/
    cmd.cmd   = 0U;
    cmd.alt   = 0xFFU;
    cmd.addr  = 0U;
    cmd.dummy = 7U;
    cmd.cfg   = AP_HAL::QSPI::CFG_CMD_MODE_NONE |
                AP_HAL::QSPI::CFG_ADDR_SIZE_24 |
                AP_HAL::QSPI::CFG_ADDR_MODE_FOUR_LINES |
                AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES |
                AP_HAL::QSPI::CFG_ALT_MODE_FOUR_LINES |
                AP_HAL::QSPI::CFG_ALT_SIZE_8;
    _dev->set_cmd_header(cmd);
    _dev->transfer(nullptr, 0, buf, 1);

    /// NOTE: This is Vendor specific, we haven't read the parameter table
    /// yet, so don't know how we can reset the chip.
    // Various methods for Soft Reset are at Ref. JESD216D 6.4.19
    /* Quad line CMD_RESET_ENABLE command.*/
    cmd.cmd   =  CMD_RESET_ENABLE;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES;
    cmd.addr  =  0;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    _dev->transfer(nullptr, 0, nullptr, 0);

    /* Quad line CMD_RESET_MEMORY command.*/
    cmd.cmd   =  CMD_RESET_MEMORY;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES;
    cmd.addr  =  0;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    _dev->transfer(nullptr, 0, nullptr, 0);

    /* Single line CMD_RESET_MEMORY command.*/
    cmd.cmd    =  CMD_RESET_ENABLE;
    cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE;
    cmd.addr   =  0;
    cmd.alt    =  0;
    cmd.dummy  =  0;
    _dev->set_cmd_header(cmd);
    _dev->transfer(nullptr, 0, nullptr, 0);


    /* Single line N25Q_CMD_RESET_MEMORY command.*/
    cmd.cmd   =  CMD_RESET_MEMORY;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE;
    cmd.addr  =  0;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    _dev->transfer(nullptr, 0, nullptr, 0);

    // By now we are pretty sure the chip is reset
}

// Does initial configuration to bring up and setup chip
bool AP_FlashIface_JEDEC::detect_device()
{
    AP_HAL::QSPIDevice::CommandHeader cmd;

    {
        uint8_t buf[3] {};
        cmd.cmd    =  CMD_READ_ID;
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE |
                      AP_HAL::QSPI::CFG_DATA_MODE_ONE_LINE;
        cmd.addr   =  0;
        cmd.alt    =  0;
        cmd.dummy  =  0;

        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, buf, sizeof(buf))) {
            Debug("Failed to read Device ID");
            return false;
        }

        if (buf[0] != supported_devices[_dev_list_idx].manufacturer_id ||
            buf[1] != supported_devices[_dev_list_idx].device_id) {
            return false;
        }
    }

    // Read SFDP header to get information Ref. JESD216D 4 and 6.2
    {
        uint32_t sfdp_header[2];

        cmd.cmd    =  CMD_READ_SFDP;
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE |
                      AP_HAL::QSPI::CFG_ADDR_MODE_ONE_LINE |
                      AP_HAL::QSPI::CFG_ADDR_SIZE_24 |
                      AP_HAL::QSPI::CFG_DATA_MODE_ONE_LINE;
        cmd.addr   =  0;
        cmd.alt    =  0;
        cmd.dummy  =  8; // 8 dummy cycles
        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, (uint8_t*)sfdp_header, sizeof(sfdp_header))) {
            Debug("SFDP Header read failed");
            return false;
        }

        // Read Signature
        if (memcmp(sfdp_header, "SFDP", 4)) {
            Debug("SFDP Bad Signature: 0x%lx", (unsigned long)sfdp_header[0]);
            return false;
        }

        // Read Num Param Headers
        if (SFDP_HDR_NUM_PARAMS(sfdp_header) == 0) {
            Debug("Unsupported number of param headers %ld", (unsigned long)SFDP_HDR_NUM_PARAMS(sfdp_header));
            return false;
        }
        // Read Revision
        _desc.param_rev = SFDP_HDR_PARAM_REV(sfdp_header);
        if (_desc.param_rev != SFDP_REV_1_6) {
            Debug("Unsupported revision %x", (unsigned int)_desc.param_rev);
            return false;
        }
    }


    // Read Param Header Ref. JESD216D 6.4.1 6.4.2
    {
        uint32_t param_header[2] {}; // read only first parameter header
        // Immediately after 2 DWORDS of SFDP Header
        cmd.addr = 2*sizeof(uint32_t);
        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, (uint8_t*)param_header, sizeof(param_header))) {
            Debug("Param header read failed");
            return false;
        }

        if (SFDP_PARAM_ID(param_header) != 0xFF) {
            Debug("Only basic Param Table supported not %lx", (unsigned long)SFDP_PARAM_ID(param_header));
            return false;
        }
        // Lets get the length of parameter table
        _desc.param_table_len = MIN(SFDP_PARAM_DWORD_LEN(param_header), 20UL);
        _desc.param_table_pointer = SFDP_PARAM_POINTER(param_header);
    }

    // Read and parse the param table
    {
        uint32_t param_table[20] {};
        cmd.addr = _desc.param_table_pointer;
        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, (uint8_t*)param_table, _desc.param_table_len*sizeof(uint32_t))) {
            Debug("Failed to read Parameter Table");
            return false;
        }

        // Flash Memory details Ref. JESD216D 6.4.5 6.4.14
        if (SFDP_GET_BIT(param_table[1], 31)) {
            Debug("Unsupported Flash Size");
            return false;
        }
        _desc.flash_size = SFDP_GET_BITS(param_table[1], 0, 30)/8;
        _desc.page_size = 1UL<<SFDP_GET_BITS(param_table[10], 4, 7);
        _desc.page_count = _desc.flash_size/_desc.page_size;
        if (_desc.page_count == 0) {
            Debug("Page size greater than flash size unsupported");
            return false;
        }

        // Erase Flash Memory details Ref. JESD216D 6.4.11 6.4.12
        for (uint8_t i = 0; i < 4; i++) {
            uint32_t size = 1UL<<SFDP_GET_BITS(param_table[7 + (i/2)], 0 + 16*(i%2), 7 + 16*(i%2));
            uint8_t ins = SFDP_GET_BITS(param_table[7 + (i/2)], 8 + 16*(i%2), 15 + 16*(i%2));
            if ((size-1) > 0) {
                _desc.erase_type[i].size = size;
                _desc.erase_type[i].ins = ins;
                if (size > _desc.sector_size) {
                    _desc.sector_size = size;
                }
                if (size < _desc.min_erase_size) {
                    _desc.min_erase_size = size;
                }
            }
        }
        _desc.sector_count = _desc.flash_size/_desc.sector_size;
        if (_desc.sector_count == 0) {
            _desc.sector_count = 1;
        }
        // Read Erase Times 6.4.13
        uint8_t timeout_mult = 2*(SFDP_GET_BITS(param_table[9], 0, 3) + 1);
        for (uint8_t i = 0; i < 4; i++) {
            if (_desc.erase_type[i].size) {
                uint32_t unit = SFDP_GET_BITS(param_table[9], 9+(7*i), 10+(7*i));
                uint8_t val = SFDP_GET_BITS(param_table[9], 4+(7*i), 8+(7*i));
                if (unit == 0b00) {
                    unit = 1; //1ms
                } else if (unit == 0b01) {
                    unit = 16; // 16ms
                } else if (unit == 0b10) {
                    unit = 128; // 128ms
                } else if (unit == 0b11) {
                    unit = 1000; // 1s
                }

                _desc.erase_type[i].delay_ms = (val+1)*unit;
                _desc.erase_type[i].timeout_ms = timeout_mult*_desc.erase_type[i].delay_ms;
            }
        }
        // Mass Erase times 6.4.14
        uint32_t unit = SFDP_GET_BITS(param_table[10], 29, 30);
        if (unit == 0b00) {
            unit = 16; // 16ms
        } else if (unit == 0b01) {
            unit = 256; // 256ms
        } else if (unit == 0b10) {
            unit = 4000; // 4s
        } else if (unit == 0b11) {
            unit = 64000; // 64s
        }
        _desc.mass_erase_delay_ms = (SFDP_GET_BITS(param_table[10], 24, 28) + 1)*unit;
        _desc.mass_erase_timeout_ms = timeout_mult*_desc.mass_erase_delay_ms;

        // Setup Write Enable Instruction Ref. JESD216D 6.4.19
        // If needed legacy support Ref. JESD216D 6.4.4 and implement that
        if (SFDP_GET_BIT(param_table[15], 0) ||
            SFDP_GET_BIT(param_table[15], 1)) {
            _desc.write_enable_ins = 0x06;
        } else if (SFDP_GET_BIT(param_table[15], 2)) {
            _desc.write_enable_ins = 0x50;
        } else if (SFDP_GET_BITS(param_table[15], 3, 6)) {
            Debug("Unsupported Register Write Enable Config");
            return false;
        }

        // Setup Program timings Ref. JESD216D 6.4.14
        // unit = SFDP_GET_BIT(param_table[10], 23)?1:8;
        // _desc.add_byte_prog_delay_us = (SFDP_GET_BITS(19, 22) + 1) * unit;
        // _desc.add_byte_prog_timeout_us = _desc.add_byte_prog_delay_us * timeout_mult;
        // unit = SFDP_GET_BIT(param_table[10], 18)?1:8;
        // _desc.first_byte_prog_delay_us = (SFDP_GET_BITS(14, 17) + 1) * unit;
        // _desc.first_byte_prog_timeout_us = _desc.first_byte_prog_delay_us * timeout_mult;

        // Implement above code if more precise delay and timeouts are needed while programming
        // otherwise fraction of page timings should be fine
        timeout_mult = 2*(SFDP_GET_BITS(param_table[10], 0, 3) + 1);
        unit = SFDP_GET_BIT(param_table[10], 18)?8:64;
        _desc.page_prog_delay_us = (SFDP_GET_BITS(param_table[10], 8, 13) + 1) * unit;
        _desc.page_prog_timeout_us = _desc.page_prog_delay_us * timeout_mult;


        // Configure Quad Mode Enable and Read Sequence, Ref. JESD216D 6.4.8 6.4.10 6.4.18
        if (!SFDP_GET_BIT(param_table[4], 4)) {
            Debug("Quad mode unsupported");
            return false;
        }

        _desc.fast_read_ins = SFDP_GET_BITS(param_table[6], 24, 31);
        // we get number of dummy clocks cycles needed, also include mode bits
        _desc.fast_read_mode_clocks =  SFDP_GET_BITS(param_table[6], 21, 23);
        _desc.fast_read_dummy_cycles = SFDP_GET_BITS(param_table[6], 16, 20);

        uint8_t QER = SFDP_GET_BITS(param_table[14], 20, 22);
        if (QER != 0b000) {
            Debug("Unsupported Quad Enable Requirement");
            return false;
        }
        if (SFDP_GET_BIT(param_table[14], 4)) {
            Debug("Unsupported Quad Enable Requirement: set QE bits");
            return false;
        }

        if (SFDP_GET_BIT(param_table[14], 5)) {
            _desc.quad_mode_ins = 0x38;
        } else if (SFDP_GET_BIT(param_table[14], 6)) {
            _desc.quad_mode_ins = 0x35;
        }

        if (SFDP_GET_BIT(param_table[14], 7)) {
            Debug("Unsupported Quad enable seq");
            return false;
        } else if (SFDP_GET_BIT(param_table[14], 8)) {
            _desc.quad_mode_rmw_seq = true;
        } else {
            _desc.quad_mode_rmw_seq = false;
        }

        // Configure XIP mode Ref. JESD216D 6.4.18
        if (SFDP_GET_BIT(param_table[14], 9)) {
            _desc.is_xip_supported = true;
        } else {
            _desc.is_xip_supported = false;
        }
        if (_desc.is_xip_supported) {
            if (SFDP_GET_BIT(param_table[14],17)) {
                _desc.entry_method = AP_FlashIface_JEDEC::XIP_ENTRY_METHOD_2;
            } else {
                Debug("Unsupported XIP enable sequence");       
            }
        }

        // Configure Status Polling Method Ref. JESD216D 6.4.17
        if (SFDP_GET_BIT(param_table[13], 3)) {
            _desc.legacy_status_polling = false;
            _desc.status_read_ins = 0x70;
        } else if (SFDP_GET_BIT(param_table[13], 2)) {
            _desc.legacy_status_polling = true;
            _desc.status_read_ins = 0x05;
        }
    }
    initialised = true;
    return true;
}

// Configures device to normal working state, currently 4-4-4 QSPI
bool AP_FlashIface_JEDEC::configure_device()
{
    AP_HAL::QSPIDevice::CommandHeader cmd;
    // Enable 4-4-4 mode and test it by fetching Device ID
    {
        // Quad Mode Enable Sequence, Ref. JESD216D 6.4.18
        if (_desc.quad_mode_ins && !write_enable_called) {
            if (!send_cmd(_desc.quad_mode_ins, false)) {
                return false;
            }
        } else if (_desc.quad_mode_rmw_seq) {
            write_enable(false);
            if (!modify_reg(0x65, 0x61, (1<<7), 0, false)) {
                write_disable(false);
                return false;
            }
            write_disable(true);
        }

        uint8_t buf[3] {};
        cmd.cmd    =  CMD_MULTILINE_READ_ID;
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES;
        cmd.addr   =  0;
        cmd.alt    =  0;
        cmd.dummy  =  0;

        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, buf, sizeof(buf))) {
            Debug("Failed to switch to Quad mode");
            return false;
        }

        if (buf[0] != supported_devices[_dev_list_idx].manufacturer_id ||
            buf[1] != supported_devices[_dev_list_idx].device_id) {
            Debug("Device bad mfg_id(0x%x) and dev_id(0x%x)", (unsigned int)buf[0], (unsigned int)buf[1]);
            return false;
        }
        Debug("Device configured for 4-4-4 mode");
    }
    // Hurray! We are in 4-4-4 mode
    _quad_spi_mode = true;
    return true;
}

// Enables commands that modify flash data or settings
bool AP_FlashIface_JEDEC::write_enable(bool quad_mode)
{
    if (_desc.write_enable_ins) {
        write_enable_called = true;
        return send_cmd(_desc.write_enable_ins, quad_mode);
    }
    return true;
}

// Disables commands that modify flash data or settings
bool AP_FlashIface_JEDEC::write_disable(bool quad_mode)
{
    if (_desc.write_enable_ins) {
        write_enable_called = true;
        return send_cmd(CMD_WRITE_DISABLE, quad_mode);
    }
    return true;
}

// Read modify write register
bool AP_FlashIface_JEDEC::modify_reg(uint8_t read_ins, uint8_t write_ins,
                                     uint8_t mask, uint8_t val, bool quad_mode)
{
    // Read
    uint8_t reg_val;
    if (!read_reg(read_ins, reg_val, quad_mode)) {
        return false;
    }

    // Modify
    reg_val = (reg_val & ~mask) | (val & mask);

    // Write
    if (!write_reg(write_ins, reg_val, quad_mode)) {
        return false;
    }
    return true;
}

// reads a register value of chip using instruction
bool AP_FlashIface_JEDEC::read_reg(uint8_t read_ins, uint8_t &read_val, bool quad_mode)
{
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd = read_ins;
    if (quad_mode) {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES;
    } else {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE |
                      AP_HAL::QSPI::CFG_DATA_MODE_ONE_LINE;
    }
    cmd.addr   =  0;
    cmd.alt    =  0;
    cmd.dummy  =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(nullptr, 0, &read_val, sizeof(read_val))) {
        Debug("Failed Register Read");
        return false;
    }
    return true;
}

// sends instruction to write a register value in the chip
bool AP_FlashIface_JEDEC::write_reg(uint8_t read_ins, uint8_t write_val, bool quad_mode)
{
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd = read_ins;
    if (quad_mode) {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES;
    } else {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE |
                      AP_HAL::QSPI::CFG_DATA_MODE_ONE_LINE;
    }
    cmd.addr   =  0;
    cmd.alt    =  0;
    cmd.dummy  =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(&write_val, 1, nullptr, 0)) {
        Debug("Failed Register Write");
        return false;
    }
    return true;
}

// Sends QSPI command without data
bool AP_FlashIface_JEDEC::send_cmd(uint8_t ins, bool quad_mode)
{
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd = ins;
    if (quad_mode) {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES;
    } else {
        cmd.cfg    =  AP_HAL::QSPI::CFG_CMD_MODE_ONE_LINE;
    }
    cmd.addr   =  0;
    cmd.alt    =  0;
    cmd.dummy  =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(nullptr, 0, nullptr, 0)) {
        Debug("Failed Register Write");
        return false;
    }
    return true;
}

//////////////////////////////////////////////////////
////////////////////PUBLIC METHODS////////////////////
//////////////////////////////////////////////////////
/**
 * @details Sends command to erase the entire chips.
 *
 * @param[out] delay_ms     Time to wait until next is_device_busy call
 * @param[out] timeout_ms   Time by which the erase should have timedout
 *
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::start_mass_erase(uint32_t &delay_ms, uint32_t &timeout_ms)
{
    write_enable(true);
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd   =  CMD_MASS_ERASE;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES;
    cmd.addr  =  0;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(nullptr, 0, nullptr, 0)) { // Command only
        write_disable(true);
        Debug("Failed to send erase command");
        return false;
    }
    delay_ms = _desc.mass_erase_delay_ms;
    timeout_ms = _desc.mass_erase_timeout_ms;
    write_disable(true);
    return true;
}

/**
 * @details Sends command to erase a sector of the chip.
 *
 * @param[in]  sector       Sector number to be erased
 * @param[out] delay_ms     Time to wait until next is_device_busy call
 * @param[out] timeout_ms   Time by which the erase should have timedout
 *
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::start_sector_erase(uint32_t sector, uint32_t &delay_ms, uint32_t &timeout_ms)
{
    if (sector > _desc.sector_count) {
        Debug("Invalid sector");
        return false;
    }
    uint32_t erasing;
    bool ret = start_erase_offset(_desc.sector_size*sector, _desc.sector_size, erasing, delay_ms, timeout_ms);
    if (!ret || (erasing != _desc.sector_size)) {
        Debug("Failed to erase sector");
        return false;
    }
    return true;
}

/**
 * @details Tries to erase as much as possible starting from the offset
 *          until size. User needs to call this as many times as needed
 *          taking already erased bytes into account, until desired erase
 *          has taken place
 *
 * @param[in] offset        address offset for erase
 * @param[in] size          size desired to be erased
 * @param[out] erasing      number of bytes erasing
 * @param[out] delay_ms     Time to wait until next is_device_busy call
 * @param[out] timeout_ms   Time by which the erase should have timedout
 *
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::start_erase_offset(uint32_t offset, uint32_t size, uint32_t &erasing,
        uint32_t &delay_ms, uint32_t &timeout_ms)
{
    uint8_t ins;
    uint32_t erase_size = 0;
    erasing = 0;
    // Find the maximum size we can erase
    for (uint8_t i=0; i < 4; i++) {
        if (_desc.erase_type[i].size == 0) {
            continue;
        }
        if (_desc.erase_type[i].size < erase_size) {
            // we already found a larger size we can erase
            continue;
        }
        // check if we can find an instruction to match the erase req.
        if ((size >= _desc.erase_type[i].size) && !(offset % _desc.erase_type[i].size)) {
            erase_size = size;
            ins = _desc.erase_type[i].ins;
            delay_ms = _desc.erase_type[i].delay_ms;
            timeout_ms = _desc.erase_type[i].timeout_ms;
        }
    }
    if (erase_size == 0) {
        Debug("Requested Erase size is too small");
        return false;
    }
    // Start Erasing
    write_enable(true);
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd   =  ins;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                 AP_HAL::QSPI::CFG_ADDR_MODE_FOUR_LINES |
                 AP_HAL::QSPI::CFG_ADDR_SIZE_24;
    cmd.addr  =  offset;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(nullptr, 0, nullptr, 0)) { // Command only
        write_disable(true);
        Debug("Failed to send erase command");
        return false;
    }
    write_disable(true);
    erasing = erase_size;
    return true;
}


/**
 * @details Check if selected sector is erased.
 *
 * @param[in] sector        sector for which to check erase
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::verify_sector_erase(uint32_t sector)
{
    uint8_t buf[MAX_READ_SIZE] {};  // Read 1KB per read
    for (uint32_t offset = _desc.sector_size*sector; offset < (_desc.sector_size*(sector+1)); offset+=sizeof(buf)) {
        if (read(offset, buf, sizeof(buf))) {
            for (uint16_t i = 0; i < sizeof(buf); i++) {
                if (buf[i] != 0xFF) {
                    Debug("Found unerased byte %x @ offset %ld", (unsigned int)buf[i], (unsigned long)offset);
                    return false;
                }
            }
        } else {
            Debug("Read Failed");
            return false;
        }
    }
    return true;
}

/**
 * @details Sends command to start programming a page of the chip.
 *
 * @param[in]  page         Page number to be written to
 * @param[in]  data         data to be written
 * @param[out] delay_us     Time to wait until next is_device_busy call
 * @param[out] timeout_us   Time after which the erase should be timedout,
 *                          should be reset at every call.
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::start_program_page(uint32_t page, const uint8_t* data,
        uint32_t &delay_us, uint32_t &timeout_us)
{
    if (page > _desc.page_count) {
        Debug("Invalid Page");
        return false;
    }
    uint32_t programming;
    bool ret = start_program_offset(_desc.page_size*page, data, _desc.sector_size, programming, delay_us, timeout_us);
    if (!ret || (programming != _desc.page_size)) {
        Debug("Failed to program page");
        return false;
    }
    return true;
}

/**
 * @details Tries to program as much as possible starting from the offset
 *          until size. User needs to call this as many times as needed
 *          taking already programmed bytes into account.
 *
 * @param[in] offset        address offset for program
 * @param[in] data          data to be programmed
 * @param[in] size          size desired to be programmed
 * @param[out] programming  number of bytes programming, taking care of the limits
 * @param[out] delay_us     Time to wait until program typically finishes
 * @param[out] timeout_us   Time by which current program should have timedout.
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::start_program_offset(uint32_t offset, const uint8_t* data, uint32_t size, uint32_t &programming,
        uint32_t &delay_us, uint32_t &timeout_us)
{
    if (size > _desc.page_size) {
        // we can only program single
        // page at the max in one go
        size = _desc.page_size;
    }
    // Ensure we don't go beyond the page of offset, while writing
    size = MIN(_desc.page_size - (offset % _desc.page_size), size);

    write_enable(true);
    AP_HAL::QSPIDevice::CommandHeader cmd;
    cmd.cmd   =  CMD_PAGE_PROGRAM;
    cmd.cfg   =  AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                 AP_HAL::QSPI::CFG_ADDR_MODE_FOUR_LINES |
                 AP_HAL::QSPI::CFG_ADDR_SIZE_24 |
                 AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES;
    cmd.addr  =  offset;
    cmd.alt   =  0;
    cmd.dummy =  0;
    _dev->set_cmd_header(cmd);
    if (!_dev->transfer(data, size, nullptr, 0)) { // Command only
        write_disable(true);
        Debug("Failed to send program command");
        return false;
    }
    write_disable(true);
    programming = size;
    // we are mostly going to program in chunks so this will do
    delay_us = (_desc.page_prog_delay_us*size)/(_desc.page_size);
    timeout_us = (_desc.page_prog_timeout_us*size)/(_desc.page_size);
    return true;
}

/**
 * @details Read data from flash chip.
 *
 * @param[in] offset        address offset from where to start the read
 * @param[out] data         data to be read from the device
 * @param[in] size          size of the data to be read
 * @return                  The operation status.
 * @retval false            if the operation failed.
 * @retval true             if the operation succeeded.
 *
 */
bool AP_FlashIface_JEDEC::read(uint32_t offset, uint8_t* data, uint32_t size)
{
    if ((offset + size) > _desc.flash_size) {
        // reading more than what exists
        return false;
    }
    uint32_t read_ptr, read_size;
    for (read_ptr = offset; read_ptr < (offset+size); read_ptr+=MAX_READ_SIZE) {
        read_size = MIN((offset+size) - read_ptr, MAX_READ_SIZE);
        AP_HAL::QSPIDevice::CommandHeader cmd;
        cmd.cmd     =   _desc.fast_read_ins;
        cmd.cfg     =   AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                        AP_HAL::QSPI::CFG_ADDR_MODE_FOUR_LINES |
                        AP_HAL::QSPI::CFG_ADDR_SIZE_24 |
                        AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES;
        cmd.addr    =   read_ptr;
        cmd.alt     =   0;
        // mode bits are 0 so might as well add them to dummy
        cmd.dummy   =   _desc.fast_read_dummy_cycles + _desc.fast_read_mode_clocks; 
        _dev->set_cmd_header(cmd);
        if (!_dev->transfer(nullptr, 0, &data[read_ptr-offset], read_size)) { // Command only
            Debug("Failed to read flash");
            return false;
        }
    }
    return true;
}

/**
 * @details Check if the device is busy.
 *
 * @return                  device busy with last op.
 *
 * @retval false            if the device is ready.
 * @retval true             if the device is busy.
 *
 */
bool AP_FlashIface_JEDEC::is_device_busy()
{
    uint8_t status;
    read_reg(_desc.status_read_ins, status, true);
    if (_desc.legacy_status_polling) {
        return (status & 0x1);
    } else {
        return !(status & 1<<7);
    }
}

bool AP_FlashIface_JEDEC::start_xip_mode(void** addr)
{
    if (!_desc.is_xip_supported) {
        Debug("XIP mode unsupported on this chip");
        return false;
    }
    switch(_desc.entry_method) {
        case AP_FlashIface_JEDEC::XIP_ENTRY_METHOD_2:
        {
            // set configuration register to start 0-4-4 mode
            write_enable(true);
            if (!modify_reg(0x85, 0x81, 1<<3, 0, true)) {
                Debug("Failed to configure chip for XIP");
                write_disable(true);
                return false;
            }
            // Set QSPI module for XIP mode
            AP_HAL::QSPIDevice::CommandHeader cmd;
            cmd.cmd = _desc.fast_read_ins;
            cmd.alt = 0;
            cmd.cfg = AP_HAL::QSPI::CFG_ADDR_SIZE_24 |
                      AP_HAL::QSPI::CFG_CMD_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_ADDR_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_DATA_MODE_FOUR_LINES |
                      AP_HAL::QSPI::CFG_ALT_MODE_FOUR_LINES |  /* Always 4 lines, note.*/
                      AP_HAL::QSPI::CFG_ALT_SIZE_8 |
                      AP_HAL::QSPI::CFG_SIOO;
            cmd.addr = 0;
            // correct dummy bytes because of addition of alt bytes
            cmd.dummy = _desc.fast_read_dummy_cycles - 1;
            _dev->set_cmd_header(cmd);
            return _dev->enter_xip_mode(addr);
        }
        default:
        {
            Debug("Unsupported XIP Entry Method");
            return false;
        }
    }
}

bool AP_FlashIface_JEDEC::stop_xip_mode()
{
    return _dev->exit_xip_mode();
}