#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/wmi.h>
#include <linux/leds.h>

MODULE_AUTHOR("Matthew");
MODULE_DESCRIPTION("ASUS WMI Screenpad Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1");

#define ASUS_WMI_MGMT_GUID "97845ED0-4E6D-11DE-8A39-0800200C9A66"
#define ASUS_ACPI_UID_ASUSWMI "ASUSWMI"
#define ASUS_WMI_UNSUPPORTED_METHOD 0xFFFFFFFE
#define ASUS_WMI_METHODID_DCTS 0x53544344 /* Device status (DCTS) */
#define ASUS_WMI_METHODID_DSTS 0x53545344 /* Device status (DSTS) */
#define ASUS_WMI_METHODID_DEVS 0x53564544 /* DEVice Set */
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
    int screenpad_led_wk;
    struct workqueue_struct *workqueue;
    struct work_struct screenpad_work;
    struct led_classdev screenpad_led;
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

static int screenpad_led_read(struct asus_wmi_screenpad *asus_wmi_screenpad, int *level)
{
    u32 value, retval;
    retval = asus_wmi_get_devstate(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD, &value);
    if (retval == 0 && (value & 0x21) != 0) {
        // screen is activated, so read backlight
        retval = asus_wmi_get_devstate(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD_LIGHT, &value);
        if (retval == 0) {
            *level = value & ASUS_WMI_DSTS_BRIGHTNESS_MASK;
        }
    } else {
        *level = 0;
    }

    if (retval < 0) return retval;
    return 0;
}

static void screenpad_led_update(struct work_struct *work)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad =
        container_of(work, struct asus_wmi_screenpad, screenpad_work);
    int ctrl_param;

    ctrl_param = asus_wmi_screenpad->screenpad_led_wk;
    if (ctrl_param == 0x00) {
        // turn off screen
        asus_wmi_set_devstate(ASUS_WMI_DEVID_SCREENPAD, ctrl_param, NULL);
    } else {
        // set backlight (also turns on screen if is off)
        asus_wmi_set_devstate(ASUS_WMI_DEVID_SCREENPAD_LIGHT, ctrl_param, NULL);
    }
}

static void screenpad_led_set(struct led_classdev *led_cdev, enum led_brightness value)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad =
        container_of(led_cdev, struct asus_wmi_screenpad, screenpad_led);

    asus_wmi_screenpad->screenpad_led_wk = value;
    queue_work(asus_wmi_screenpad->workqueue, &asus_wmi_screenpad->screenpad_work);
}

static enum led_brightness screenpad_led_get(struct led_classdev *led_cdev)
{
    struct asus_wmi_screenpad *asus_wmi_screenpad;
    int retval, value;

    asus_wmi_screenpad = container_of(led_cdev, struct asus_wmi_screenpad, screenpad_led);

    retval = screenpad_led_read(asus_wmi_screenpad, &value);

    if (retval < 0) return retval;
    return value;
}

static int configure_setup(struct asus_wmi_screenpad *asus_wmi_screenpad)
{
    screenpad_led_read(asus_wmi_screenpad, &asus_wmi_screenpad->screenpad_led_wk);

    INIT_WORK(&asus_wmi_screenpad->screenpad_work, screenpad_led_update);

    asus_wmi_screenpad->workqueue = create_singlethread_workqueue("screenpad_workqueue");

    asus_wmi_screenpad->screenpad_led.name = "asus::screenpad";
    asus_wmi_screenpad->screenpad_led.brightness_set = screenpad_led_set;
    asus_wmi_screenpad->screenpad_led.brightness_get = screenpad_led_get;
    asus_wmi_screenpad->screenpad_led.max_brightness = 0xff;

    devm_led_classdev_register(&asus_wmi_screenpad->platform_device->dev, &asus_wmi_screenpad->screenpad_led);

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
    is_screenpad_present = asus_wmi_dev_is_present(asus_wmi_screenpad, ASUS_WMI_DEVID_SCREENPAD);
    if (!is_screenpad_present) return -ENODEV;

    if (!strcmp(wmi_uid, ASUS_ACPI_UID_ASUSWMI)) {
        dev_info(&pdev->dev, "Detected ASUSWMI, use DCTS\n");
        asus_wmi_screenpad->dsts_id = ASUS_WMI_METHODID_DCTS;
    } else {
        dev_info(&pdev->dev, "Detected %s, not ASUSWMI, use DSTS\n", wmi_uid);
        asus_wmi_screenpad->dsts_id = ASUS_WMI_METHODID_DSTS;
    }

    asus_wmi_screenpad->platform_device = pdev;
    asus_wmi_screenpad->platform_driver = asus_wmi_screenpad_platform_driver;

    platform_set_drvdata(asus_wmi_screenpad->platform_device, asus_wmi_screenpad);

    configure_setup(asus_wmi_screenpad);

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
