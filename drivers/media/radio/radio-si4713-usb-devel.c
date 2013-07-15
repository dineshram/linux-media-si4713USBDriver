/*
 * Copyright (c) 2013 Dinesh Ram <dinesh.ram@cern.ch> and Hans Verkuil
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
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
//#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <linux/mutex.h>

/* driver and module definitions */
MODULE_AUTHOR("Dinesh Ram <dinesh.ram@cern.ch> and Hans Verkuil<>");
MODULE_DESCRIPTION("Si4713 FM Transmitter usb driver");
MODULE_LICENSE("GPL v2");

/* The Device announces itself as Cygnal Integrated Products, Inc. */
#define USB_SI4713_VENDOR 0x10c4
#define USB_SI4713_PRODUCT 0x8244

/* Probably USB_TIMEOUT should be modified in module parameter */
#define BUFFER_LENGTH 64
#define USB_TIMEOUT 500

/* USB Device ID List */
static struct usb_device_id usb_si4713_device_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(USB_SI4713_VENDOR, USB_SI4713_PRODUCT,
							USB_CLASS_HID, 0, 0) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, usb_si4713_device_table);

struct si4713_device {
	struct usb_device *usbdev; /* the usb device for this device */
	struct usb_interface *intf;
	struct video_device vdev; /* the v4l device for this device */
	struct v4l2_device v4l2_dev;
	//struct v4l2_ctrl_handler hdl;
	struct mutex lock;

	u8 *buffer;
	unsigned curfreq;
	u8 tx;
	u8 pa;
	bool stereo;
	bool muted;
	bool preemph_75_us;
};

static inline struct si4713_device *to_si4713_dev(struct v4l2_device *v4l2_dev)
{
	return container_of(v4l2_dev, struct si4713_device, v4l2_dev);
}


static int vidioc_querycap(struct file *file, void *priv,
					struct v4l2_capability *v)
{
	struct si4713_device *radio = video_drvdata(file);

