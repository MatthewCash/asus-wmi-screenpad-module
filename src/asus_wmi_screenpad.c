#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/wmi.h>
#include <linux/leds.h>
#include <linux/backlight.h>

MODULE_AUTHOR("Matthew");
MODULE_DESCRIPTION("ASUS WMI Screenpad Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1");

static bool enable_led_dev = true;
static bool enable_bl_dev = false; // Defaults to false because some DEs only expect one backlight dev

module_param(enable_led_dev, bool, 0644);
MODULE_PARM_DESC(enable_led_dev, "Whether to register an LED class device");
module_param(enable_bl_dev, bool, 0644);
MODULE_PARM_DESC(enable_bl_dev, "Whether to register a backlight device");

#define ASUS_WMI_MGMT_GUID "97845ED0-4E6D-11DE-8A39-0800200C9A66"
#define ASUS_ACPI_UID_ASUSWMI "ASUSWMI"
#define ASUS_WMI_UNSUPPORTED_METHOD 0xFFFFFFFE
#define ASUS_WMI_METHODID_DCTS 0x53544344 /* Device status (DCTS) */
#define ASUS_WMI_METHODID_DSTS 0x53545344 /* Device status (DSTS) */
#define ASUS_WMI_METHODID_DEVS 0x53564544 /* DEVice Set */
#define ASUS_WMI_DSTS_PRESENCE_BIT 0x00010000
#define ASUS_WMI_DSTS_BRIGHTNESS_MASK 0x000000FF
#define ASUS_WMI_DEVID_SCREENPAD 0x00050031
#define ASUS_WMI_DEVID_SCREENPAD_LIGHT 0x00050032

struct bios_args {
    u32 arg0;
    u32 arg1;
    u32 arg2; /* At least TUF Gaming series uses 3 dword input buffer. */
    u32 arg3;
    u32 arg4; /* Some ROG laptops require a full 5 input args */
    u32 arg5;
} __packed;

struct asus_wmi_screenpad {
    struct platform_driver platform_driver;
    struct platform_device *platform_device;

    int dsts_id;

    bool screenpad_power_wk;
    int screenpad_brightness_wk;

    struct workqueue_struct *power_workqueue;
    struct workqueue_struct *brightness_workqueue;
    struct work_struct screenpad_power_work;
    struct work_struct screenpad_brightness_work;

    struct led_classdev screenpad_led;
    struct backlight_device *screenpad_backlight;
};

static int asus_wmi_evaluate_method(u32 method_id, u32 arg0, u32 arg1, u32 arg2, u32 *retval)
{
    struct bios_args args = {
        .arg0 = arg0,
        .arg1 = arg1,
        .arg2 = arg2,
    };
    struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
    struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status status;
    union acpi_object *obj;
    u32 tmp = 0;

    status = wmi_evaluate_method(ASUS_WMI_MGMT_GUID, 0, method_id, &input, &output);

    if (ACPI_FAILURE(status))
        return -EIO;

    obj = (union acpi_object *) output.pointer;
    if (obj && obj->type == ACPI_TYPE_INTEGER)
        tmp = (u32) obj->integer.value;

    if (retval)
        *retval = tmp;

    kfree(obj);

    if (tmp == ASUS_WMI_UNSUPPORTED_METHOD)
        return -ENODEV;

    return 0;
}

static int asus_wmi_get_devstate(struct asus_wmi_screenpad *asus_wmi_screenpad, u32 dev_id, u32 *retval)
{
    return asus_wmi_evaluate_method(asus_wmi_screenpad->dsts_id, dev_id, 0, 0, retval);
}

static bool asus_wmi_dev_is_present(struct asus_wmi_screenpad *asus_wmi_screenpad, u32 dev_id)
{
	u32 retval;
	int status = asus_wmi_get_devstate(asus_wmi_screenpad, dev_id, &retval);

	return status == 0 && (retval & ASUS_WMI_DSTS_PRESENCE_BIT);
}

static int asus_wmi_set_devstate(u32 dev_id, u32 ctrl_param, u32 *retval)
{
    return asus_wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, dev_id, ctrl_param, 0, retval);
}

static int screenpad_read_power(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    u32 power, retval;
    retval = asus_wmi_get_devstate(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD_LIGHT, &power);

    if (retval < 0) return retval;
    return power;
}

