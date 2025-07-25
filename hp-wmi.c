// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HP WMI hotkeys
 *
 * Copyright (C) 2008 Red Hat <mjg@redhat.com>
 * Copyright (C) 2010, 2011 Anssi Hannula <anssi.hannula@iki.fi>
 *
 * Portions based on wistron_btns.c:
 * Copyright (C) 2005 Miloslav Trmac <mitr@volny.cz>
 * Copyright (C) 2005 Bernhard Rosenkraenzer <bero@arklinux.org>
 * Copyright (C) 2005 Dmitry Torokhov <dtor@mail.ru>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/power_supply.h>
#include <linux/rfkill.h>
#include <linux/string.h>
#include <linux/dmi.h>

MODULE_AUTHOR("Matthew Garrett <mjg59@srcf.ucam.org>");
MODULE_DESCRIPTION("HP laptop WMI driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("wmi:95F24279-4D7B-4334-9387-ACCDC67EF61C");
MODULE_ALIAS("wmi:5FB7F034-2C63-45E9-BE91-3D44E2C707E4");

#define HPWMI_EVENT_GUID "95F24279-4D7B-4334-9387-ACCDC67EF61C"
#define HPWMI_BIOS_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"

#define HP_OMEN_EC_THERMAL_PROFILE_OFFSET 0x8F

#define HP_POWER_LIMIT_DEFAULT	 0x00
#define HP_POWER_LIMIT_NO_CHANGE 0xFF

#define ACPI_AC_CLASS "ac_adapter"

#define zero_if_sup(tmp) (zero_insize_support?0:sizeof(tmp)) // use when zero insize is required

static const char * const thermal_profile_v1_boards[] = {
	"8BCD"
};

enum hp_fan_mode { //Performance Mode
    FANMODE_LEGACY_DEFAULT     =  0x00, // 0b00000000
    FANMODE_LEGACY_PERFORMANCE =  0x01, // 0b00000001
    FANMODE_LEGACY_COOL        =  0x02, // 0b00000010
    FANMODE_LEGACY_QUIET       =  0x03, // 0b00000011
    FANMODE_LEGACY_EXTREME     =  0x04, // 0b00000100
    FANMODE_L8                 =  0x04, // 0b00000100
    FANMODE_L0                 =  0x10, // 0b00010000
    FANMODE_L5                 =  0x11, // 0b00010001
    FANMODE_L1                 =  0x20, // 0b00100000
    FANMODE_L6                 =  0x21, // 0b00100001
    FANMODE_L2                 =  0x30, // 0b00110000
    FANMODE_L7                 =  0x31, // 0b00110001
    FANMODE_L3                 =  0x40, // 0b01000000
    FANMODE_L4                 =  0x50  // 0b01010000
};

enum AdapterStatus { //not working but still implementing
    ADAPTER_NOT_SUPPORTED     = 0x00,  // No smart power adapter support
    ADAPTER_MEETS_REQUIREMENT = 0x01,  // Sufficient power
    ADAPTER_BELOW_REQUIREMENT = 0x02,  // Insufficient power
    ADAPTER_BATTERY_POWER     = 0x03,  // Not on AC power
    ADAPTER_NOT_FUNCTIONING   = 0x04,  // Malfunction
    ADAPTER_ERROR             = 0xFF   // Error
};

enum Throttling {
    UNKNOWN = 0x00,  // Unknown state (BIOS call failed)
    ON      = 0x01,  // Thermal throttling enabled
    DEFAULT = 0x04   // Observed default state
};

// enum TypeC{
//     USB_TYPEC_SCENARIO_ERROR = -1,
//     USB_TYPEC_SCENARIO_OK = 0,
//     USB_TYPEC_SCENARIO_POWER_ADAPTER_ACCEPTED_MATCHES_CAPABILITIES_TO_CHARGE_WHILE_IN_SX = 1,
//     USB_TYPEC_SCENARIO_POWER_ADAPTER_REJECTED_PROVIDER_AND_CONSUMER_MISMATCH = 7,
//     USB_TYPEC_SCENARIO_ALTERNATE_MODE_REJECTED_INCOMPATIBLE_CABLE = 9,
//     USB_TYPEC_SCENARIO_NON_HP_TYPEC_ADAPTER = 10,
//     USB_TYPEC_SCENARIO_PORT_OVER_VOLTAGE = 13
// };

enum hp_gpu_mode{
    GPUMODE_HYBRID   = 0,  // 0x00 - Hybrid graphics mode (or BIOS call failed)
    GPUMODE_DISCRETE = 1,  // 0x01 - Discrete GPU exclusive mode
    GPUMODE_OPTIMUS  = 2   // 0x02 - NVIDIA Optimus mode
};

enum hp_wmi_radio {
	HPWMI_WIFI	= 0x0,
	HPWMI_BLUETOOTH	= 0x1,
	HPWMI_WWAN	= 0x2,
	HPWMI_GPS	= 0x3,
};

enum hp_wmi_event_ids {
	HPWMI_DOCK_EVENT			= 0x01,
	HPWMI_PARK_HDD				= 0x02,
	HPWMI_SMART_ADAPTER			= 0x03,
	HPWMI_BEZEL_BUTTON			= 0x04,
	HPWMI_WIRELESS				= 0x05,
	HPWMI_CPU_BATTERY_THROTTLE	= 0x06,
	HPWMI_LOCK_SWITCH			= 0x07,
	HPWMI_LID_SWITCH			= 0x08,
	HPWMI_SCREEN_ROTATION		= 0x09,
	HPWMI_COOLSENSE_SYSTEM_MOBILE	= 0x0A,
	HPWMI_COOLSENSE_SYSTEM_HOT	= 0x0B,
	HPWMI_PROXIMITY_SENSOR		= 0x0C,
	HPWMI_BACKLIT_KB_BRIGHTNESS	= 0x0D,
	HPWMI_PEAKSHIFT_PERIOD		= 0x0F,
	HPWMI_BATTERY_CHARGE_PERIOD	= 0x10,
	HPWMI_SANITIZATION_MODE		= 0x17,
	HPWMI_CAMERA_TOGGLE			= 0x1A,
	HPWMI_OMEN_KEY				= 0x1D,
	HPWMI_SMART_EXPERIENCE_APP	= 0x21,
};

struct hp_power_limits {
	u8 pl1;
	u8 pl2;
	u8 pl4;
	u8 cpu_gpu_concurrent_limit;
};

struct hp_gpu_power_modes {
	u8 ctgp_enable;
	u8 ppab_enable;
	u8 dstate;
	u8 gpu_slowdown_temp;
}; 

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[];
};

enum hp_wmi_commandtype {
	HPWMI_DISPLAY_QUERY			= 0x01,
	HPWMI_HDDTEMP_QUERY			= 0x02,
	HPWMI_ALS_QUERY				= 0x03,
	HPWMI_HARDWARE_QUERY		= 0x04,
	HPWMI_WIRELESS_QUERY		= 0x05,
	HPWMI_BATTERY_QUERY			= 0x07,
	HPWMI_BIOS_QUERY			= 0x09,
	HPWMI_FEATURE_QUERY			= 0x0b,
	HPWMI_HOTKEY_QUERY			= 0x0c,
	HPWMI_FEATURE2_QUERY		= 0x0d,
	HPWMI_WIRELESS2_QUERY		= 0x1b,
	HPWMI_POSTCODEERROR_QUERY	= 0x2a,
	HPWMI_SYSTEM_DEVICE_MODE	= 0x40,
};

