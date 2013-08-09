/*
 * Copyright 2013 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 * 
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* kernel includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
/* V4l includes */
#include <linux/videodev2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>

#include "si4713-i2c.h"

/* driver and module definitions */
MODULE_AUTHOR("Dinesh Ram <dinram@cisco.com>");
MODULE_DESCRIPTION("Si4713 FM Transmitter USB driver");
MODULE_LICENSE("GPL v2");

/* The Device announces itself as Cygnal Integrated Products, Inc. */
#define USB_SI4713_VENDOR		0x10c4 
#define USB_SI4713_PRODUCT		0x8244

#define BUFFER_LENGTH			64
#define USB_TIMEOUT			1000

/* The SI4713 I2C sensor chip has a fixed slave address of 0xc6 or 0x22. */
#define SI4713_I2C_ADDR_BUSEN_HIGH      0x63
#define SI4713_I2C_ADDR_BUSEN_LOW       0x11

#define SLEEP				3
#define TIMEOUT				15

/* USB Device ID List */
static struct usb_device_id usb_si4713_usb_device_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(USB_SI4713_VENDOR, USB_SI4713_PRODUCT,
							USB_CLASS_HID, 0, 0) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, usb_si4713_usb_device_table);

struct si4713_usb_device {
	struct usb_device 	*usbdev;	/* the usb device for this device */
	struct usb_interface 	*intf;
	struct video_device 	vdev;		/* the v4l device for this device */
	struct v4l2_device 	v4l2_dev;
	struct v4l2_subdev	*v4l2_subdev;
	struct mutex 		lock;
	struct i2c_adapter 	i2c_adapter;	/* I2C adapter */
	
	u8 			*buffer;
};

static inline struct si4713_usb_device *to_si4713_dev(struct v4l2_device *v4l2_dev)
{
	return container_of(v4l2_dev, struct si4713_usb_device, v4l2_dev);
}

static int vidioc_querycap(struct file *file, void *priv,
					struct v4l2_capability *v)
{
	struct si4713_usb_device *radio = video_drvdata(file);

	strlcpy(v->driver, "radio-si4713-usb", sizeof(v->driver));
	strlcpy(v->card, "Si4713 FM Transmitter", sizeof(v->card));
	usb_make_path(radio->usbdev, v->bus_info, sizeof(v->bus_info));
	v->device_caps = V4L2_CAP_MODULATOR | V4L2_CAP_RDS_OUTPUT;
	v->capabilities = v->device_caps | V4L2_CAP_DEVICE_CAPS;
	
	return 0;
}

static int vidioc_g_modulator(struct file *file, void *priv,
				struct v4l2_modulator *vm)
{
	struct si4713_usb_device *radio = video_drvdata(file);
	
	return v4l2_subdev_call(radio->v4l2_subdev, tuner, g_modulator, vm);
}

static int vidioc_s_modulator(struct file *file, void *priv,
				const struct v4l2_modulator *vm)
{
	struct si4713_usb_device *radio = video_drvdata(file);
	
	return v4l2_subdev_call(radio->v4l2_subdev, tuner, s_modulator, vm);
}

static int vidioc_s_frequency(struct file *file, void *priv,
				const struct v4l2_frequency *vf)
{
	struct si4713_usb_device *radio = video_drvdata(file);
	
	return v4l2_subdev_call(radio->v4l2_subdev, tuner, s_frequency, vf);
}

static int vidioc_g_frequency(struct file *file, void *priv,
				struct v4l2_frequency *vf)
{
	struct si4713_usb_device *radio = video_drvdata(file);
	
	return v4l2_subdev_call(radio->v4l2_subdev, tuner, g_frequency, vf);
}

