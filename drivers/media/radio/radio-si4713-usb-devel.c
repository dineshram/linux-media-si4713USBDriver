/*
 * Copyright (c) 2013 Dinesh Ram<dinram@cisco.com> & Hans Verkuil<hansverk@cisco.com>
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
#include <linux/mutex.h>
#include <linux/i2c.h>
/* V4l includes */
#include <linux/videodev2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/radio-si4713.h>

/* module parameters */
static int debug;
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug level (0 - 2)");

/* driver and module definitions */
MODULE_AUTHOR("Dinesh Ram <dinram@cisco.com> and Hans Verkuil <hansverk@cisco.com>");
MODULE_DESCRIPTION("Si4713 FM Transmitter USB driver");
MODULE_LICENSE("GPL v2");

/* The Device announces itself as Cygnal Integrated Products, Inc. */
#define USB_SI4713_VENDOR	0x10c4 
#define USB_SI4713_PRODUCT	0x8244

#define BUFFER_LENGTH		64
#define USB_TIMEOUT		1000

/* The SI4713 I2C sensor chip has a fixed slave address of 0xc6 or 0x22. */
#define SI4713_I2C_ADDR_BUSEN_HIGH      0x63
#define SI4713_I2C_ADDR_BUSEN_LOW       0x11

#define SI4713_CMD_POWER_UP		0x01
#define SI4713_CMD_GET_REV		0x10
#define SI4713_CMD_POWER_DOWN		0x11
#define	SI4713_CMD_SET_PROPERTY		0x12
#define SI4713_CMD_GET_PROPERTY		0x13
#define SI4713_CMD_TX_TUNE_FREQ		0x30
#define SI4713_CMD_TX_TUNE_POWER	0x31
#define SI4713_CMD_TX_TUNE_MEASURE	0x32
#define SI4713_CMD_TX_TUNE_STATUS	0x33
#define SI4713_CMD_TX_ASQ_STATUS	0x34
#define SI4713_CMD_GET_INT_STATUS	0x14
#define SI4713_CMD_TX_RDS_BUFF		0x35
#define SI4713_CMD_TX_RDS_PS		0x36

#define SLEEP				3
#define TIMEOUT				15

/* USB Device ID List */
static struct usb_device_id usb_si4713_device_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(USB_SI4713_VENDOR, USB_SI4713_PRODUCT,
							USB_CLASS_HID, 0, 0) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, usb_si4713_device_table);

struct si4713_device {
	struct usb_device 	*usbdev;	/* the usb device for this device */
	struct usb_interface 	*intf;
	struct video_device 	vdev;		/* the v4l device for this device */
	struct v4l2_device 	v4l2_dev;
	struct v4l2_subdev	*v4l2_subdev;
	struct mutex 		i2c_lock;
	struct i2c_adapter 	i2c_adapter;	/* I2C adapter */
	struct mutex		i2c_mutex;	/* I2C lock */
	u8 			*buffer;
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
	v->device_caps = V4L2_CAP_MODULATOR | V4L2_CAP_RDS_OUTPUT;
	v->capabilities = v->device_caps | V4L2_CAP_DEVICE_CAPS;
	
	return 0;
}

static inline struct v4l2_device *get_v4l2_dev(struct file *file)
{
	return &((struct si4713_device *)video_drvdata(file))->v4l2_dev;
}

static int vidioc_g_modulator(struct file *file, void *priv,
				struct v4l2_modulator *vm)
{
	return v4l2_device_call_until_err(get_v4l2_dev(file), 0, tuner,
						g_modulator, vm);
}

static int vidioc_s_modulator(struct file *file, void *priv,
				const struct v4l2_modulator *vm)
{
	return v4l2_device_call_until_err(get_v4l2_dev(file), 0, tuner,
						s_modulator, vm);
}

static int vidioc_s_frequency(struct file *file, void *priv,
				const struct v4l2_frequency *vf)
{
	return v4l2_device_call_until_err(get_v4l2_dev(file), 0, tuner,
					  s_frequency, vf);
}

static int vidioc_g_frequency(struct file *file, void *priv,
				struct v4l2_frequency *vf)
{
	return v4l2_device_call_until_err(get_v4l2_dev(file), 0, tuner,
					  g_frequency, vf);
}

static const struct v4l2_ioctl_ops usb_si4713_ioctl_ops = {
	.vidioc_querycap    = vidioc_querycap,
	.vidioc_g_modulator = vidioc_g_modulator,
	.vidioc_s_modulator = vidioc_s_modulator,
	.vidioc_g_frequency = vidioc_g_frequency,
	.vidioc_s_frequency = vidioc_s_frequency,
};