enum hp_wmi_gm_commandtype {
	HPWMI_GET_GPU_THERMAL_MODES_QUERY	= 0x21,
	HPWMI_SET_GPU_THERMAL_MODES_QUERY	= 0x22,
	HPWMI_GET_TEMPERATURE_QUERY		= 0x23,
	HPWMI_SET_PERFORMANCE_MODE 		= 0x1A,
	HPWMI_FAN_SPEED_MAX_GET_QUERY 	= 0x26,
	HPWMI_FAN_SPEED_MAX_SET_QUERY 	= 0x27,
	HPWMI_GET_SYSTEM_DESIGN_DATA 	= 0x28,
	HPWMI_FAN_SPEED_GET_QUERY		= 0x2D,
	HPWMI_FAN_SPEED_SET_QUERY		= 0x2E,
	HPWMI_FAN_COUNT_GET_QUERY		= 0x10,
	HPWMI_FAN_TABLE_GET_QUERY		= 0x2F,
	HPWMI_FAN_TABLE_SET_QUERY		= 0x32,
	HPWMI_CPU_POWER_SET_QUERY		= 0X29,
	HPWMI_GPU_POWER_QUERY			= 0X52, //MUX
	HPWMI_ADAPTER_QUERY				= 0x0F, //Adapter
};

enum hp_wmi_kb_commandtype{
	HPWMI_GET_BACKLIGHT = 0x04,
	HPWMI_SET_BACKLIGHT = 0x05,
};

enum hp_wmi_command {
	HPWMI_READ	= 0x01, // Earliest implemented (1)
	HPWMI_WRITE	= 0x02, // Graphics mode switch (2)
	HPWMI_ODM	= 0x03,
	HPWMI_GM	= 0x20008, // Most commands (131080){working so running in this}
	HPWMI_GM_v2 = 0X20009, //Current Implementation(going on)
	HPWMI_GM_v3 = 0X2000b, //typec
};

enum backlight {
    BACKLIGHT_OFF = 0x64,  // 0b01100100 - Keyboard backlight off
    BACKLIGHT_ON  = 0xE4   // 0b11100100 - Keyboard backlight on
};

enum hp_wmi_hardware_mask {
	HPWMI_DOCK_MASK		= 0x01,
	HPWMI_TABLET_MASK	= 0x04,
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE		= 0x02,
	HPWMI_RET_UNKNOWN_COMMAND		= 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE		= 0x04,
	HPWMI_RET_INVALID_PARAMETERS	= 0x05,
};

enum hp_wireless2_bits {
	HPWMI_POWER_STATE	= 0x01,
	HPWMI_POWER_SOFT	= 0x02,
	HPWMI_POWER_BIOS	= 0x04,
	HPWMI_POWER_HARD	= 0x08,
	HPWMI_POWER_FW_OR_HW	= HPWMI_POWER_BIOS | HPWMI_POWER_HARD,
};

#define IS_HWBLOCKED(x) ((x & HPWMI_POWER_FW_OR_HW) != HPWMI_POWER_FW_OR_HW)
#define IS_SWBLOCKED(x) !(x & HPWMI_POWER_SOFT)

struct bios_rfkill2_device_state {
	u8 radio_type;
	u8 bus_type;
	u16 vendor_id;
	u16 product_id;
	u16 subsys_vendor_id;
	u16 subsys_product_id;
	u8 rfkill_id;
	u8 power;
	u8 unknown[4];
};

/* 7 devices fit into the 128 byte buffer */
#define HPWMI_MAX_RFKILL2_DEVICES	7

struct bios_rfkill2_state {
	u8 unknown[7];
	u8 count;
	u8 pad[8];
	struct bios_rfkill2_device_state device[HPWMI_MAX_RFKILL2_DEVICES];
};

static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, 0x02,    { KEY_BRIGHTNESSUP } },
	{ KE_KEY, 0x03,    { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x270,   { KEY_MICMUTE } },
	{ KE_KEY, 0x20e6,  { KEY_PROG1 } },
	{ KE_KEY, 0x20e8,  { KEY_MEDIA } },
	{ KE_KEY, 0x2142,  { KEY_MEDIA } },
	{ KE_KEY, 0x213b,  { KEY_INFO } },
	{ KE_KEY, 0x2169,  { KEY_ROTATE_DISPLAY } },
	{ KE_KEY, 0x216a,  { KEY_SETUP } },
	{ KE_IGNORE, 0x21a4,  }, /* Win Lock On */
	{ KE_IGNORE, 0x121a4, }, /* Win Lock Off */
	{ KE_KEY, 0x21a5,  { KEY_PROG2 } }, /* HP Omen Key */
	{ KE_KEY, 0x21a7,  { KEY_FN_ESC } },
	{ KE_KEY, 0x21a8,  { KEY_PROG2 } }, /* HP Envy x360 programmable key */
	{ KE_KEY, 0x21a9,  { KEY_TOUCHPAD_OFF } },
	{ KE_KEY, 0x121a9, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 0x231b,  { KEY_HELP } },
	{ KE_END, 0 }
};

/*
 * Mutex for the active_platform_profile variable,
 * see omen_powersource_event.
 */
static DEFINE_MUTEX(active_platform_profile_lock);

static struct input_dev *hp_wmi_input_dev;
static struct input_dev *camera_shutter_input_dev;
static struct platform_device *hp_wmi_platform_dev;
static struct device *platform_profile_device;
static struct notifier_block platform_power_source_nb;
static enum platform_profile_option active_platform_profile;
static bool platform_profile_support;
static bool zero_insize_support;

static struct rfkill *wifi_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *wwan_rfkill;

static const char * hp_gpumode(int mode)
{
    switch (mode) {
        case GPUMODE_HYBRID:
            return "Hybrid graphics mode";
        case GPUMODE_DISCRETE:
            return "Discrete GPU exclusive mode";
        case GPUMODE_OPTIMUS:
            return "NVIDIA Optimus mode";
        default:
            return "Unknown mode";  // In case of an invalid mode
    }
}

// static const char *usb_typec(int scenario) {
//     switch (scenario) {
//         case USB_TYPEC_SCENARIO_ERROR:
//             return "Error";
//         case USB_TYPEC_SCENARIO_OK:
//             return "Ok";
//         case USB_TYPEC_SCENARIO_POWER_ADAPTER_ACCEPTED_MATCHES_CAPABILITIES_TO_CHARGE_WHILE_IN_SX:
//             return "Power Adapter Accepted, Matches Capabilities to Charge While in Sx";
//         case USB_TYPEC_SCENARIO_POWER_ADAPTER_REJECTED_PROVIDER_AND_CONSUMER_MISMATCH:
//             return "Power Adapter Rejected, Provider and Consumer Mismatch";
//         case USB_TYPEC_SCENARIO_ALTERNATE_MODE_REJECTED_INCOMPATIBLE_CABLE:
//             return "Alternate Mode Rejected, Incompatible Cable";
//         case USB_TYPEC_SCENARIO_NON_HP_TYPEC_ADAPTER:
//             return "Non-HP Type-C Adapter";
//         case USB_TYPEC_SCENARIO_PORT_OVER_VOLTAGE:
//             return "Port Over Voltage";
//         default:
//             return "Unknown scenario";
//     }
// }


