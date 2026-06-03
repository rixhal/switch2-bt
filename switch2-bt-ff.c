// SPDX-License-Identifier: GPL-2.0
/*
 * Force feedback support for Nintendo Switch 2 Pro Controller via UHID/Bluetooth
 *
 * Registers FF_RUMBLE on the HID gamepad device. When a game sends an evdev
 * FF ioctl (EVIOCSFF), this driver translates it to the HID output report
 * that the switch2-bt UHID daemon forwards to the controller via GATT.
 *
 * Output Report format (Report ID 2): [0x02, left_motor, right_motor]
 *   left_motor:  strong rumble motor (0-255)
 *   right_motor: weak rumble motor (0-255)
 *
 * Based on hid-emsff.c and hid-zpff.c patterns.
 */
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>

struct switch2bt_ff {
	struct hid_report *report;
};

static int switch2bt_ff_play(struct input_dev *dev, void *data,
			      struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct switch2bt_ff *ff = data;
	u8 left, right;

	left  = effect->u.rumble.strong_magnitude >> 8;
	right = effect->u.rumble.weak_magnitude >> 8;

	ff->report->field[0]->value[0] = 0x02; /* Report ID */
	ff->report->field[0]->value[1] = left;
	ff->report->field[0]->value[2] = right;

	hid_hw_output_report(hid, (u8 *)ff->report->field[0]->value,
			     ff->report->field[0]->report_count);

	return 0;
}

static int switch2bt_ff_init(struct hid_device *hid)
{
	struct switch2bt_ff *ff;
	struct hid_report *report;
	struct hid_input *hidinput;
	struct input_dev *dev;
	int error;

	if (list_empty(&hid->inputs)) {
		hid_err(hid, "no inputs found\n");
		return -ENODEV;
	}
	hidinput = list_first_entry(&hid->inputs, struct hid_input, list);
	dev = hidinput->input;

	/* Find Output Report ID 2 (rumble) */
	report = hid_validate_values(hid, HID_OUTPUT_REPORT, 2, 0, 3);
	if (!report) {
		hid_warn(hid, "no output report ID 2 with 3 values found — "
			 "rumble will not work\n");
		return 0; /* Non-fatal: device still works without FF */
	}

	ff = kzalloc(sizeof(*ff), GFP_KERNEL);
	if (!ff)
		return -ENOMEM;

	set_bit(FF_RUMBLE, dev->ffbit);

	error = input_ff_create_memless(dev, ff, switch2bt_ff_play);
	if (error) {
		kfree(ff);
		return error;
	}

	ff->report = report;
	hid_info(hid, "force feedback (FF_RUMBLE) registered for Switch 2 Pro Controller\n");

	return 0;
}

static int switch2bt_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	int ret;

	/* Let hid-generic handle input, we only add FF */
	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	ret = switch2bt_ff_init(hdev);
	if (ret) {
		hid_err(hdev, "force feedback init failed\n");
		hid_hw_stop(hdev);
		return ret;
	}

	return 0;
}

static void switch2bt_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

static const struct hid_device_id switch2bt_devices[] = {
	/* Switch 2 Pro Controller via Bluetooth (UHID bus = BUS_BLUETOOTH) */
	{ HID_BLUETOOTH_DEVICE(0x057E, 0x2060) },
	{ }
};
MODULE_DEVICE_TABLE(hid, switch2bt_devices);

static struct hid_driver switch2bt_driver = {
	.name = "switch2-bt-ff",
	.id_table = switch2bt_devices,
	.probe = switch2bt_probe,
	.remove = switch2bt_remove,
};
module_hid_driver(switch2bt_driver);

MODULE_DESCRIPTION("Force feedback for Switch 2 Pro Controller (BT/UHID)");
MODULE_LICENSE("GPL");
