#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/gpio.h>

#include "piplates.h"

MODULE_LICENSE("Dual BSD/GPL");

struct piplate_dev *piplate_spi = NULL;
static dev_t piplate_spi_num;
static struct cdev *piplate_spi_cdev;
static struct class *piplate_spi_class;
static struct device *piplate_spi_dev;

static const struct of_device_id piplate_dt_ids[] = {
	{ .compatible = "piplate" },
	{},
};
MODULE_DEVICE_TABLE(of, piplate_dt_ids);

static int piplate_spi_open(struct inode *inode, struct file *filp){
	struct piplate_dev *dev = piplate_spi;

	spin_lock_irq(&dev->spinlock);

	if(dev->opened){
		printk(KERN_INFO "Cannot open device file twice\n");
		spin_unlock_irq(&dev->spinlock);
		return -EIO;
	}

	dev->opened = true;
	filp->private_data = dev;

	spin_unlock_irq(&dev->spinlock);

	return 0;
}

static int piplate_spi_release(struct inode *inode, struct file *filp){
	struct piplate_dev *dev;
	dev = filp->private_data;

	spin_lock_irq(&dev->spinlock);

	filp->private_data = NULL;
	dev->opened = false;

	spin_unlock_irq(&dev->spinlock);

	return 0;
}

static int piplate_spi_message(struct piplate_dev *dev, int bytesToReturn){
	struct spi_transfer transfer = {
		.tx_buf = &dev->tx_buf,
		.len = 4,
		.speed_hz = MAX_SPEED_HZ,
		.delay_usecs = 60,//Not sure if this could cause the process to sleep. Almost confident the answer is no.
	};

	struct spi_message msg = { };
	int status;

	spi_message_init(&msg);
	spi_message_add_tail(&transfer, &msg);

	gpio_set_value(FRAME, 1);

	status = spi_sync(dev->spi, &msg);

	if(status)
		return status;

	if(bytesToReturn > 0){
		//Wait for 100/250 us (not sure what the best way to do this is yet)

		transfer.tx_buf = NULL;
		transfer.len = 1;
		unsigned char rBuf[1];
		transfer.rx_buf = &rBuf;
		transfer.delay_usecs = 20;

		//Not sure if I can reuse transfer or reuse message. I'm resuing transfer but not message at the moment as a middle ground.

		for(int i = 0; i < bytesToReturn; i ++){
			struct spi_message msg = { };
			spi_message_init(&msg);
			spi_message_add_tail(&transfer, &msg);
			status = spi_sync(dev->spi, &msg);
			if(status)
				return status;
			//Copy results to intermediate location, then copy to user.
		}
	}else if(bytesToReturn == -1){
		//Receive bytes until exceeding 25 or until a 0 is received.
	}

	gpio_set_value(FRAME, 0);

	//Copy bytes to a user space buffer

	return 0;
}


static int piplate_ack_spi_message(struct piplate_dev *dev, unsigned char addr, unsigned char cmd, unsigned char p1, unsigned char p2, int bytesToReturn){
	unsigned char buf[4];
	buf[0] = addr;//Add base depending on type
	buf[1] = cmd;
	buf[2] = p1;
	buf[3] = p2;
	struct spi_transfer transfer = {
		.tx_buf = buf,
		.len = 4,
		.speed_hz = MAX_SPEED_HZ,
		.delay_usecs = 5, //Once again, not sure if this causes the process to sleep.
	};

	struct spi_message msg = { };
	int status;

	spi_message_init(&msg);
	spi_message_add_tail(&transfer, &msg);

	//Confirm ACK bit is high

	gpio_set_value(FRAME, 1);

	status = spi_sync(dev->spi, &msg);

	//Verify status

	//Wait for ACK bit low
	if(bytesToReturn > 0){
		for(int i = 0; i <= bytesToReturn; i++){
			//Small delay
			//Receive bytes
		}
	}else if(bytesToReturn == -1){
		//Receive bytes until exceeding 25 or until a 0 is received.
	}

	gpio_set_value(FRAME, 0);

	//Copy bytes to a user space buffer

	return 0;
}

static int piplate_probe(struct spi_device *spi){
	if(!piplate_spi){
		piplate_spi = kzalloc(sizeof *piplate_spi, GFP_KERNEL);
	}
	if (!piplate_spi){
		printk(KERN_INFO "Failed to allocate memory for spi device\n");
		return -ENOMEM;
	}

	spi->mode = SPI_MODE_0;
	piplate_spi->spi = spi;
	piplate_spi->opened = false;
	piplate_spi->max_speed_hz = MAX_SPEED_HZ;

	spi_set_drvdata(spi, piplate_spi);

	return 0;
}

