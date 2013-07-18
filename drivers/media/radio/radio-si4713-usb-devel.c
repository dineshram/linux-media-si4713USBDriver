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
#include <media/v4l2-common.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
//#include <media/v4l2-ctrls.h>
//#include <media/v4l2-event.h>
#include <linux/platform_device.h>
#include <media/radio-si4713.h>

/* driver and module definitions */
MODULE_AUTHOR("Dinesh Ram <dinram@cisco.com> and Hans Verkuil <hansverk@cisco.com>");
MODULE_DESCRIPTION("Si4713 FM Transmitter usb driver");
MODULE_LICENSE("GPL v2");

/* The Device announces itself as Cygnal Integrated Products, Inc. */
#define USB_SI4713_VENDOR	0x10c4
#define USB_SI4713_PRODUCT	0x8244

/* Probably USB_TIMEOUT should be modified in module parameter */
#define BUFFER_LENGTH	64
#define USB_TIMEOUT	500

/* The SI4713 I2C sensor chip has a fixed slave address of 0xc6 or 0x22. */
#define SI4713_I2C_ADDR_BUSEN_HIGH      0x63
#define SI4713_I2C_ADDR_BUSEN_LOW       0x11

#define CTRL_READ_REQUEST	0xb8
#define CTRL_WRITE_REQUEST	0x38

#define REQTYPE_I2C_READ	0x13
#define REQTYPE_I2C_WRITE	0x12
#define REQTYPE_I2C_WRITE_STATT	0xd0


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
	
	struct mutex 		lock;
	
	struct i2c_adapter 	i2c_adapter;	/* I2C adapter */
	char			i2c_buf[BUFFER_LENGTH]; /* I2C message buffer space */
	struct mutex		i2c_mutex;	/* I2C lock */

	u8 			*buffer;
	unsigned 		curfreq;
	u8 			tx;
	u8 			pa;
	bool 			stereo;
	bool 			muted;
	bool 			preemph_75_us;
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
	struct si4713_device *radio = video_drvdata(file);

  	return v4l2_subdev_call(radio->sd, tuner, g_frequency, vf);
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
	.unlocked_ioctl	= video_ioctl2,
};

static const struct v4l2_ioctl_ops usb_si4713_ioctl_ops = {
	.vidioc_querycap    = vidioc_querycap,
	.vidioc_g_modulator = vidioc_g_modulator,
	.vidioc_s_modulator = vidioc_s_modulator,
	.vidioc_g_frequency = vidioc_g_frequency,
	.vidioc_s_frequency = vidioc_s_frequency,
	//.vidioc_log_status = v4l2_ctrl_log_status,
};

static void usb_si4713_video_device_release(struct v4l2_device *v4l2_dev)
{
	struct si4713_device *radio = to_si4713_dev(v4l2_dev);

	/* free rest memory */
	kfree(radio->buffer);
	kfree(radio);
}

// start i2c code

static struct i2c_board_info si4713_board_info __initdata_or_module = {
	I2C_BOARD_INFO("si4713", SI4713_I2C_ADDR_BUSEN_HIGH),
};

static int si4713_i2c_read(struct si4713_device *radio, int bus,
			    unsigned char addr, char *wdata, int wlen,
			    char *data, int len)
{
	int retval;

	if ((len > sizeof(radio->i2c_buf)) || (wlen > sizeof(radio->i2c_buf)))
		return -EINVAL;

	if (wlen) {
		memcpy(&radio->i2c_buf, wdata, wlen);
		retval = usb_control_msg(radio->usbdev, usb_sndctrlpipe(radio->usbdev, 0),
				      REQTYPE_I2C_WRITE, CTRL_WRITE_REQUEST,
				      (bus << 8) | addr, 0, &radio->i2c_buf,
				      wlen, 1000);
		if (retval < 0)
			return retval;
	}

	retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
			      REQTYPE_I2C_READ, CTRL_READ_REQUEST,
			      (bus << 8) | addr, 0, &radio->i2c_buf, len, 1000);

	if (retval == len) {
		memcpy(data, &radio->i2c_buf, len);
		retval = 0;
	} else if (retval >= 0)
		retval = -EIO;

	return retval;
}

static int si4713_i2c_write(struct si4713_device *radio, int bus,
			   unsigned char addr, char *data, int len)
{
	int retval;

	if (len > sizeof(radio->i2c_buf))
		return -EINVAL;

	memcpy(&radio->i2c_buf, data, len);
	retval = usb_control_msg(radio->usbdev, usb_sndctrlpipe(radio->usbdev, 0),
			      REQTYPE_I2C_WRITE, CTRL_WRITE_REQUEST,
			      (bus << 8) | addr, 0, &radio->i2c_buf, len, 1000);

	if (retval < 0)
		return retval;

	retval = usb_control_msg(radio->usbdev, usb_rcvctrlpipe(radio->usbdev, 0),
			      REQTYPE_I2C_WRITE_STATT, CTRL_READ_REQUEST,
			      0, 0, &radio->i2c_buf, 2, 1000);

	if ((retval == 2) && (radio->i2c_buf[1] == (len - 1)))
		retval = 0;
	else if (retval >= 0)
		retval = -EIO;

	return retval;
}