struct rfkill2_device {
	u8 id;
	int num;
	struct rfkill *rfkill;
};

static int rfkill2_count;
static struct rfkill2_device rfkill2[HPWMI_MAX_RFKILL2_DEVICES];

/*
 * Chassis Types values were obtained from SMBIOS reference
 * specification version 3.00. A complete list of system enclosures
 * and chassis types is available on Table 17.
 */
static const char * const tablet_chassis_types[] = {
	"30", /* Tablet*/
	"31", /* Convertible */
	"32"  /* Detachable */
};

#define DEVICE_MODE_TABLET	0x06

/* map output size to the corresponding WMI method id */
static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

/*
 * hp_wmi_perform_query
 *
 * query:	The commandtype (enum hp_wmi_commandtype)
 * write:	The command (enum hp_wmi_command)
 * buffer:	Buffer used as input and/or output
 * insize:	Size of input buffer
 * outsize:	Size of output buffer
 *
 * returns zero on success
 *         an HP WMI query specific error code (which is positive)
 *         -EINVAL if the query was not successful at all
 *         -EINVAL if the output buffer size exceeds buffersize
 *
 * Note: The buffersize must at least be the maximum of the input and output
 *       size. E.g. Battery info query is defined to have 1 byte input
 *       and 128 byte output. The caller would do:
 *       buffer = kzalloc(128, GFP_KERNEL);
 *       ret = hp_wmi_perform_query(HPWMI_BATTERY_QUERY, HPWMI_READ, buffer, 1, 128)
 */
static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
				void *buffer, int insize, int outsize)
{
	struct acpi_buffer input, output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct bios_return *bios_return;
	union acpi_object *obj = NULL;
	struct bios_args *args = NULL;
	int mid, actual_insize, actual_outsize;
	size_t bios_args_size;
	int ret;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;

	actual_insize = max(insize, 128);
	bios_args_size = struct_size(args, data, actual_insize);
	args = kmalloc(bios_args_size, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	input.length = bios_args_size;
	input.pointer = args;

	args->signature = 0x55434553;
	args->command = command;
	args->commandtype = query;
	args->datasize = insize;
	memcpy(args->data, buffer, flex_array_size(args, data, insize));

	ret = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	if (ret)
		goto out_free;

	obj = output.pointer;
	if (!obj) {
		ret = -EINVAL;
		goto out_free;
	}

	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_warn("query 0x%x returned an invalid object 0x%x\n", query, ret);
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;

	if (ret) {
		if (ret != HPWMI_RET_UNKNOWN_COMMAND &&
		    ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x returned error 0x%x\n", query, ret);
		goto out_free;
	}

	/* Ignore output data of zero size */
	if (!outsize)
		goto out_free;

	actual_outsize = min(outsize, (int)(obj->buffer.length - sizeof(*bios_return)));
	memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
	memset(buffer + actual_outsize, 0, outsize - actual_outsize);

out_free:
	kfree(obj);
	kfree(args);
	return ret;
}

/*
 *After a 120 seconds timeout however, the laptop goes back to its fallback state.
 */
static int hp_wmi_get_fan_count(void)
{
	u8 fan_data[4] = {};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_COUNT_GET_QUERY, HPWMI_GM,
				   &fan_data, sizeof(u8),
				   sizeof(fan_data));
	if (ret != 0)
		return -EINVAL;

	printk("Bios-control off(120s)\n");
	return fan_data[0]; /* BIOS_PROTECTION-{0},OCP-{1},OTP-{2} */
}

static int hp_wmi_get_fan_speed(int fan)
{
	u8 fan_data[128] = {};
	int ret;

	if (fan < 0 || fan >= sizeof(fan_data))
		return -EINVAL;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_GET_QUERY,
				   HPWMI_GM, &fan_data, sizeof(u8),
				   sizeof(fan_data));
	if (ret != 0)
		return -EINVAL;

	return fan_data[fan] * 100;
}

static int hp_wmi_set_fan_speed(int cpu,int gpu)
{
	u8 fan_speed[2] = { cpu, gpu };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_SET_QUERY, HPWMI_GM,
				   &fan_speed, sizeof(fan_speed), 0);

	return ret;
}

static int hp_wmi_fan_speed_max_set(int enabled)
{
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, HPWMI_GM,
				   &enabled, sizeof(enabled), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return enabled;
}

