/*
 * hid_report_descriptor.h
 *
 * Standard USB HID gamepad report descriptor.
 *
 * Features:
 *   - 24 buttons (3-byte bitmap, Nintendo-native + Home/Capture/GL/GR/C)
 *   - 4 analog axes: X, Y, Z, Rz (16-bit each, range 0..4095)
 *   - Hat switch (d-pad, 4 bits)
 *   - Rumble output (2 bytes, left/right motor)
 *
 * Input Report (Report ID 1) - 13 bytes total:
 *   Byte 0:    Report ID (0x01)
 *   Byte 1-3:  Button bitmap (24 buttons, little-endian)
 *   Byte 4-5:  X axis  (16-bit LE)
 *   Byte 6-7:  Y axis  (16-bit LE)
 *   Byte 8-9:  Z axis  (16-bit LE)
 *   Byte 10-11: Rz axis (16-bit LE)
 *   Byte 12:   Bits 0-3: Hat Switch, Bits 4-7: Padding
 *
 * Output Report (Report ID 2) - 3 bytes total:
 *   Byte 0:    Report ID (0x02)
 *   Byte 1:    Left rumble motor  (0=off, 255=max)
 *   Byte 2:    Right rumble motor (0=off, 255=max)
 *
 * References:
 *   - HID Usage Tables 1.12 (USB-IF)
 *   - Linux drivers/hid/hid-sony.c (motion_rdesc, ps3remote_rdesc)
 *   - Linux drivers/hid/hid-nintendo.c
 *   - Arduino Joystick Library (MHeironimus/ArduinoJoystickLibrary)
 *   - https://eleccelerator.com/tutorial-about-usb-hid-report-descriptors/
 */

#ifndef HID_REPORT_DESCRIPTOR_H
#define HID_REPORT_DESCRIPTOR_H

#include <stdint.h>

/*
 * Raw USB HID report descriptor bytes.
 *
 * This descriptor defines a single Application Collection containing:
 *   - Input report  (Report ID 1): buttons + axes + hat switch
 *   - Output report (Report ID 2): dual rumble motors
 */
static const uint8_t hid_report_descriptor[] = {
    /* ------------------------------------------------------------------
     * Application Collection: Game Pad
     * ------------------------------------------------------------------ */
    0x05, 0x01,        /* Usage Page (Generic Desktop)                 */
    0x09, 0x05,        /* Usage (Game Pad)                             */
    0xA1, 0x01,        /* Collection (Application)                     */

    /* ------------------------------------------------------------------
     * Input Report: Report ID 1
     * ------------------------------------------------------------------ */
    0x85, 0x01,        /*   Report ID (1)                              */

    /* --- 24 buttons as a 3-byte bitmap --- */
    0x05, 0x09,        /*   Usage Page (Button)                        */
    0x19, 0x01,        /*   Usage Minimum (Button 1)                   */
    0x29, 0x18,        /*   Usage Maximum (Button 24)                  */
    0x15, 0x00,        /*   Logical Minimum (0)                        */
    0x25, 0x01,        /*   Logical Maximum (1)                        */
    0x75, 0x01,        /*   Report Size (1 bit)                        */
    0x95, 0x18,        /*   Report Count (24)                          */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute)           */

    /* --- 4 analog axes: X, Y, Z, Rz (16-bit, 0..4095) --- */
    0x05, 0x01,        /*   Usage Page (Generic Desktop)               */
    0x09, 0x30,        /*   Usage (X)                                  */
    0x09, 0x31,        /*   Usage (Y)                                  */
    0x09, 0x32,        /*   Usage (Z)                                  */
    0x09, 0x35,        /*   Usage (Rz)                                 */
    0x15, 0x00,        /*   Logical Minimum (0)                        */
    0x26, 0xFF, 0x0F,  /*   Logical Maximum (4095)                     */
    0x35, 0x00,        /*   Physical Minimum (0)                       */
    0x46, 0xFF, 0x0F,  /*   Physical Maximum (4095)                    */
    0x75, 0x10,        /*   Report Size (16 bits)                      */
    0x95, 0x04,        /*   Report Count (4)                           */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute)           */

    /* --- Hat switch (d-pad) --- */
    0x05, 0x01,        /*   Usage Page (Generic Desktop)               */
    0x09, 0x39,        /*   Usage (Hat Switch)                         */
    0x15, 0x00,        /*   Logical Minimum (0)                        */
    0x25, 0x07,        /*   Logical Maximum (7)                        */
    0x35, 0x00,        /*   Physical Minimum (0)                       */
    0x46, 0x3B, 0x01,  /*   Physical Maximum (315 degrees)             */
    0x75, 0x04,        /*   Report Size (4 bits)                       */
    0x95, 0x01,        /*   Report Count (1)                           */
    0x81, 0x42,        /*   Input (Data, Var, Abs, Null State)         */
                        /*   Null State: value 8 = center (no direction) */

    /* --- 4 bits padding (align to byte boundary) --- */
    0x75, 0x04,        /*   Report Size (4 bits)                       */
    0x95, 0x01,        /*   Report Count (1)                           */
    0x81, 0x03,        /*   Input (Constant, Variable, Absolute)       */

    /* ------------------------------------------------------------------
     * Output Report: Report ID 2 (Rumble — Physical Page for FF)
     * Usage Page Physical (0x0F) signals force-feedback capable device.
     * Games using direct HID output will trigger UHID_OUTPUT → daemon → GATT.
     * Full evdev FF ioctl (EVIOCSFF) needs a kernel module — Phase 2.
     * ------------------------------------------------------------------ */
    0x85, 0x02,        /*   Report ID (2)                              */
    0x05, 0x0F,        /*   Usage Page (Physical Interface)            */
    0x09, 0x21,        /*   Usage (0x21 — Set Effect Report)          */
    0x15, 0x00,        /*   Logical Minimum (0)                        */
    0x25, 0xFF,        /*   Logical Maximum (255)                      */
    0x75, 0x08,        /*   Report Size (8 bits)                       */
    0x95, 0x02,        /*   Report Count (2)                           */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute)          */

    /* ------------------------------------------------------------------
     * End Collection
     * ------------------------------------------------------------------ */
    0xC0               /* End Collection                               */
};

#define HID_REPORT_DESC_SIZE (sizeof(hid_report_descriptor) / sizeof(hid_report_descriptor[0]))

/*
 * Report sizes (excluding the 1-byte Report ID):
 *
 *   HID_INPUT_REPORT_SIZE   = 11  (buttons + axes + hat + padding)
 *   HID_OUTPUT_REPORT_SIZE  =  2  (left motor + right motor)
 */
#define HID_INPUT_REPORT_SIZE   13   /* report ID (1) + buttons (3) + axes (8) + hat+pad (1) */
#define HID_OUTPUT_REPORT_SIZE   3    /* report ID (1) + motors (2) */

#endif /* HID_REPORT_DESCRIPTOR_H */