/*from include/media/radio-si4713.h */
static struct radio_si4713_platform_data si4713_data __initdata_or_module = {
	.i2c_bus = 2,
	.subdev_board_info = &si4713_board_info,
};

/* struct i2c_msg — an I2C transaction segment beginning with START 
 * An i2c_msg is the low level representation of one segment of an I2C transaction.
 * It is visible to drivers in the i2c_transfer() procedure, to userspace from i2c-dev,
 *	 and to I2C adapter drivers through the i2c_adapter.master_xfer() method. 
 * Protocol : Each transaction begins with a START. That is followed by the slave address, 
 * 		and a bit encoding read versus write, followed by all the data bytes.The transfer 
 * 		terminates with a NAK, or when all those bytes have been transferred and ACKed. 
 * 		If this is the last message in a group, it is followed by a STOP. Otherwise it is 
 * 		followed by the next i2c_msg transaction segment, beginning with a (repeated) START. 
 * Members - addr : Slave address, either seven or ten bits.
	     flags : I2C_M_RD is handled by all adapters. No other flags may be provided unless 
		the adapter exported the relevant I2C_FUNC_* flags through i2c_check_functionality. 
	     len : Number of data bytes in buf being read from or written to the I2C slave address.
	     buf : The buffer into which data is read, or from which it's written. 
 
 
 */
static int si4713_transfer(struct i2c_adapter *i2c_adapter, struct i2c_msg *msgs,
			  int num)
{
  printk(KERN_INFO "si4713_transfer : i2c adapter transfer function %p\n", i2c_adapter);
  printk(KERN_INFO "si4713_transfer : i2c adapter transfer function %p\n", i2c_get_adapdata(i2c_adapter));
  //return -EIO;
  struct si4713_device *radio = i2c_get_adapdata(i2c_adapter);
  if (!radio) {
	printk(KERN_INFO "si4713_transfer : failed to get a pointer from i2c_get_adapdata(i2c_adapter)\n");
	goto err;
  }
  int retval = 0, addr;
  
  if (num <= 0)
	return 0;
  
  mutex_lock(&radio->i2c_mutex);
  
  addr = msgs[0].addr << 1;
  
  if (num == 1) {
	if (msgs[0].flags & I2C_M_RD)
		retval = si4713_i2c_read(radio, 1, addr, NULL, 0,
					msgs[0].buf, msgs[0].len);
	else
		retval = si4713_i2c_write(radio, 1, addr, msgs[0].buf,
					 msgs[0].len);
	} else if (num == 2) {
		if (msgs[0].addr != msgs[1].addr) {
			v4l2_warn(&radio->v4l2_dev, "refusing 2-phase i2c xfer "
				  "with conflicting target addresses\n");
			retval = -EINVAL;
			goto out;
		}

		if ((msgs[0].flags & I2C_M_RD) || !(msgs[1].flags & I2C_M_RD)) {
			v4l2_warn(&radio->v4l2_dev, "refusing complex xfer with "
				  "r0=%d, r1=%d\n", msgs[0].flags & I2C_M_RD,
				  msgs[1].flags & I2C_M_RD);
			retval = -EINVAL;
			goto out;
		}
		
		 //
		 //* Write followed by atomic read is the only complex xfer that
		 //* we actually support here.
		 //
		retval = si4713_i2c_read(radio, 1, addr, msgs[0].buf, msgs[0].len,
					msgs[1].buf, msgs[1].len);
	} else {
		v4l2_warn(&radio->v4l2_dev, "refusing %d-phase i2c xfer\n", num);
	}

out:
  mutex_unlock(&radio->i2c_mutex);
err:
  return 0;

  return retval ? retval : num; 
}

static u32 si4713_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

/*
 * To unregister an I2C adapter, the driver should call the function i2c_del_adapter with a pointer to the struct i2c_adapter,
 *  like this:  i2c_del_adapter(&si4713_i2c_adapter_template);
 
 * An I2C algorithm is used by the I2C bus driver to talk to the I2C bus.
 * An algorithm driver is defined by a struct i2c_algorithm structure.
 */
 
 static struct i2c_algorithm si4713_algo = {
	.master_xfer   = si4713_transfer,	// a function pointer to be set if this algorithm driver can do I2C direct-level accesses. 
						// If it is set, this function is called whenever an I2C chip driver wants to communicate with the chip device.
	.functionality = si4713_functionality,	// a function pointer called by the I2C core to determine what kind of reads and writes the I2C adapter driver can do.
};