/* File system interface */
static const struct v4l2_file_operations usb_si4713_fops = {
	.owner		= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = v4l2_fh_release,
	.unlocked_ioctl	= video_ioctl2,
};

static void usb_si4713_video_device_release(struct v4l2_device *v4l2_dev)
{
	struct si4713_device *radio = to_si4713_dev(v4l2_dev);
	struct i2c_adapter *adapter = &radio->i2c_adapter;

	/* free rest memory */
	i2c_del_adapter(adapter);
	v4l2_device_unregister(&radio->v4l2_dev);
	kfree(radio->buffer);
	kfree(radio);
}

static int si4713_send_startup_command(struct si4713_device *radio)
{
	/* TODO : Implement proper timeout */
	int retval;
	int timeout = 0;
	//u8 *buffer = radio->buffer;
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
// 		timeout += 1;
// 		msleep(SLEEP);
// 		} while((radio->buffer[1] || radio->buffer[2]) && timeout < TIMEOUT);
// 		return retval;
// 	}
// 	
// 	if(buffer[1] == 0x06) 
// 	{	
// 		do {
// 		/* receive the response */
// 		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
// 						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
// 		timeout += 1;
// 		msleep(SLEEP);
// 		} while((radio->buffer[1] && radio->buffer[2] != 0x80 && radio->buffer[9] != 0x08) && timeout < TIMEOUT);
// 		return retval;
// 	}
// 		
// 	if(buffer[1] == 0x14 || buffer[1] == 0x12 ) 
// 	{
// 		do {
// 		/* receive the response */
// 		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
// 						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
// 		timeout += 1;
// 		msleep(SLEEP);
// 		} while((radio->buffer[1] && radio->buffer[2] != 0x80) && timeout < TIMEOUT);
// 		return retval;
// 	}
	
	do {
		/* receive the response */
		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
		timeout += 1;
		msleep(SLEEP);
	} while(radio->buffer[1] && timeout < TIMEOUT);
	
	return retval;
}

struct si4713_start_seq_table {
	int len;
	u8 payload[8];
};

struct si4713_start_seq_table start_seq[] = {
	
	{2, {0x3f, 0x03}},
	{3, {0x3f, 0x32, 0x7f}},
	{7, {0x3f, 0x06, 0x03, 0x03, 0x08, 0x01, 0x0f}},
	{3, {0x3f, 0x14, 0x02}},
	{3, {0x3f, 0x09, 0x90}},
	{4, {0x3f, 0x08, 0x90, 0xfa}},
	{3, {0x3f, 0x36, 0x01}},
	{3, {0x3f, 0x05, 0x03}},
	{8, {0x3f, 0x06, 0x00, 0x06, 0x0e, 0x01, 0x0f, 0x05}},
	{2, {0x3f, 0x12}},
	/* Commands that are sent after pressing the 'Initialize' button in the windows application */
	{2, {0x3f, 0x03}},
	{2, {0x3f, 0x01}},
	{3, {0x3f, 0x09, 0x90}},
	{4, {0x3f, 0x08, 0x90, 0xfa}},
	{2, {0x3f, 0x34}},
	{3, {0x3f, 0x35, 0x01}},
	{3, {0x3f, 0x36, 0x01}},
	{3, {0x3f, 0x30, 0x09}},
	{5, {0x3f, 0x30, 0x06, 0x00, 0xe2}},
	{4, {0x3f, 0x31, 0x01, 0x30}},
	{4, {0x3f, 0x31, 0x04, 0x09}},
	{3, {0x3f, 0x05, 0x02}},
	{7, {0x3f, 0x06, 0x03, 0x03, 0x08, 0x01, 0x0f}},
};

static int si4713_start_seq(struct si4713_device *radio)
{
	int i;
	int retval = 0;
	for(i = 0; i < ARRAY_SIZE(start_seq); i++){
		int len = start_seq[i].len;
		u8 *payload = start_seq[i].payload;
		memcpy(radio->buffer, payload, len);
		memset(radio->buffer + len, 0, 64-len);
		retval = si4713_send_startup_command(radio);
	}
	return retval;
}

static struct i2c_board_info si4713_board_info __initdata_or_module = {
	I2C_BOARD_INFO("si4713", SI4713_I2C_ADDR_BUSEN_HIGH),
};

struct si4713_command_table {
	int pref;
	u8 payload[8];
};