static int piplate_remove(struct spi_device *spi){
	struct piplate_dev *mydev = spi_get_drvdata(spi);

	kfree(mydev);

	return 0;
}

static struct spi_driver piplate_driver = {
	.driver = {
		.name = "piplateSPI",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(piplate_dt_ids),
	},
	.probe = piplate_probe,
	.remove = piplate_remove,
};

static const struct file_operations piplate_fops = {
	.owner = THIS_MODULE,
	.open = piplate_spi_open,
	.release = piplate_spi_release,
	.ioctl = piplate_spi_ioctl,
};

static int __init piplate_spi_init(void){
	int registered;

	int error;

	printk(KERN_INFO "Loading module...\n");

	if(gpio_request(FRAME, "FRAME")){
		printk(KERN_INFO "Can't request FRAME pin\n");
		error = -ENOMEM;
		goto end;
	}

	if(gpio_request(ACK, "ACK")){
		printk(KERN_INFO "Can't request ACK pin\n");
		error = -ENOMEM;
		goto free_frame;
	}

	if(gpio_direction_input(ACK) || gpio_direction_output(FRAME, 0)){
		printk(KERN_INFO "Can't set input/output mode for pins\n");
		error = -ENOMEM;
		goto free_ack;
	}

	if(!(piplate_spi = kzalloc(sizeof(*piplate_spi), GFP_KERNEL))){
		printk(KERN_INFO "Can't allocate memory for spi device");
		error = -ENOMEM;
		goto free_ack;
	}

	spin_lock_init(&piplate_spi->spinlock);

	if(alloc_chrdev_region(&piplate_spi_num, 0, 1, DEV_NAME) < 0){
		printk(KERN_INFO "Error while allocating device number\n");
		error = -ENOMEM;
		goto free_mem;
	}

	if(!(piplate_spi_cdev = cdev_alloc())){
		printk(KERN_INFO "Error allocating memory for cdev struct\n");
		error = -ENOMEM;
		goto unregister_chrdev;
	}

	piplate_spi_cdev->owner = THIS_MODULE;
	piplate_spi_cdev->ops = &piplate_fops;

	if(cdev_add(piplate_spi_cdev, piplate_spi_num, 1)){
		printk(KERN_INFO "Failed to add cdev object\n");
		error = -ENOMEM;
		goto unregister_chrdev;
	}

	if(!(piplate_spi_class = class_create(THIS_MODULE, DEV_NAME))){
		printk(KERN_INFO "Error while creating device class\n");
		error = -ENOMEM;
		goto delete_cdev;
	}

	if(!(piplate_spi_dev = device_create(piplate_spi_class, NULL, piplate_spi_num, NULL, "%s", DEV_NAME))){
		printk(KERN_INFO "Error while creating device\n");
		error = -ENOMEM;
		goto destroy_class;
	}

	if(registered = spi_register_driver(&piplate_driver)){
		printk(KERN_INFO "Error while registering driver\n");
		error = -ENOMEM;
		goto destroy_dev;
	}

	printk(KERN_INFO "Sucessfully loaded module");

	return 0;

	destroy_dev:
		device_destroy(piplate_spi_class, piplate_spi_num);
	destroy_class:
		class_destroy(piplate_spi_class);
	delete_cdev:
		cdev_del(piplate_spi_cdev);
	unregister_chrdev:
		unregister_chrdev_region(piplate_spi_num, 1);
	free_mem:
		kfree(piplate_spi);
	free_ack:
		gpio_free(ACK);
	free_frame:
		gpio_free(FRAME);
	end:
		return error;
}

static void __exit piplate_spi_exit(void){
	printk(KERN_INFO "Unloading module...");
	spi_unregister_driver(&piplate_driver);
	device_destroy(piplate_spi_class, piplate_spi_num);
	class_destroy(piplate_spi_class);
	cdev_del(piplate_spi_cdev);
	unregister_chrdev_region(piplate_spi_num, 1);
	kfree(piplate_spi);
	gpio_free(ACK);
	gpio_free(FRAME);
	printk(KERN_INFO "Sucessfully unloaded module");
}

module_init(piplate_spi_init);
module_exit(piplate_spi_exit);