static int screenpad_set_power(struct asus_wmi_screenpad *asus_wmi_screenpad, bool power)
{
    int retval = asus_wmi_set_devstate(ASUS_WMI_DEVID_SCREENPAD, power, NULL);
    if (enable_bl_dev && retval == 0) asus_wmi_screenpad->screenpad_backlight->props.power = power;
    return retval;
}

static void screenpad_queue_power(struct asus_wmi_screenpad *asus_wmi_screenpad, bool power)
{
    asus_wmi_screenpad->screenpad_power_wk = power;
    queue_work(asus_wmi_screenpad->power_workqueue, &asus_wmi_screenpad->screenpad_power_work);
}

static int screenpad_read_brightness(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    u32 brightness, retval;
    retval = asus_wmi_get_devstate(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD_LIGHT, &brightness);

    if (retval < 0) return retval;
    return brightness & ASUS_WMI_DSTS_BRIGHTNESS_MASK;
}

static int screenpad_set_brightness(struct asus_wmi_screenpad *asus_wmi_screenpad, u32 brightness)
{
    int retval = asus_wmi_set_devstate(ASUS_WMI_DEVID_SCREENPAD_LIGHT, brightness, NULL);
    if (enable_bl_dev && retval == 0) asus_wmi_screenpad->screenpad_backlight->props.brightness = brightness;
    return retval;
}

static void screenpad_queue_brightness(struct asus_wmi_screenpad *asus_wmi_screenpad, int brightness)
{
    asus_wmi_screenpad->screenpad_brightness_wk = brightness;
    queue_work(asus_wmi_screenpad->brightness_workqueue, &asus_wmi_screenpad->screenpad_brightness_work);
}

static int screenpad_led_read(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    u32 power = screenpad_read_power(asus_wmi_screenpad);
    if (power < 0) return power;
    if (power & 28) {
        u32 brightness = screenpad_read_brightness(asus_wmi_screenpad);
        return brightness;
    } else {
        return 0;
    }
}

static void screenpad_power_update(struct work_struct *work)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad =
        container_of(work, struct asus_wmi_screenpad, screenpad_power_work);

    int power = asus_wmi_screenpad->screenpad_power_wk;

    screenpad_set_power(asus_wmi_screenpad, power);
}

static void screenpad_brightness_update(struct work_struct *work)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad =
        container_of(work, struct asus_wmi_screenpad, screenpad_brightness_work);

    int brightness = asus_wmi_screenpad->screenpad_brightness_wk;

    screenpad_set_brightness(asus_wmi_screenpad, brightness);
}

static void screenpad_led_set(struct led_classdev *led_cdev, enum led_brightness value)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad =
        container_of(led_cdev, struct asus_wmi_screenpad, screenpad_led);

    if (value > 0) {
        screenpad_queue_power(asus_wmi_screenpad, true); // This may be unnecessary
        screenpad_queue_brightness(asus_wmi_screenpad, value);
    } else {
        screenpad_queue_power(asus_wmi_screenpad, false);
    }
}

static int screenpad_backlight_update(struct backlight_device *bdev)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad = bl_get_data(bdev);

    // Power
    {
        int power = screenpad_read_power(asus_wmi_screenpad);
        if (power < 0) return power;

        if (power != bdev->props.power) {
        screenpad_queue_power(asus_wmi_screenpad, bdev->props.power);
        }
    }

    // Brightness
    {
        int brightness = screenpad_read_brightness(asus_wmi_screenpad);
        if (brightness < 0) return brightness;

        if (brightness != bdev->props.brightness) {
            screenpad_queue_brightness(asus_wmi_screenpad, bdev->props.brightness);
        }
    }

    return 0;
}

static enum led_brightness screenpad_led_get(struct led_classdev *led_cdev)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad;

    asus_wmi_screenpad = container_of(led_cdev, struct asus_wmi_screenpad, screenpad_led);

    return screenpad_led_read(asus_wmi_screenpad);
}

static int screenpad_backlight_get_brightness(struct backlight_device *bdev)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad = bl_get_data(bdev);

    return screenpad_read_brightness(asus_wmi_screenpad);
}

static const struct backlight_ops screenpad_bl_ops = {
	.get_brightness = screenpad_backlight_get_brightness,
	.update_status = screenpad_backlight_update,
};

static int create_led_dev(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    asus_wmi_screenpad->screenpad_led.name = "asus::screenpad";
    asus_wmi_screenpad->screenpad_led.brightness_set = screenpad_led_set;
    asus_wmi_screenpad->screenpad_led.brightness_get = screenpad_led_get;
    asus_wmi_screenpad->screenpad_led.max_brightness = 0xff;

    return devm_led_classdev_register(&asus_wmi_screenpad->platform_device->dev, &asus_wmi_screenpad->screenpad_led);
}