/* 
 * An I2C bus driver is described by a struct named i2c_adapter, which is defined in the include/linux/i2c.h file. 
 * Only the following fields need to be set up by the bus driver */
 
static struct i2c_adapter si4713_i2c_adapter_template = {
	.name   = "Si4713-I2C-fooooo",	// This value shows up in the sysfs filename associated with this I2C adapter
	.owner  = THIS_MODULE,
	.algo   = &si4713_algo,
};

/*
 * To register this I2C adapter, the driver calls the function i2c_add_adapter with a pointer to the struct i2c_adapter
 * If the I2C adapter lives on a type of device that has a struct device associated with it, such as a PCI or USB device, 
 *  then before the call to i2c_add_adapter, the adapter device's parent pointer should be set to that device. */

int si4713_register_i2c_adapter(struct si4713_device *radio)
{
	int retval = -ENOMEM;

	radio->i2c_adapter = si4713_i2c_adapter_template;
	radio->i2c_adapter.dev.parent = &radio->usbdev->dev; // set up sysfs linkage to our parent device.
	i2c_set_adapdata(&radio->i2c_adapter, radio);

	retval = i2c_add_adapter(&radio->i2c_adapter);
	if (retval < 0) {
	  printk(KERN_INFO "si4713_register_i2c_adapter : i2c adapter failed to register\n");
	}
	printk(KERN_INFO "si4713_register_i2c_adapter : i2c adapter registered\n");
	return retval;
}

// end i2c code

/* check if the device is present and register with v4l and usb if it is */
static int usb_si4713_probe(struct usb_interface *intf,
				const struct usb_device_id *id) 
{	
	struct usb_device *usbdev = interface_to_usbdev(intf); /* WARNING : unused variable */
	struct si4713_device *radio;
	struct i2c_adapter *adapter;
	struct v4l2_subdev *sd;
	//struct radio_si4713_platform_data *si4713_data;
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
	
	/* TODO : some code to be written here */
	
	/* Device Registration : v4l2_device_register(struct device *usbdev, struct v4l2_device *v4l2_dev);
	   Registration will initialize the v4l2_device struct and link usbdev->driver_data
	   to v4l2_dev. Registration will also set v4l2_dev->name to a value derived from
	   usbdev (driver name followed by the bus_id, to be precise). 
	   The first 'usbdev' argument is normally the struct device pointer of a pci_dev,
	   usb_device or platform_device.*/
	retval = v4l2_device_register(&intf->dev, &radio->v4l2_dev);	
	if (retval < 0) {
		dev_err(&intf->dev, "couldn't register v4l2_device\n");
		goto err_v4l2;
	}

	mutex_init(&radio->lock);
	mutex_init(&radio->i2c_mutex);
	
	radio->usbdev = interface_to_usbdev(intf);
	radio->intf = intf;
	usb_set_intfdata(intf, &radio->v4l2_dev);
	
	/* from radio-si4713.c*/
	//adapter = i2c_get_adapter(pdata->i2c_bus);
	// register 12c device
	printk(KERN_INFO "probe : initialize registering i2c device\n");
	retval = si4713_register_i2c_adapter(radio);
	printk(KERN_INFO "probe : finished registering i2c device\n");
	if (retval < 0) {
		dev_err(&intf->dev, "could not register i2c device\n");
		goto err_i2cdev;
	}
	
	adapter = &radio->i2c_adapter;
	if (!adapter) {
		dev_err(&intf->dev, "Cannot get i2c adapter\n"); /* pdata->i2c_bus */
		retval = -ENODEV;
		goto unregister_v4l2_dev;
	}
	printk(KERN_INFO "probe : got adapter %p\n", adapter);
	printk(KERN_INFO "probe : got adapter %p %p\n", radio, i2c_get_adapdata(adapter));

	sd = v4l2_i2c_new_subdev_board(&radio->v4l2_dev, adapter,
				       si4713_data.subdev_board_info, NULL);
	if (!sd) {
		dev_err(&intf->dev, "Cannot get v4l2 subdevice\n");
		retval = -ENODEV;
		goto del_adapter; 
	}

	
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
		goto err_vdev;
	}
	
	dev_info(&intf->dev, "V4L2 device registered as %s\n",
			video_device_node_name(&radio->vdev));
	
	return 0;
	
del_adapter:
	i2c_del_adapter(adapter);

unregister_v4l2_dev:
	v4l2_device_unregister(&radio->v4l2_dev);

err_i2cdev:
	printk(KERN_INFO "err_i2cdev"); 

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

module_init(si4713_init);
module_exit(si4713_exit);