/* Structure of a command :
 * 	Byte 1 : 0x3f
 * 	Byte 2 : 0x06 (send a command)
 * 	Byte 3 : Unknown 
 * 	Byte 4 : Number of arguments + 1 (for the command byte)
 * 	Byte 5 : Number of response bytes
 */
struct si4713_command_table command_table[] = {
	
	{5, {0x3f, 0x06, 0x00, 0x03, 0x01}}, /* POWER_UP */ 
	{5, {0x3f, 0x06, 0x03, 0x01, 0x10}}, /* GET_REV */
	{5, {0x3f, 0x06, 0x00, 0x01, 0x01}}, /* POWER_DOWN */ 
	{5, {0x3f, 0x06, 0x00, 0x06, 0x01}}, /* SET_PROPERTY */
	{5, {0x3f, 0x06, 0x00, 0x04, 0x04}}, /* GET_PROPERTY */
	{5, {0x3f, 0x06, 0x03, 0x04, 0x01}}, /* TX_TUNE_FREQ */
	{5, {0x3f, 0x06, 0x03, 0x05, 0x01}}, /* TX_TUNE_POWER */
	{5, {0x3f, 0x06, 0x03, 0x05, 0x01}}, /* TX_TUNE_MEASURE */ /* TODO : Check byte 3 */
	{5, {0x3f, 0x06, 0x00, 0x02, 0x08}}, /* TX_TUNE_STATUS */
	{5, {0x3f, 0x06, 0x03, 0x02, 0x05}}, /* TX_ASQ_STATUS */
	{5, {0x3f, 0x06, 0x03, 0x01, 0x01}}, /* GET_INT_STATUS */
	{5, {0x3f, 0x06, 0x03, 0x06, 0x08}}, /* TX_RDS_BUFF */ /* TODO : Check byte 3 */
	{5, {0x3f, 0x06, 0x00, 0x06, 0x01}}, /* TX_RDS_PS */
	
};

static int send_command(struct si4713_device *radio, int pref, u8 *payload, char *data, int len)
{
	int retval;
	int timeout = 0;
	memcpy(radio->buffer, payload, pref);
	memcpy(radio->buffer + pref, data, len);
	memset(radio->buffer + pref + len, 0, 64-pref-len);
	/* send the command */
	retval = usb_control_msg(radio->usbdev, usb_sndctrlpipe(radio->usbdev, 0),
					0x09, 0x21, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
	if (retval < 0)
		return retval;
	/* receive the response */
	do {
		retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
						0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
		timeout += 1;
		msleep(SLEEP);
	} while(radio->buffer[2] != 0x80 && radio->buffer[2] != 0x81 && timeout < TIMEOUT);
	
	return retval;
}

static int si4713_i2c_read(struct si4713_device *radio, char *data, int len)
{
	int retval;
	
	/* receive the response */
	retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
					0x01, 0xa1, 0x033f, 0, radio->buffer, BUFFER_LENGTH, USB_TIMEOUT);
	if (retval == BUFFER_LENGTH) {
		memcpy(data, radio->buffer + 2, len);
		retval = 0;
	} else if (retval >= 0)
		retval = -EIO;

	return retval < 0 ? retval : 0;
}

static int si4713_i2c_write(struct si4713_device *radio, char *data, int len)
{
	int retval;
	
	if (len > BUFFER_LENGTH)
		return -EINVAL;
	
	switch(data[0]){
		case SI4713_CMD_POWER_UP:
			retval = send_command(radio,command_table[0].pref, command_table[0].payload, data, len);
			break;
		case SI4713_CMD_GET_REV:
			retval = send_command(radio, command_table[1].pref, command_table[1].payload, data, len);
			break;
		case SI4713_CMD_POWER_DOWN:
			retval = send_command(radio,command_table[2].pref, command_table[2].payload, data, len);
			break;
		case SI4713_CMD_SET_PROPERTY:
			retval = send_command(radio,command_table[3].pref, command_table[3].payload, data, len);
			break;
		case SI4713_CMD_GET_PROPERTY:
			retval = send_command(radio,command_table[4].pref, command_table[4].payload, data, len);
			break;
		case SI4713_CMD_TX_TUNE_FREQ:
			retval = send_command(radio,command_table[5].pref, command_table[5].payload, data, len);
			break;
		case SI4713_CMD_TX_TUNE_POWER:
			retval = send_command(radio,command_table[6].pref, command_table[6].payload, data, len);
			break;
		case SI4713_CMD_TX_TUNE_MEASURE:
			retval = send_command(radio,command_table[7].pref, command_table[7].payload, data, len);
			break;
		case SI4713_CMD_TX_TUNE_STATUS:
			retval = send_command(radio,command_table[8].pref, command_table[8].payload, data, len);
			break;
		case SI4713_CMD_TX_ASQ_STATUS:
			retval = send_command(radio,command_table[9].pref, command_table[9].payload, data, len);
			break;	
		case SI4713_CMD_GET_INT_STATUS:
			retval = send_command(radio,command_table[10].pref, command_table[10].payload, data, len);
			break;
		case SI4713_CMD_TX_RDS_BUFF:
			retval = send_command(radio,command_table[11].pref, command_table[11].payload, data, len);
			break;
		case SI4713_CMD_TX_RDS_PS:
			retval = send_command(radio,command_table[12].pref, command_table[12].payload, data, len);
			break;
	}
				
	return retval < 0 ? retval : 0;
}