static int hp_wmi_fan_speed_max_get(void)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GM,
				   &val, zero_if_sup(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

static int hp_wmi_fan_speed_max_reset(void)
{
	int ret;

	ret = hp_wmi_fan_speed_max_set(0);
	if (ret)
		return ret;

	ret = hp_wmi_set_fan_speed(0x00,0x00);
	return ret;
}

static int hp_wmi_get_backlight(void)
{
	int ret;
	u8 data[4]={};

	ret = hp_wmi_perform_query(HPWMI_GET_BACKLIGHT,HPWMI_GM_v2,
				   &data, sizeof(u8),sizeof(data));

	if (ret)
		return ret < 0 ? ret : -EINVAL;
	return (data[0]==0x00)?0:1;
}

static int hp_wmi_set_backlight(enum backlight enabled)
{
	int ret;
	u8 data[4]={enabled,0,0,0};

	ret = hp_wmi_perform_query(HPWMI_SET_BACKLIGHT,HPWMI_GM_v2,
				   &data, sizeof(data), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return 1;
}

// static int hp_wmi_get_UsbTypeCScenario(void)
// {
// 	int ret;
// 	u8 data[4]={0x0};

// 	ret = hp_wmi_perform_query(0x01,HPWMI_GM_v3,
// 				   &data, sizeof(data),128);

// 	if (ret)
// 		return ret < 0 ? ret : -EINVAL;

// 	return ret;
// }

static int hp_wmi_get_gpumode(void)
{
	int ret;
	u8 data[4]={0x0};

	ret = hp_wmi_perform_query(HPWMI_GPU_POWER_QUERY,HPWMI_READ,
				   &data, sizeof(data),sizeof(data));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return ret;
}

static int hp_wmi_set_gpumode(enum hp_gpu_mode enabled)
{
	int ret;
	u8 data[4]={enabled,0,0,0};

	ret = hp_wmi_perform_query(HPWMI_GPU_POWER_QUERY,HPWMI_GM,
				   &data, sizeof(data), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return 1;
}

static int hp_wmi_read_int(int query)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(query, HPWMI_READ, &val,
				   zero_if_sup(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

static int hp_wmi_get_dock_state(void)
{
	int state = hp_wmi_read_int(HPWMI_HARDWARE_QUERY);

	if (state < 0)
		return state;

	return !!(state & HPWMI_DOCK_MASK);
}

static int hp_wmi_get_tablet_mode(void)
{
	char system_device_mode[4] = { 0 };
	const char *chassis_type;
	bool tablet_found;
	int ret;

	chassis_type = dmi_get_system_info(DMI_CHASSIS_TYPE);
	if (!chassis_type)
		return -ENODEV;

	tablet_found = match_string(tablet_chassis_types,
				    ARRAY_SIZE(tablet_chassis_types),
				    chassis_type) >= 0;
	if (!tablet_found)
		return -ENODEV;

	ret = hp_wmi_perform_query(HPWMI_SYSTEM_DEVICE_MODE, HPWMI_READ,
				   system_device_mode, zero_if_sup(system_device_mode),
				   sizeof(system_device_mode));
	if (ret < 0)
		return ret;

	return system_device_mode[0] == DEVICE_MODE_TABLET;
}

static int omen_thermal_profile_set(enum hp_fan_mode mode)
{
	/* The Omen Control Center actively sets the first byte of the buffer to
	 * 255, so let's mimic this behaviour to be as close as possible to
	 * the original software.
	 */
	char buffer[2] = {-1, mode};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_SET_PERFORMANCE_MODE, HPWMI_GM,
				   &buffer, sizeof(buffer), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return mode;
}

static int omen_get_thermal_policy_version(void)
{
	unsigned char buffer[8] = { 0 };
	int ret;

	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (board_name) {
		int matches = match_string(thermal_profile_v1_boards,
			ARRAY_SIZE(thermal_profile_v1_boards),
			board_name);
		if (matches >= 0)
			return 0;
	}

	ret = hp_wmi_perform_query(HPWMI_GET_SYSTEM_DESIGN_DATA, HPWMI_GM,
				   &buffer, sizeof(buffer), sizeof(buffer));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return buffer[3];
}

static int __init hp_wmi_bios_2008_later(void)
{
	int state = 0;
	int ret = hp_wmi_perform_query(HPWMI_FEATURE_QUERY, HPWMI_READ, &state,
				       zero_if_sup(state), sizeof(state));
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_bios_2009_later(void)
{
	u8 state[128];
	int ret = hp_wmi_perform_query(HPWMI_FEATURE2_QUERY, HPWMI_READ, &state,
				       zero_if_sup(state), sizeof(state));
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_enable_hotkeys(void)
{
	int value = 0x6e;
	int ret = hp_wmi_perform_query(HPWMI_BIOS_QUERY, HPWMI_WRITE, &value,
				       sizeof(value), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static int hp_wmi_set_block(void *data, bool blocked)
{
	enum hp_wmi_radio r = (long)data;
	int query = BIT(r + 8) | ((!blocked) << r);
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE,
				   &query, sizeof(query), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill_ops = {
	.set_block = hp_wmi_set_block,
};

static bool hp_wmi_get_sw_state(enum hp_wmi_radio r)
{
	int mask = 0x200 << (r * 8);

	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	/* TBD: Pass error */
	WARN_ONCE(wireless < 0, "error executing HPWMI_WIRELESS_QUERY");

	return !(wireless & mask);
}

static bool hp_wmi_get_hw_state(enum hp_wmi_radio r)
{
	int mask = 0x800 << (r * 8);

	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	/* TBD: Pass error */
	WARN_ONCE(wireless < 0, "error executing HPWMI_WIRELESS_QUERY");

	return !(wireless & mask);
}

static int hp_wmi_rfkill2_set_block(void *data, bool blocked)
{
	int rfkill_id = (int)(long)data;
	char buffer[4] = { 0x01, 0x00, rfkill_id, !blocked };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_WRITE,
				   buffer, sizeof(buffer), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill2_ops = {
	.set_block = hp_wmi_rfkill2_set_block,
};

static int hp_wmi_rfkill2_refresh(void)
{
	struct bios_rfkill2_state state;
	int err, i;

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   zero_if_sup(state), sizeof(state));
	if (err)
		return err;

	for (i = 0; i < rfkill2_count; i++) {
		int num = rfkill2[i].num;
		struct bios_rfkill2_device_state *devstate;

		devstate = &state.device[num];

		if (num >= state.count ||
		    devstate->rfkill_id != rfkill2[i].id) {
			pr_warn("power configuration of the wireless devices unexpectedly changed\n");
			continue;
		}

		rfkill_set_states(rfkill2[i].rfkill,
				  IS_SWBLOCKED(devstate->power),
				  IS_HWBLOCKED(devstate->power));
	}

	return 0;
}

static ssize_t systemdesign_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int ret;
	unsigned char buffer[128] = {0x00};

	ret = hp_wmi_perform_query(HPWMI_GET_SYSTEM_DESIGN_DATA, HPWMI_GM, &buffer ,sizeof(buffer), sizeof(buffer));
	if(ret < 0)
		return -EINVAL;
	
	return sysfs_emit(buf, "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7],
				  buffer[8], buffer[9], buffer[10], buffer[11], buffer[12], buffer[13], buffer[14], buffer[15]);
}

// static ssize_t typec_show(struct device *dev, struct device_attribute *attr, char *buf)
// {
//     int ret;
// 	ret = hp_wmi_get_UsbTypeCScenario();
// 	if(ret < 0)
// 		return -EINVAL;

// 	return sysfs_emit(buf,"%s\n",usb_typec(ret));
// }

static ssize_t adapter_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int ret;
	ret = hp_wmi_read_int(HPWMI_ADAPTER_QUERY);
	if(ret < 0)
		return -EINVAL;

	return sysfs_emit(buf,"%02X\n",ret);
}

static ssize_t mux_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int ret;
	ret = hp_wmi_get_gpumode();
	if(ret < 0)
		return -EINVAL;

	return sysfs_emit(buf,"%s\n",hp_gpumode(ret));
}

static ssize_t fancount_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int ret = hp_wmi_get_fan_count();
	if(ret < 0)
		return -EINVAL;
	return sysfs_emit(buf, "fancount : %d\n", ret);
}

static ssize_t fanspeed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int speed = hp_wmi_get_fan_speed(0);
	if(speed < 0)
		return -EINVAL;
	return sysfs_emit(buf, "speed : %d\n", speed);
}

static ssize_t backlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int ret = hp_wmi_get_backlight();
	if(ret < 0)
		return -EINVAL;
	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t display_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_DISPLAY_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t hddtemp_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_HDDTEMP_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t als_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int value = hp_wmi_read_int(HPWMI_ALS_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t dock_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	int value = hp_wmi_get_dock_state();

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t tablet_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	int value = hp_wmi_get_tablet_mode();

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t postcode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	/* Get the POST error code of previous boot failure. */
	int value = hp_wmi_read_int(HPWMI_POSTCODEERROR_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "0x%x\n", value);
}

static ssize_t backlight_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{	
	hp_wmi_get_fan_count();
	if (buf[0]=='0'){
		hp_wmi_set_backlight(BACKLIGHT_OFF);
	}else if(buf[0]=='1'){
		hp_wmi_set_backlight(BACKLIGHT_ON);
	}
	else
		pr_warn("Invalid input in Backlight feature input(1/0)\n");
	return count;
}

static ssize_t fanspeed_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{	
	int tmp;
	int ret;
	
	ret = kstrtoint(buf, 16, &tmp);
	if (ret < 0){
		pr_warn("Something is wrong\n");
		return ret;
	}
	if (tmp==0)
	{
		hp_wmi_set_fan_speed(0,0);
	}
	else if (tmp >0 && tmp < 63)
	{
		hp_wmi_set_fan_speed(tmp,tmp+3);
	}
	
	else{
		printk("invalid input\n");
	}
	return count;
}

static ssize_t als_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	u32 tmp;
	int ret;

	ret = kstrtou32(buf, 10, &tmp);
	if (ret)
		return ret;

	ret = hp_wmi_perform_query(HPWMI_ALS_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0);
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static ssize_t postcode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	u32 tmp = 1;
	bool clear;
	int ret;

	ret = kstrtobool(buf, &clear);
	if (ret)
		return ret;

	if (clear == false)
		return -EINVAL;

	/* Clear the POST error code. It is kept until cleared. */
	ret = hp_wmi_perform_query(HPWMI_POSTCODEERROR_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0);
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static int camera_shutter_input_setup(void)
{
	int err;

	camera_shutter_input_dev = input_allocate_device();
	if (!camera_shutter_input_dev)
		return -ENOMEM;

	camera_shutter_input_dev->name = "HP WMI camera shutter";
	camera_shutter_input_dev->phys = "wmi/input1";
	camera_shutter_input_dev->id.bustype = BUS_HOST;

	__set_bit(EV_SW, camera_shutter_input_dev->evbit);
	__set_bit(SW_CAMERA_LENS_COVER, camera_shutter_input_dev->swbit);

	err = input_register_device(camera_shutter_input_dev);
	if (err)
		goto err_free_dev;

	return 0;

 err_free_dev:
	input_free_device(camera_shutter_input_dev);
	camera_shutter_input_dev = NULL;
	return err;
}

static DEVICE_ATTR_RO(display);
static DEVICE_ATTR_RO(hddtemp);
static DEVICE_ATTR_RW(als);
static DEVICE_ATTR_RO(dock);
static DEVICE_ATTR_RO(tablet);
static DEVICE_ATTR_RW(postcode);
static DEVICE_ATTR_RW(backlight);
static DEVICE_ATTR_RO(fancount);
static DEVICE_ATTR_RW(fanspeed);
static DEVICE_ATTR_RO(systemdesign);
static DEVICE_ATTR_RO(mux);
static DEVICE_ATTR_RO(adapter);

static struct attribute *hp_wmi_attrs[] = {
	&dev_attr_backlight.attr,
	&dev_attr_mux.attr,	
	&dev_attr_adapter.attr,
	&dev_attr_fancount.attr,
	&dev_attr_fanspeed.attr,
	&dev_attr_systemdesign.attr,
	&dev_attr_display.attr,
	&dev_attr_hddtemp.attr,
	&dev_attr_als.attr,
	&dev_attr_dock.attr,
	&dev_attr_tablet.attr,
	&dev_attr_postcode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hp_wmi);

static void hp_wmi_notify(union acpi_object *obj, void *context)
{
	u32 event_id, event_data;
	u32 *location;
	int key_code;

	if (!obj)
		return;
	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_info("Unknown response received %d\n", obj->type);
		return;
	}

	/*
	 * Depending on ACPI version the concatenation of id and event data
	 * inside _WED function will result in a 8 or 16 byte buffer.
	 */
	location = (u32 *)obj->buffer.pointer;
	if (obj->buffer.length == 8) {
		event_id = *location;
		event_data = *(location + 1);
	} else if (obj->buffer.length == 16) {
		event_id = *location;
		event_data = *(location + 2);
	} else {
		pr_info("Unknown buffer length %d\n", obj->buffer.length);
		return;
	}

	switch (event_id) {
	case HPWMI_DOCK_EVENT:
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK,
					    hp_wmi_get_dock_state());
		if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE,
					    hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);
		break;
	case HPWMI_PARK_HDD:
		break;
	case HPWMI_SMART_ADAPTER:
		break;
	case HPWMI_BEZEL_BUTTON:
		key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if (key_code < 0)
			break;

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown key code - 0x%x\n", key_code);
		break;
	case HPWMI_OMEN_KEY:
		if (event_data) /* Only should be true for HP Omen */
			key_code = event_data;
		else
			key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown key code - 0x%x\n", key_code);
		break;
	case HPWMI_WIRELESS:
		if (rfkill2_count) {
			hp_wmi_rfkill2_refresh();
			break;
		}

		if (wifi_rfkill)
			rfkill_set_states(wifi_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WIFI),
					  hp_wmi_get_hw_state(HPWMI_WIFI));
		if (bluetooth_rfkill)
			rfkill_set_states(bluetooth_rfkill,
					  hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
					  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if (wwan_rfkill)
			rfkill_set_states(wwan_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WWAN),
					  hp_wmi_get_hw_state(HPWMI_WWAN));
		break;
	case HPWMI_CPU_BATTERY_THROTTLE:
		pr_info("Unimplemented CPU throttle because of 3 Cell battery event detected\n");
		break;
	case HPWMI_LOCK_SWITCH:
		break;
	case HPWMI_LID_SWITCH:
		break;
	case HPWMI_SCREEN_ROTATION:
		break;
	case HPWMI_COOLSENSE_SYSTEM_MOBILE:
		break;
	case HPWMI_COOLSENSE_SYSTEM_HOT:
		break;
	case HPWMI_PROXIMITY_SENSOR:
		break;
	case HPWMI_BACKLIT_KB_BRIGHTNESS:
		break;
	case HPWMI_PEAKSHIFT_PERIOD:
		break;
	case HPWMI_BATTERY_CHARGE_PERIOD:
		break;
	case HPWMI_SANITIZATION_MODE:
		break;
	case HPWMI_CAMERA_TOGGLE:
		if (!camera_shutter_input_dev)
			if (camera_shutter_input_setup()) {
				pr_err("Failed to setup camera shutter input device\n");
				break;
			}
		if (event_data == 0xff)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 1);
		else if (event_data == 0xfe)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 0);
		else
			pr_warn("Unknown camera shutter state - 0x%x\n", event_data);
		input_sync(camera_shutter_input_dev);
		break;
	case HPWMI_SMART_EXPERIENCE_APP:
		break;
	default:
		pr_info("Unknown event_id - %d - 0x%x\n", event_id, event_data);
		break;
	}
}