static const struct v4l2_ioctl_ops usb_si4713_ioctl_ops = {
	.vidioc_querycap    	  = vidioc_querycap,
	.vidioc_g_modulator 	  = vidioc_g_modulator,
	.vidioc_s_modulator 	  = vidioc_s_modulator,
	.vidioc_g_frequency 	  = vidioc_g_frequency,
	.vidioc_s_frequency 	  = vidioc_s_frequency,
	.vidioc_log_status    	  = v4l2_ctrl_log_status,
        .vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
        .vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* File system interface */
static const struct v4l2_file_operations usb_si4713_fops = {
	.owner		= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = v4l2_fh_release,
	.poll           = v4l2_ctrl_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static void usb_si4713_video_device_release(struct v4l2_device *v4l2_dev)
{
	struct si4713_usb_device *radio = to_si4713_dev(v4l2_dev);
	struct i2c_adapter *adapter = &radio->i2c_adapter;

	i2c_del_adapter(adapter);
	v4l2_device_unregister(&radio->v4l2_dev);
	kfree(radio->buffer);
	kfree(radio);
}

static int si4713_send_startup_command(struct si4713_usb_device *radio)
{
	int retval;
	int timeout = 0;
//	u8 *buffer = radio->buffer;
	/* send the command */
	retval = usb_control_msg(radio->usbdev, usb_sndctrlpipe(radio->usbdev, 0),
 					0x09, 0x21, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
 	if (retval < 0)
 		return retval;
	
// 	if(buffer[1] == 0x32)
// 	{
// 		do {
// 		/* receive the response */
// 		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
// 						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
// 		timeout ++;
// 		msleep(SLEEP);
// 		} while ((radio->buffer[1] || radio->buffer[2]) && timeout < TIMEOUT);
// 		return retval;
// 	}
// 	
// 	if(buffer[1] == 0x06) 
// 	{	
// 		do {
// 		/* receive the response */
// 		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
// 						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
// 		timeout ++;
// 		msleep(SLEEP);
// 		} while ((radio->buffer[1] && radio->buffer[2] != SI4713_CTS && radio->buffer[9] != 0x08) && timeout < TIMEOUT);
// 		return retval;
// 	}
// 		
// 	if(buffer[1] == 0x14 || buffer[1] == 0x12 ) 
// 	{
// 		do {
// 		/* receive the response */
// 		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
// 						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
// 		timeout ++;
// 		msleep(SLEEP);
// 		} while ((radio->buffer[1] && radio->buffer[2] != SI4713_CTS) && timeout < TIMEOUT);
// 		return retval;
// 	}
	
	do {
		/* receive the response */
		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
		timeout ++;
		msleep(SLEEP);
	} while (radio->buffer[1] && timeout < TIMEOUT);
	
	return retval;
}

struct si4713_start_seq_table {
	int len;
	u8 payload[8];
};

struct si4713_start_seq_table start_seq[] = {
	
	{ 1, { 0x03 } },
	{ 2, { 0x32, 0x7f } },
	{ 6, { 0x06, 0x03, 0x03, 0x08, 0x01, 0x0f } },
	{ 2, { 0x14, 0x02 } },
	{ 2, { 0x09, 0x90 } },
	{ 3, { 0x08, 0x90, 0xfa } },
	{ 2, { 0x36, 0x01 } },
	{ 2, { 0x05, 0x03 } },
	{ 7, { 0x06, 0x00, 0x06, 0x0e, 0x01, 0x0f, 0x05 } },
	{ 1, { 0x12 } },
	/* Commands that are sent after pressing the 'Initialize' 
	 	button in the windows application */
	{ 1, { 0x03 } },
	{ 1, { 0x01 } },
	{ 2, { 0x09, 0x90 } },
	{ 3, { 0x08, 0x90, 0xfa } },
	{ 1, { 0x34 } },
	{ 2, { 0x35, 0x01 } },
	{ 2, { 0x36, 0x01 } },
	{ 2, { 0x30, 0x09 } },
	{ 4, { 0x30, 0x06, 0x00, 0xe2 } },
	{ 3, { 0x31, 0x01, 0x30 } },
	{ 3, { 0x31, 0x04, 0x09 } },
	{ 2, { 0x05, 0x02 } },
	{ 6, { 0x06, 0x03, 0x03, 0x08, 0x01, 0x0f } },
};

static int si4713_start_seq(struct si4713_usb_device *radio)
{
	int i;
	int retval = 0;
	
	radio->buffer[0] = 0x3f;
	
	for (i = 0; i < ARRAY_SIZE(start_seq); i++) {
		int len = start_seq[i].len;
		u8 *payload = start_seq[i].payload;
		memcpy(radio->buffer + 1, payload, len);
		memset(radio->buffer + len + 1, 0, 63 - len);
		retval = si4713_send_startup_command(radio);
	}
	
	return retval;
}

static struct i2c_board_info si4713_board_info = {
	I2C_BOARD_INFO("si4713", SI4713_I2C_ADDR_BUSEN_HIGH),
};

struct si4713_command_table {
	int command_id;
	u8 payload[8];
};

/* Structure of a command :
 * 	Byte 1 : 0x3f (always)
 * 	Byte 2 : 0x06 (send a command)
 * 	Byte 3 : Unknown 
 * 	Byte 4 : Number of arguments + 1 (for the command byte)
 * 	Byte 5 : Number of response bytes
 */
struct si4713_command_table command_table[] = {
	
	{ SI4713_CMD_POWER_UP,		{ 0x00, SI4713_PWUP_NARGS + 1, SI4713_PWUP_NRESP} },
	{ SI4713_CMD_GET_REV,		{ 0x03, 0x01, SI4713_GETREV_NRESP } },
	{ SI4713_CMD_POWER_DOWN,	{ 0x00, 0x01, SI4713_PWDN_NRESP} }, 
	{ SI4713_CMD_SET_PROPERTY,	{ 0x00, SI4713_SET_PROP_NARGS + 1, SI4713_SET_PROP_NRESP } },
	{ SI4713_CMD_GET_PROPERTY,	{ 0x00, SI4713_GET_PROP_NARGS + 1, SI4713_GET_PROP_NRESP } }, 
	{ SI4713_CMD_TX_TUNE_FREQ,	{ 0x03, SI4713_TXFREQ_NARGS + 1, SI4713_TXFREQ_NRESP } }, 
	{ SI4713_CMD_TX_TUNE_POWER,	{ 0x03, SI4713_TXPWR_NARGS + 1, SI4713_TXPWR_NRESP } }, 
	{ SI4713_CMD_TX_TUNE_MEASURE,	{ 0x03, SI4713_TXMEA_NARGS + 1, SI4713_TXMEA_NRESP } },
	{ SI4713_CMD_TX_TUNE_STATUS,	{ 0x00, SI4713_TXSTATUS_NARGS + 1, SI4713_TXSTATUS_NRESP } }, 
	{ SI4713_CMD_TX_ASQ_STATUS,	{ 0x03, SI4713_ASQSTATUS_NARGS + 1, SI4713_ASQSTATUS_NRESP } },
	{ SI4713_CMD_GET_INT_STATUS,	{ 0x03, 0x01, SI4713_GET_STATUS_NRESP } },
	{ SI4713_CMD_TX_RDS_BUFF,	{ 0x03, SI4713_RDSBUFF_NARGS + 1, SI4713_RDSBUFF_NRESP } },
	{ SI4713_CMD_TX_RDS_PS,		{ 0x00, SI4713_RDSPS_NARGS + 1, SI4713_RDSPS_NRESP } },
	
};

static int send_command(struct si4713_usb_device *radio, u8 *payload, char *data, int len)
{
	int retval;
	int timeout = 0;
	
	radio->buffer[0] = 0x3f;
	radio->buffer[1] = 0x06;
	
	memcpy(radio->buffer + 2, payload, 3);
	memcpy(radio->buffer + 5, data, len); 
	memset(radio->buffer + 5 + len, 0, 59 - len);
	/* send the command */
	retval = usb_control_msg(radio->usbdev, usb_sndctrlpipe(radio->usbdev, 0),
					0x09, 0x21, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
	if (retval < 0)
		return retval;
	/* receive the response */
	do {
		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
		timeout ++;
		msleep(SLEEP);
	} while (radio->buffer[2] != SI4713_CTS && radio->buffer[2] != 0x81 && timeout < TIMEOUT);
	
	return retval;
}

static int si4713_i2c_read(struct si4713_usb_device *radio, char *data, int len)
{
	int retval;
	
	/* receive the response */
	retval = usb_control_msg(radio->usbdev, 
					usb_rcvctrlpipe(radio->usbdev, 0),
					0x01, 0xa1, 0x033f, 0, 
					radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
	if (retval == BUFFER_LENGTH) {
		memcpy(data, radio->buffer + 2, len);
		return 0;
	}

	return retval < 0 ? retval : -EIO;
}

static int si4713_i2c_write(struct si4713_usb_device *radio, char *data, int len)
{
	int retval;
	int i;
	
	if (len > BUFFER_LENGTH)
		return -EINVAL;
	
	for (i = 0; i < ARRAY_SIZE(command_table); i++) {
		if (data[0] == command_table[i].command_id)
			retval = send_command(radio, command_table[i].payload, data, len);
	}
				
	return retval < 0 ? retval : 0;
}

static int si4713_transfer(struct i2c_adapter *i2c_adapter, struct i2c_msg *msgs, int num)
{
	struct si4713_usb_device *radio = i2c_get_adapdata(i2c_adapter);
	int retval = -EINVAL;
	int i;
	
	if (num <= 0)
		return 0;

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			retval = si4713_i2c_read(radio, msgs[i].buf, msgs[i].len);
		else
			retval = si4713_i2c_write(radio, msgs[i].buf, msgs[i].len);
		if (retval)
			break;
	}
	
	return retval ? retval : num; 
}

/* see the description of the flags here : <linux/i2c.h> */ 
static u32 si4713_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}
 
static struct i2c_algorithm si4713_algo = {
	.master_xfer   = si4713_transfer,	
	.functionality = si4713_functionality,
};
 
static struct i2c_adapter si4713_i2c_adapter_template = {
	/* This value shows up in the sysfs filename associated 
	 	with this I2C adapter */
	.name   = "si4713-i2c",
	.owner  = THIS_MODULE,
	.algo   = &si4713_algo,
};

int si4713_register_i2c_adapter(struct si4713_usb_device *radio)
{
	radio->i2c_adapter = si4713_i2c_adapter_template;
	/* set up sysfs linkage to our parent device */
	radio->i2c_adapter.dev.parent = &radio->usbdev->dev;
	i2c_set_adapdata(&radio->i2c_adapter, radio);

	return i2c_add_adapter(&radio->i2c_adapter);
}

/* check if the device is present and register with v4l and usb if it is */
static int usb_si4713_probe(struct usb_interface *intf,
				const struct usb_device_id *id) 
{	
	struct si4713_usb_device *radio;
	struct i2c_adapter *adapter;
	struct v4l2_subdev *sd;
	int retval = -ENOMEM;

	dev_info(&intf->dev, "Si4713 development board discovered: (%04X:%04X)\n",
			id->idVendor, id->idProduct);
	
	/* Initialize local device structure */
	radio = kzalloc(sizeof(struct si4713_usb_device), GFP_KERNEL);
	if (radio)
		radio->buffer = kmalloc(BUFFER_LENGTH, GFP_KERNEL);

	if (!radio || !radio->buffer) {
		dev_err(&intf->dev, "kmalloc for si4713_usb_device failed\n");
		kfree(radio);
		return -ENOMEM;
	}
	
	mutex_init(&radio->lock);
	
	radio->usbdev = interface_to_usbdev(intf);
	radio->intf = intf;
	usb_set_intfdata(intf, &radio->v4l2_dev);
	
	retval = si4713_start_seq(radio);
	if (retval < 0)
		goto err_v4l2;
	
	retval = v4l2_device_register(&intf->dev, &radio->v4l2_dev);
	if (retval < 0) {
		dev_err(&intf->dev, "couldn't register v4l2_device\n");
		goto err_v4l2;
	}
	
	retval = si4713_register_i2c_adapter(radio);
	if (retval < 0) {
		dev_err(&intf->dev, "could not register i2c device\n");
		goto err_i2cdev;
	}
	
	adapter = &radio->i2c_adapter;
	sd = v4l2_i2c_new_subdev_board(&radio->v4l2_dev, adapter,
					  &si4713_board_info, NULL);
	radio->v4l2_subdev = sd;
	if (!sd) {
		dev_err(&intf->dev, "Cannot get v4l2 subdevice\n");
		retval = -ENODEV;
		goto del_adapter; 
	}
	
	radio->vdev.ctrl_handler = sd->ctrl_handler;
	radio->v4l2_dev.release = usb_si4713_video_device_release;
	strlcpy(radio->vdev.name, radio->v4l2_dev.name,
		sizeof(radio->vdev.name));
	radio->vdev.v4l2_dev = &radio->v4l2_dev;
	radio->vdev.fops = &usb_si4713_fops;
	radio->vdev.ioctl_ops = &usb_si4713_ioctl_ops;
	radio->vdev.lock = &radio->lock;
	radio->vdev.release = video_device_release_empty;
	radio->vdev.vfl_dir = VFL_DIR_TX;

	video_set_drvdata(&radio->vdev, radio);
	set_bit(V4L2_FL_USE_FH_PRIO, &radio->vdev.flags);
	
	retval = video_register_device(&radio->vdev, VFL_TYPE_RADIO, -1);
	if (retval < 0) {
		dev_err(&intf->dev, "could not register video device\n");
		goto del_adapter;
	}
	
	dev_info(&intf->dev, "V4L2 device registered as %s\n",
			video_device_node_name(&radio->vdev));
	
	return 0;

del_adapter:
	i2c_del_adapter(adapter);
err_i2cdev:
	v4l2_device_unregister(&radio->v4l2_dev);
err_v4l2:
	kfree(radio->buffer);
	kfree(radio);
	return retval;
}

static void usb_si4713_disconnect(struct usb_interface *intf)
{	
	struct si4713_usb_device *radio = to_si4713_dev(usb_get_intfdata(intf));
	
	dev_info(&intf->dev, "Si4713 development board now disconnected\n");
	
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
	.id_table		= usb_si4713_usb_device_table,
};

module_usb_driver(usb_si4713_driver);