static int si4713_transfer(struct i2c_adapter *i2c_adapter, struct i2c_msg *msgs, int num)
{
	struct si4713_device *radio = i2c_get_adapdata(i2c_adapter);
	int retval = -EINVAL;
	int i;
	
	if (num <= 0)
		return 0;
	
	mutex_lock(&radio->i2c_mutex);

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			retval = si4713_i2c_read(radio, msgs[i].buf, msgs[i].len);
		else
			retval = si4713_i2c_write(radio, msgs[i].buf, msgs[i].len);
		if (retval)
			break;
	}
	
	mutex_unlock(&radio->i2c_mutex);
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
	.name   = "Si4713-I2C",	// This value shows up in the sysfs filename associated with this I2C adapter
	.owner  = THIS_MODULE,
	.algo   = &si4713_algo,
};

int si4713_register_i2c_adapter(struct si4713_device *radio)
{
	int retval = -ENOMEM;

	radio->i2c_adapter = si4713_i2c_adapter_template;
	radio->i2c_adapter.dev.parent = &radio->usbdev->dev; // set up sysfs linkage to our parent device.
	i2c_set_adapdata(&radio->i2c_adapter, radio);

	retval = i2c_add_adapter(&radio->i2c_adapter);
	return retval;
}

/* check if the device is present and register with v4l and usb if it is */
static int usb_si4713_probe(struct usb_interface *intf,
				const struct usb_device_id *id) 
{	
	struct si4713_device *radio;
	struct i2c_adapter *adapter;
	struct v4l2_subdev *sd;
	int retval = -ENOMEM;

	struct usb_host_interface *iface_desc;
	iface_desc = intf->cur_altsetting;
	dev_info(&intf->dev, "Si4713 development board i/f %d discovered: (%04X:%04X)\n",
			iface_desc->desc.bInterfaceNumber, id->idVendor, id->idProduct);
	dev_info(&intf->dev, "Si4713 : ID->bInterfaceClass: %02X\n",
			iface_desc->desc.bInterfaceClass);
	
	/* Initialize local device structure */
	radio = kzalloc(sizeof(struct si4713_device), GFP_KERNEL);
	if (radio)
		radio->buffer = kmalloc(BUFFER_LENGTH, GFP_KERNEL);

	if (!radio || !radio->buffer) {
		dev_err(&intf->dev, "kmalloc for si4713_device failed\n");
		kfree(radio);
		return -ENOMEM;
	}
	
	mutex_init(&radio->i2c_lock);
	mutex_init(&radio->i2c_mutex);
	
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
	radio->vdev.lock = &radio->i2c_lock;
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
	struct si4713_device *radio = to_si4713_dev(usb_get_intfdata(intf));
	printk(KERN_INFO "Si4713 development board i/f %d now disconnected\n",
		intf->cur_altsetting->desc.bInterfaceNumber);
	mutex_lock(&radio->i2c_lock);
	usb_set_intfdata(intf, NULL);
	video_unregister_device(&radio->vdev);
	v4l2_device_disconnect(&radio->v4l2_dev);
	mutex_unlock(&radio->i2c_lock);
	v4l2_device_put(&radio->v4l2_dev);
}

/* USB subsystem interface */
static struct usb_driver usb_si4713_driver = {
	.name			= "radio-si4713-usb",
	.probe			= usb_si4713_probe,
	.disconnect		= usb_si4713_disconnect,
	.id_table		= usb_si4713_device_table,
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

module_init(si4713_init);
module_exit(si4713_exit);