static int __init hp_wmi_input_setup(void)
{
	acpi_status status;
	int err, val;

	hp_wmi_input_dev = input_allocate_device();
	if (!hp_wmi_input_dev)
		return -ENOMEM;

	hp_wmi_input_dev->name = "HP WMI hotkeys";
	hp_wmi_input_dev->phys = "wmi/input0";
	hp_wmi_input_dev->id.bustype = BUS_HOST;

	__set_bit(EV_SW, hp_wmi_input_dev->evbit);

	/* Dock */
	val = hp_wmi_get_dock_state();
	if (!(val < 0)) {
		__set_bit(SW_DOCK, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_DOCK, val);
	}

	/* Tablet mode */
	val = hp_wmi_get_tablet_mode();
	if (!(val < 0)) {
		__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, val);
	}

	err = sparse_keymap_setup(hp_wmi_input_dev, hp_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	/* Set initial hardware state */
	input_sync(hp_wmi_input_dev);

	if (!hp_wmi_bios_2009_later() && hp_wmi_bios_2008_later())
		hp_wmi_enable_hotkeys();

	status = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		err = -EIO;
		goto err_free_dev;
	}

	err = input_register_device(hp_wmi_input_dev);
	if (err)
		goto err_uninstall_notifier;

	return 0;

 err_uninstall_notifier:
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
 err_free_dev:
	input_free_device(hp_wmi_input_dev);
	return err;
}