static int create_bl_dev(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    struct backlight_properties props;

    memset(&props, 0, sizeof(struct backlight_properties));

    props.type = BACKLIGHT_PLATFORM;
    props.max_brightness = 0xff;

    struct backlight_device *bl_dev = devm_backlight_device_register(&asus_wmi_screenpad->platform_device->dev, "screenpad_backlight", &asus_wmi_screenpad->platform_device->dev, asus_wmi_screenpad, &screenpad_bl_ops, &props);

    if (IS_ERR(bl_dev)) {
        return PTR_ERR(bl_dev);
    }

    asus_wmi_screenpad->screenpad_backlight = bl_dev;

    return 0;
}

static int setup_devs(struct asus_wmi_screenpad *asus_wmi_screenpad)
{

    INIT_WORK(&asus_wmi_screenpad->screenpad_power_work, screenpad_power_update);
    INIT_WORK(&asus_wmi_screenpad->screenpad_brightness_work, screenpad_brightness_update);

    asus_wmi_screenpad->power_workqueue = create_singlethread_workqueue("screenpad_power_workqueue");
    asus_wmi_screenpad->brightness_workqueue = create_singlethread_workqueue("screenpad_brightness_workqueue");

    if (enable_led_dev) {
        int res = create_led_dev(asus_wmi_screenpad);
        if (res != 0) return res;
    }

    if (enable_bl_dev) {
        int res = create_bl_dev(asus_wmi_screenpad);
        if (res != 0) return res;
    }

    return 0;
}

static int asus_wmi_screenpad_probe(struct platform_device *pdev);

static struct platform_device *asus_wmi_screenpad_platform_device;

static struct platform_driver asus_wmi_screenpad_platform_driver = {
    .driver = {
        .name = "asus-wmi-screenpad",
    },
    .probe = asus_wmi_screenpad_probe,
};

static int asus_wmi_screenpad_probe(struct platform_device *pdev)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad;
    char *wmi_uid;
    bool is_screenpad_present;

    asus_wmi_screenpad = devm_kzalloc(&pdev->dev, sizeof(struct asus_wmi_screenpad), GFP_KERNEL);
    if (!asus_wmi_screenpad) return -ENOMEM;

    wmi_uid = wmi_get_acpi_device_uid(ASUS_WMI_MGMT_GUID);
    if (!wmi_uid) return -ENODEV;

    if (!strcmp(wmi_uid, ASUS_ACPI_UID_ASUSWMI)) {
        dev_info(&pdev->dev, "Detected ASUSWMI, use DCTS\n");
        asus_wmi_screenpad->dsts_id = ASUS_WMI_METHODID_DCTS;
    } else {
        dev_info(&pdev->dev, "Detected %s, not ASUSWMI, use DSTS\n", wmi_uid);
        asus_wmi_screenpad->dsts_id = ASUS_WMI_METHODID_DSTS;
    }

    is_screenpad_present = asus_wmi_dev_is_present(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD);
    if (!is_screenpad_present) return -ENODEV;

    asus_wmi_screenpad->platform_device = pdev;
    asus_wmi_screenpad->platform_driver = asus_wmi_screenpad_platform_driver;

    platform_set_drvdata(asus_wmi_screenpad->platform_device, asus_wmi_screenpad);

    setup_devs(asus_wmi_screenpad);

    return 0;
}

static int __init asus_wmi_screenpad_init(void)
{
    asus_wmi_screenpad_platform_device =
        platform_create_bundle(&asus_wmi_screenpad_platform_driver, asus_wmi_screenpad_probe, NULL, 0, NULL, 0);

    if (IS_ERR(asus_wmi_screenpad_platform_device))
        return PTR_ERR(asus_wmi_screenpad_platform_device);

    pr_info("ASUS WMI Screenpad driver loaded\n");

    return 0;
}

static void __exit asus_wmi_screenpad_exit(void)
{
    platform_device_unregister(asus_wmi_screenpad_platform_device);
    platform_driver_unregister(&asus_wmi_screenpad_platform_driver);

    pr_info("ASUS WMI Screenpad driver unloaded\n");
}

module_init(asus_wmi_screenpad_init);
module_exit(asus_wmi_screenpad_exit);
