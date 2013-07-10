/*
 * Copyright (c) 2013 Dinesh Ram <dinesh.ram@cern.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* kernel includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
/*
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <linux/mutex.h> */

/* driver and module definitions */
MODULE_AUTHOR("Dinesh Ram <dinesh.ram@cern.ch>");
MODULE_DESCRIPTION("Si4713 FM Transmitter usb driver");
MODULE_LICENSE("GPL");

/* The Device announces itself as Cygnal Integrated Products, Inc. */
#define USB_SI4713_VENDOR 0x10c4
#define USB_SI4713_PRODUCT 0x8244

/* USB Device ID List */
static struct usb_device_id usb_si4713_device_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(USB_SI4713_VENDOR, USB_SI4713_PRODUCT,
							USB_CLASS_HID, 0, 0) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, usb_si4713_device_table);

/* USB subsystem interface */
static struct usb_driver usb_si4713_driver = {
	.name			= "radio-si4713-usb",
	.probe			= usb_si4713_probe,
	.disconnect		= usb_si4713_disconnect,
	.id_table		= usb_si4713_device_table,
	/*.suspend		= usb_si4713_suspend,
	.resume			= usb_si4713_resume,
	.reset_resume		= usb_si4713_resume,*/
};

/* check if the device is present and register with v4l and usb if it is */
static int usb_si4713_probe(struct usb_interface *intf,
				const struct usb_device_id *id)
{
	
	struct usb_device *dev = interface_to_usbdev(intf);
	struct si4713_device *radio;
	int retval = 0;
	
	/*just for testing*/
	struct usb_host_interface *iface_desc;
	iface_desc = interface->cur_altsetting;
	printk(KERN_INFO "Si4713 development board i/f %d now probed: (%04X:%04X)\n",
            iface_desc->desc.bInterfaceNumber, id->idVendor, id->idProduct);
	printk(KERN_INFO "ID->bInterfaceClass: %02X\n",
            iface_desc->desc.bInterfaceClass);
	
	/* Initialize our local device structure */
	radio = kzalloc(sizeof(struct si4713_device), GFP_KERNEL);
	//memset (radio, 0x00, sizeof (*radio)); /* is it necessary? */
	if (radio)
		radio->buffer = kmalloc(BUFFER_LENGTH, GFP_KERNEL);

	if (!radio || !radio->buffer) {
		dev_err(&intf->dev, "kmalloc for si4713_device failed\n");
		kfree(radio);
		retval = -ENOMEM;
		goto err;
	}
	
	radio->usbdev = interface_to_usbdev(intf);
	return 0;
}

static void usb_si4713_disconnect(struct usb_interface *intf)
{
	printk(KERN_INFO "Si4713 development board i/f %d now disconnected\n",
            interface->cur_altsetting->desc.bInterfaceNumber);
}

static int __init si4713_init(void)
{
	int retval = usb_register(&usb_si4713_driver);

	if (retval)
		pr_err(KBUILD_MODNAME
			": usb_register failed. Error number %d\n", retval);

	return retval;
}

static void __exit si4713_exit(void)
{
	usb_deregister(&usb_si4713_driver);
}


module_init(si4713_init);
module_exit(si4713_exit);

MODULE_LICENSE("GPL")
MODULE_AUTHOR("Dinesh Ram")
MODULE_DESCRIPTION("USB Device Driver for Si4713 development board")