static void hp_wmi_input_destroy(void)
{
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
	input_unregister_device(hp_wmi_input_dev);
}

static int __init hp_wmi_rfkill_setup(struct platform_device *device)
{
	int err, wireless;

	wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	if (wireless < 0)
		return wireless;

	err = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &wireless,
				   sizeof(wireless), 0);
	if (err)
		return err;

	if (wireless & 0x1) {
		wifi_rfkill = rfkill_alloc("hp-wifi", &device->dev,
					   RFKILL_TYPE_WLAN,
					   &hp_wmi_rfkill_ops,
					   (void *) HPWMI_WIFI);
		if (!wifi_rfkill)
			return -ENOMEM;
		rfkill_init_sw_state(wifi_rfkill,
				     hp_wmi_get_sw_state(HPWMI_WIFI));
		rfkill_set_hw_state(wifi_rfkill,
				    hp_wmi_get_hw_state(HPWMI_WIFI));
		err = rfkill_register(wifi_rfkill);
		if (err)
			goto register_wifi_error;
	}

	if (wireless & 0x2) {
		bluetooth_rfkill = rfkill_alloc("hp-bluetooth", &device->dev,
						RFKILL_TYPE_BLUETOOTH,
						&hp_wmi_rfkill_ops,
						(void *) HPWMI_BLUETOOTH);
		if (!bluetooth_rfkill) {
			err = -ENOMEM;
			goto register_bluetooth_error;
		}
		rfkill_init_sw_state(bluetooth_rfkill,
				     hp_wmi_get_sw_state(HPWMI_BLUETOOTH));
		rfkill_set_hw_state(bluetooth_rfkill,
				    hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		err = rfkill_register(bluetooth_rfkill);
		if (err)
			goto register_bluetooth_error;
	}

	if (wireless & 0x4) {
		wwan_rfkill = rfkill_alloc("hp-wwan", &device->dev,
					   RFKILL_TYPE_WWAN,
					   &hp_wmi_rfkill_ops,
					   (void *) HPWMI_WWAN);
		if (!wwan_rfkill) {
			err = -ENOMEM;
			goto register_wwan_error;
		}
		rfkill_init_sw_state(wwan_rfkill,
				     hp_wmi_get_sw_state(HPWMI_WWAN));
		rfkill_set_hw_state(wwan_rfkill,
				    hp_wmi_get_hw_state(HPWMI_WWAN));
		err = rfkill_register(wwan_rfkill);
		if (err)
			goto register_wwan_error;
	}

	return 0;

register_wwan_error:
	rfkill_destroy(wwan_rfkill);
	wwan_rfkill = NULL;
	if (bluetooth_rfkill)
		rfkill_unregister(bluetooth_rfkill);
register_bluetooth_error:
	rfkill_destroy(bluetooth_rfkill);
	bluetooth_rfkill = NULL;
	if (wifi_rfkill)
		rfkill_unregister(wifi_rfkill);
register_wifi_error:
	rfkill_destroy(wifi_rfkill);
	wifi_rfkill = NULL;
	return err;
}

static int __init hp_wmi_rfkill2_setup(struct platform_device *device)
{
	struct bios_rfkill2_state state;
	int err, i;

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   zero_if_sup(state), sizeof(state));
	if (err)
		return err < 0 ? err : -EINVAL;

	if (state.count > HPWMI_MAX_RFKILL2_DEVICES) {
		pr_warn("unable to parse 0x1b query output\n");
		return -EINVAL;
	}

	for (i = 0; i < state.count; i++) {
		struct rfkill *rfkill;
		enum rfkill_type type;
		char *name;

		switch (state.device[i].radio_type) {
		case HPWMI_WIFI:
			type = RFKILL_TYPE_WLAN;
			name = "hp-wifi";
			break;
		case HPWMI_BLUETOOTH:
			type = RFKILL_TYPE_BLUETOOTH;
			name = "hp-bluetooth";
			break;
		case HPWMI_WWAN:
			type = RFKILL_TYPE_WWAN;
			name = "hp-wwan";
			break;
		case HPWMI_GPS:
			type = RFKILL_TYPE_GPS;
			name = "hp-gps";
			break;
		default:
			pr_warn("unknown device type 0x%x\n",
				state.device[i].radio_type);
			continue;
		}

		if (!state.device[i].vendor_id) {
			pr_warn("zero device %d while %d reported\n",
				i, state.count);
			continue;
		}

		rfkill = rfkill_alloc(name, &device->dev, type,
				      &hp_wmi_rfkill2_ops, (void *)(long)i);
		if (!rfkill) {
			err = -ENOMEM;
			goto fail;
		}

		rfkill2[rfkill2_count].id = state.device[i].rfkill_id;
		rfkill2[rfkill2_count].num = i;
		rfkill2[rfkill2_count].rfkill = rfkill;

		rfkill_init_sw_state(rfkill,
				     IS_SWBLOCKED(state.device[i].power));
		rfkill_set_hw_state(rfkill,
				    IS_HWBLOCKED(state.device[i].power));

		if (!(state.device[i].power & HPWMI_POWER_BIOS))
			pr_info("device %s blocked by BIOS\n", name);

		err = rfkill_register(rfkill);
		if (err) {
			rfkill_destroy(rfkill);
			goto fail;
		}

		rfkill2_count++;
	}

	return 0;
fail:
	for (; rfkill2_count > 0; rfkill2_count--) {
		rfkill_unregister(rfkill2[rfkill2_count - 1].rfkill);
		rfkill_destroy(rfkill2[rfkill2_count - 1].rfkill);
	}
	return err;
}

static int platform_profile_omen_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	/*
	 * We directly return the stored platform profile, as the embedded
	 * controller will not accept switching to the performance option when
	 * the conditions are not met (e.g. the laptop is not plugged in).
	 *
	 * If we directly return what the EC reports, the platform profile will
	 * immediately "switch back" to normal mode, which is against the
	 * expected behaviour from a userspace point of view, as described in
	 * the Platform Profile Section page of the kernel documentation.
	 *
	 * See also omen_powersource_event.
	 */
	guard(mutex)(&active_platform_profile_lock);
	*profile = active_platform_profile;

	return 0;
}

static bool is_omen_v1_thermal_profile(void)
{
	const char *board_name;

	board_name = dmi_get_system_info(DMI_BOARD_NAME);
	if (!board_name)
		return false;

	return match_string(thermal_profile_v1_boards,
			    ARRAY_SIZE(thermal_profile_v1_boards),
			    board_name) >= 0;
}