	strlcpy(v->driver, "radio-si4713-usb", sizeof(v->driver));
	strlcpy(v->card, "Si4713 FM Transmitter", sizeof(v->card));
	usb_make_path(radio->usbdev, v->bus_info, sizeof(v->bus_info));
	v->device_caps = V4L2_CAP_TUNER | V4L2_CAP_RADIO | V4L2_CAP_MODULATOR | V4L2_CAP_RDS_CAPTURE;
	v->capabilities = v->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int vidioc_g_modulator(struct file *file, void *priv,
				struct v4l2_modulator *v)
{
 /*TODO : To be implemented */
 return 0;
}

static int vidioc_s_modulator(struct file *file, void *priv,
				const struct v4l2_modulator *v)
{
 /*TODO : To be implemented */
 return 0;
}

static int vidioc_s_frequency(struct file *file, void *priv,
				const struct v4l2_frequency *f)
{
 /*TODO : To be implemented */
 return 0;
}

static int vidioc_g_frequency(struct file *file, void *priv,
				struct v4l2_frequency *f)
{
 /*TODO : To be implemented */
 return 0;
}

// static int vidioc_log_status(struct file *file, void *priv)
// {
//   /*TODO : To be implemented 
//     * Intended to be a debugging aid for video application writers.
//     * Should print information describing the current status of the driver and its hardware.
//     * Should be sufficiently verbose to help a confused application developer figure out why
//     the video display is coming up blank
//     * Should be moderated with a call to printk_ratelimit() to keep it from being used to slow 
//     the system and fill the logfiles with junk */
//   return 0;
// }



/* File system interface */
static const struct v4l2_file_operations usb_si4713_fops = {
	.owner		= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = v4l2_fh_release,
	//.poll		= v4l2_ctrl_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static const struct v4l2_ioctl_ops usb_si4713_ioctl_ops = {
	.vidioc_querycap    = vidioc_querycap,
	.vidioc_g_modulator = vidioc_g_modulator,
	.vidioc_s_modulator = vidioc_s_modulator,
	.vidioc_g_frequency = vidioc_g_frequency,
	.vidioc_s_frequency = vidioc_s_frequency,
	//.vidioc_log_status = v4l2_ctrl_log_status,
	//.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	//.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static void usb_si4713_video_device_release(struct v4l2_device *v4l2_dev)
{
	struct si4713_device *radio = to_si4713_dev(v4l2_dev);

	/* free rest memory */
	//v4l2_ctrl_handler_free(&radio->hdl);
	kfree(radio->buffer);
	kfree(radio);
}

/* check if the device is present and register with v4l and usb if it is */
static int usb_si4713_probe(struct usb_interface *intf,
				const struct usb_device_id *id)
{	
	struct usb_device *dev = interface_to_usbdev(intf); /* WARNING : unused variable */
	struct si4713_device *radio;
	//struct v4l2_ctrl_handler *hdl;
	int retval = 0;
	
	/*just for testing*/
	struct usb_host_interface *iface_desc;
	iface_desc = intf->cur_altsetting;
	printk(KERN_INFO "Si4713 development board i/f %d now probed: (%04X:%04X)\n",
            iface_desc->desc.bInterfaceNumber, id->idVendor, id->idProduct);
	printk(KERN_INFO "ID->bInterfaceClass: %02X\n",
            iface_desc->desc.bInterfaceClass);
	
	/* Initialize our local device structure */
	radio = kzalloc(sizeof(struct si4713_device), GFP_KERNEL);
	if (radio)
		radio->buffer = kmalloc(BUFFER_LENGTH, GFP_KERNEL);

	if (!radio || !radio->buffer) {
		dev_err(&intf->dev, "kmalloc for si4713_device failed\n");
		kfree(radio);
		retval = -ENOMEM;
		goto err;
	}
	
	//hdl = &radio->hdl;
	//v4l2_ctrl_handler_init(hdl, 4);
	/* TODO : some code to be written here */
	
	//if (hdl->error) {
	//	retval = hdl->error;
	//
	//	v4l2_ctrl_handler_free(hdl);
	//	goto err_v4l2;
	//}
	retval = v4l2_device_register(&intf->dev, &radio->v4l2_dev);
	if (retval < 0) {
		dev_err(&intf->dev, "couldn't register v4l2_device\n");
		goto err_v4l2;
	}

	mutex_init(&radio->lock);
	
	//radio->v4l2_dev.ctrl_handler = hdl;
	radio->v4l2_dev.release = usb_si4713_video_device_release;
	strlcpy(radio->vdev.name, radio->v4l2_dev.name,
		sizeof(radio->vdev.name));
	radio->vdev.v4l2_dev = &radio->v4l2_dev;
	radio->vdev.fops = &usb_si4713_fops;
	radio->vdev.ioctl_ops = &usb_si4713_ioctl_ops;
	radio->vdev.lock = &radio->lock;
	radio->vdev.release = video_device_release_empty;
	radio->vdev.vfl_dir = VFL_DIR_TX;

	radio->usbdev = interface_to_usbdev(intf);
	radio->intf = intf;
	usb_set_intfdata(intf, &radio->v4l2_dev);

	video_set_drvdata(&radio->vdev, radio);
	set_bit(V4L2_FL_USE_FH_PRIO, &radio->vdev.flags);

	retval = video_register_device(&radio->vdev, VFL_TYPE_RADIO, -1);
	if (retval < 0) {
		dev_err(&intf->dev, "could not register video device\n");
		goto err_vdev;
	}
	//v4l2_ctrl_handler_setup(hdl);
	dev_info(&intf->dev, "V4L2 device registered as %s\n",
			video_device_node_name(&radio->vdev));
	
	return 0;
	
err_vdev:
	printk(KERN_INFO "err_vdev");
	v4l2_device_unregister(&radio->v4l2_dev);
err_v4l2:
	printk(KERN_INFO "err_vdev");
	kfree(radio->buffer);
	kfree(radio);
err:
	printk(KERN_INFO "err");
	return retval;
}

/* Handle unplugging the device.
 * We call video_unregister_device in any case.
 * The last function called in this procedure is
 * usb_si4713_device_release.
 */

static void usb_si4713_disconnect(struct usb_interface *intf)
{
	printk(KERN_INFO "Si4713 development board i/f %d now disconnected\n",
            intf->cur_altsetting->desc.bInterfaceNumber);
	
	struct si4713_device *radio = to_si4713_dev(usb_get_intfdata(intf));

	mutex_lock(&radio->lock);
	usb_set_intfdata(intf, NULL);
	video_unregister_device(&radio->vdev);
	v4l2_device_disconnect(&radio->v4l2_dev);
	mutex_unlock(&radio->lock);
	v4l2_device_put(&radio->v4l2_dev);
}

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


/* 
 static struct i2c_adapter si4713_i2c_adapter_template = {
	.name   = "Si4713 I2C",
	.owner  = THIS_MODULE,
	.algo   = &si4713_algo,
};
 
 */

module_init(si4713_init);
module_exit(si4713_exit);