static int omen_v1_gpu_thermal_profile_get(bool *ctgp_enable,
					    bool *ppab_enable,
					    u8 *dstate,
					    u8 *gpu_slowdown_temp)
{
	struct hp_gpu_power_modes gpu_power_modes;
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GET_GPU_THERMAL_MODES_QUERY, HPWMI_GM,
				   &gpu_power_modes, sizeof(gpu_power_modes),
				   sizeof(gpu_power_modes));
	if (ret == 0) {
		*ctgp_enable = gpu_power_modes.ctgp_enable ? true : false;
		*ppab_enable = gpu_power_modes.ppab_enable ? true : false;
		*dstate = gpu_power_modes.dstate;
		*gpu_slowdown_temp = gpu_power_modes.gpu_slowdown_temp;
	}

	return ret;
}

static int omen_v1_gpu_thermal_profile_set(bool ctgp_enable,
					    bool ppab_enable,
					    u8 dstate)
{
	struct hp_gpu_power_modes gpu_power_modes;
	int ret;

	bool current_ctgp_state, current_ppab_state;
	u8 current_dstate, current_gpu_slowdown_temp;

	/* Retrieving GPU slowdown temperature, in order to keep it unchanged */
	ret = omen_v1_gpu_thermal_profile_get(&current_ctgp_state,
					       &current_ppab_state,
					       &current_dstate,
					       &current_gpu_slowdown_temp);
	if (ret < 0) {
		pr_warn("GPU modes not updated, unable to get slowdown temp\n");
		return ret;
	}

	gpu_power_modes.ctgp_enable = ctgp_enable ? 0x01 : 0x00;
	gpu_power_modes.ppab_enable = ppab_enable ? 0x01 : 0x00;
	gpu_power_modes.dstate = dstate;
	gpu_power_modes.gpu_slowdown_temp = current_gpu_slowdown_temp;


	ret = hp_wmi_perform_query(HPWMI_SET_GPU_THERMAL_MODES_QUERY, HPWMI_GM,
				   &gpu_power_modes, sizeof(gpu_power_modes), 0);

	return ret;
}

/* Note: HP_POWER_LIMIT_DEFAULT can be used to restore default PL1 and PL2 */
static int omen_v1_set_cpu_pl1_pl2(u8 pl1, u8 pl2)
{
	struct hp_power_limits power_limits;
	int ret;

	/* We need to know both PL1 and PL2 values in order to check them */
	if (pl1 == HP_POWER_LIMIT_NO_CHANGE || pl2 == HP_POWER_LIMIT_NO_CHANGE)
		return -EINVAL;

	/* PL2 is not supposed to be lower than PL1 */
	if (pl2 < pl1)
		return -EINVAL;

	power_limits.pl1 = pl1;
	power_limits.pl2 = pl2;
	power_limits.pl4 = HP_POWER_LIMIT_NO_CHANGE;
	power_limits.cpu_gpu_concurrent_limit = HP_POWER_LIMIT_NO_CHANGE;

	ret = hp_wmi_perform_query(HPWMI_CPU_POWER_SET_QUERY, HPWMI_GM,
				   &power_limits, sizeof(power_limits), 0);

	return ret;
}

static int platform_profile_omen_v1_set_ec(enum platform_profile_option profile)
{
	bool gpu_ctgp_enable, gpu_ppab_enable;
	u8 gpu_dstate; /* Test shows 1 = 100%, 2 = 50%, 3 = 25%, 4 = 12.5% */
	int err;
	enum hp_fan_mode tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp = FANMODE_L7;
		gpu_ctgp_enable = true;
		gpu_ppab_enable = true;
		gpu_dstate = 1;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp = FANMODE_L2;
		gpu_ctgp_enable = false;
		gpu_ppab_enable = true;
		gpu_dstate = 1;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		tp = FANMODE_L2;
		gpu_ctgp_enable = false;
		gpu_ppab_enable = false;
		gpu_dstate = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = omen_thermal_profile_set(tp);
	if (err < 0) {
		pr_err("Failed to set platform profile %d: %d\n", profile, err);
		return err;
	}

	err = omen_v1_gpu_thermal_profile_set(gpu_ctgp_enable,
					       gpu_ppab_enable,
					       gpu_dstate);
	if (err < 0) {
		pr_err("Failed to set GPU profile %d: %d\n", profile, err);
		return err;
	}

	return 0;
}

static int platform_profile_omen_v1_set(struct device *dev,
					 enum platform_profile_option profile)
{
	int err;

	guard(mutex)(&active_platform_profile_lock);

	err = platform_profile_omen_v1_set_ec(profile);
	if (err < 0)
		return err;

	active_platform_profile = profile;

	return 0;
}

static int hp_wmi_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	if (is_omen_v1_thermal_profile()) {
		/* Adding an equivalent to HP Omen software ECO mode: */
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	} else {
		set_bit(PLATFORM_PROFILE_QUIET, choices);
		set_bit(PLATFORM_PROFILE_COOL, choices);
	}

	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int omen_v1_powersource_event(struct notifier_block *nb,
				      unsigned long value,
				      void *data)
{
	struct acpi_bus_event *event_entry = data;
	int err;

	if (strcmp(event_entry->device_class, ACPI_AC_CLASS) != 0)
		return NOTIFY_DONE;

	pr_debug("Received power source device event\n");

	/*
	 * Switching to battery power source while Performance mode is active
	 * needs manual triggering of CPU power limits. Same goes when switching
	 * to AC power source while Performance mode is active. Other modes
	 * however are automatically behaving without any manual action.
	 * Seen on OMEN v1 Boards.
	 */

	if (active_platform_profile == PLATFORM_PROFILE_PERFORMANCE) {
		pr_debug("Triggering CPU PL1/PL2 actualization\n");
		err = omen_v1_set_cpu_pl1_pl2(HP_POWER_LIMIT_DEFAULT,
					       HP_POWER_LIMIT_DEFAULT);
		if (err)
			pr_warn("Failed to actualize power limits: %d\n", err);

		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int omen_v1_register_powersource_event_handler(void)
{
	int err;

	platform_power_source_nb.notifier_call = omen_v1_powersource_event;
	err = register_acpi_notifier(&platform_power_source_nb);
	if (err < 0) {
		pr_warn("Failed to install ACPI power source notify handler\n");
		return err;
	}

	return 0;
}

static inline void omen_v1_unregister_powersource_event_handler(void)
{
	unregister_acpi_notifier(&platform_power_source_nb);
}

static const struct platform_profile_ops platform_profile_omen_v1_ops = {
	.probe = hp_wmi_platform_profile_probe,
	.profile_get = platform_profile_omen_get,
	.profile_set = platform_profile_omen_v1_set,
};

static int thermal_profile_setup(struct platform_device *device)
{
	const struct platform_profile_ops *ops;
	int err;
	if (is_omen_v1_thermal_profile()) {

		/*
		 * Being unable to retrieve laptop's current thermal profile,
		 * during this setup, we set it to Balanced by default.
		 */
		 
		active_platform_profile = PLATFORM_PROFILE_BALANCED;

		err = platform_profile_omen_v1_set_ec(active_platform_profile);
		if (err < 0)
			return err;

		ops = &platform_profile_omen_v1_ops;
	} 
	platform_profile_device = devm_platform_profile_register(&device->dev, "hp-wmi",
								 NULL, ops);
	if (IS_ERR(platform_profile_device))
		return PTR_ERR(platform_profile_device);

	pr_info("Registered as platform profile handler\n");
	platform_profile_support = true;

	return 0;
}

static int hp_wmi_hwmon_init(void);

static int __init hp_wmi_bios_setup(struct platform_device *device)
{
	int err;
	/* clear detected rfkill devices */
	wifi_rfkill = NULL;
	bluetooth_rfkill = NULL;
	wwan_rfkill = NULL;
	rfkill2_count = 0;

	/*
	 * In pre-2009 BIOS, command 1Bh return 0x4 to indicate that
	 * BIOS no longer controls the power for the wireless
	 * devices. All features supported by this command will no
	 * longer be supported.
	 */
	if (!hp_wmi_bios_2009_later()) {
		if (hp_wmi_rfkill_setup(device))
			hp_wmi_rfkill2_setup(device);
	}

	err = hp_wmi_hwmon_init();

	if (err < 0)
		return err;

	thermal_profile_setup(device);

	return 0;
}

static void __exit hp_wmi_bios_remove(struct platform_device *device)
{
	int i;

	for (i = 0; i < rfkill2_count; i++) {
		rfkill_unregister(rfkill2[i].rfkill);
		rfkill_destroy(rfkill2[i].rfkill);
	}

	if (wifi_rfkill) {
		rfkill_unregister(wifi_rfkill);
		rfkill_destroy(wifi_rfkill);
	}
	if (bluetooth_rfkill) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
	}
	if (wwan_rfkill) {
		rfkill_unregister(wwan_rfkill);
		rfkill_destroy(wwan_rfkill);
	}
}

static int hp_wmi_resume_handler(struct device *device)
{
	/*
	 * Hardware state may have changed while suspended, so trigger
	 * input events for the current state. As this is a switch,
	 * the input layer will only actually pass it on if the state
	 * changed.
	 */
	if (hp_wmi_input_dev) {
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK,
					    hp_wmi_get_dock_state());
		if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE,
					    hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);
	}

	if (rfkill2_count)
		hp_wmi_rfkill2_refresh();

	if (wifi_rfkill)
		rfkill_set_states(wifi_rfkill,
				  hp_wmi_get_sw_state(HPWMI_WIFI),
				  hp_wmi_get_hw_state(HPWMI_WIFI));
	if (bluetooth_rfkill)
		rfkill_set_states(bluetooth_rfkill,
				  hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
				  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
	if (wwan_rfkill)
		rfkill_set_states(wwan_rfkill,
				  hp_wmi_get_sw_state(HPWMI_WWAN),
				  hp_wmi_get_hw_state(HPWMI_WWAN));

	return 0;
}

static const struct dev_pm_ops hp_wmi_pm_ops = {
	.resume  = hp_wmi_resume_handler,
	.restore  = hp_wmi_resume_handler,
};

/*
 * hp_wmi_bios_remove() lives in .exit.text. For drivers registered via
 * module_platform_driver_probe() this is ok because they cannot get unbound at
 * runtime. So mark the driver struct with __refdata to prevent modpost
 * triggering a section mismatch warning.
 */
static struct platform_driver hp_wmi_driver __refdata = {
	.driver = {
		.name = "hp-wmi",
		.pm = &hp_wmi_pm_ops,
		.dev_groups = hp_wmi_groups,
	},
	.remove = __exit_p(hp_wmi_bios_remove),
};

static umode_t hp_wmi_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	switch (type) {
	case hwmon_pwm:
		return 0644;
	case hwmon_fan:
		if (hp_wmi_get_fan_speed(channel) >= 0)
			return 0444;
		break;
	default:
		return 0;
	}

	return 0;
}

static int hp_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	int ret;

	switch (type) {
	case hwmon_fan:
		ret = hp_wmi_get_fan_speed(channel);
		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	case hwmon_pwm:
		switch (hp_wmi_fan_speed_max_get()) {
		case 0:
			/* 0 is automatic fan, which is 2 for hwmon */
			*val = 2;
			return 0;
		case 1:
			/* 1 is max fan, which is 0
			 * (no fan speed control) for hwmon
			 */
			*val = 0;
			return 0;
		default:
			/* shouldn't happen */
			return -ENODATA;
		}
	default:
		return -EINVAL;
	}
}

static int hp_wmi_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	hp_wmi_get_fan_count();
	switch (type) {
	case hwmon_pwm:
		switch (val) {
		case 0:
			/* 0 is no fan speed control (max), which is 1 for us */
			return hp_wmi_fan_speed_max_set(1);
		case 2:
			/* 2 is automatic speed control, which is 0 for us */
			return hp_wmi_fan_speed_max_reset();
		default:
			/* we don't support manual fan speed control */
			return -EINVAL;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops ops = {
	.is_visible = hp_wmi_hwmon_is_visible,
	.read = hp_wmi_hwmon_read,
	.write = hp_wmi_hwmon_write,
};

static const struct hwmon_chip_info chip_info = {
	.ops = &ops,
	.info = info,
};

static int hp_wmi_hwmon_init(void)
{
	struct device *dev = &hp_wmi_platform_dev->dev;
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_info(dev, "hp", &hp_wmi_driver,
						     &chip_info, NULL);

	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register hp hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

static int __init hp_wmi_init(void)
{
	int event_capable = wmi_has_guid(HPWMI_EVENT_GUID);
	int bios_capable = wmi_has_guid(HPWMI_BIOS_GUID);
	int err, tmp = 0;

	if (!bios_capable && !event_capable)
		return -ENODEV;

	if (hp_wmi_perform_query(HPWMI_HARDWARE_QUERY, HPWMI_READ, &tmp,
				 sizeof(tmp), sizeof(tmp)) == HPWMI_RET_INVALID_PARAMETERS)
		zero_insize_support = true;

	if (event_capable) {
		err = hp_wmi_input_setup();
		if (err)
			return err;
	}

	if (bios_capable) {
		hp_wmi_platform_dev =
			platform_device_register_simple("hp-wmi", PLATFORM_DEVID_NONE, NULL, 0);
		if (IS_ERR(hp_wmi_platform_dev)) {
			err = PTR_ERR(hp_wmi_platform_dev);
			goto err_destroy_input;
		}

		err = platform_driver_probe(&hp_wmi_driver, hp_wmi_bios_setup);
		if (err)
			goto err_unregister_device;
	}

	if (is_omen_v1_thermal_profile()) {
			err = omen_v1_register_powersource_event_handler();
			if (err)
				goto err_unregister_device;
		}

	return 0;

err_unregister_device:
	platform_device_unregister(hp_wmi_platform_dev);
err_destroy_input:
	if (event_capable)
		hp_wmi_input_destroy();

	return err;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	if (is_omen_v1_thermal_profile()) 
		omen_v1_unregister_powersource_event_handler();

	if (wmi_has_guid(HPWMI_EVENT_GUID))
		hp_wmi_input_destroy();

	if (camera_shutter_input_dev)
		input_unregister_device(camera_shutter_input_dev);

	if (hp_wmi_platform_dev) {
		platform_device_unregister(hp_wmi_platform_dev);
		platform_driver_unregister(&hp_wmi_driver);
	}
}
module_exit(hp_wmi_exit);
