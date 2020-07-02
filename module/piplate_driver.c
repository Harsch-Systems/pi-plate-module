#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/timekeeping.h>

#include "piplate.h"

#define FRAME 25
#define ACK 23

/*
  Defines the debug level. Three options available:
  0: Doesn't log anything.
  1: Logs errors
  2: Logs errors and extra debug info
  The default level is 1.
*/

#define DEBUG_LEVEL_NONE 0
#define DEBUG_LEVEL_ERR 1
#define DEBUG_LEVEL_ALL 2

static int debug_level = DEBUG_LEVEL_ERR;
module_param(debug_level, int, S_IRUSR | S_IWUSR);

struct piplate_dev {
	struct spi_device *spi;
	unsigned char tx_buf[4];
	unsigned char rx_buf[1];
	unsigned int max_speed_hz;
	struct mutex lock;
};

struct piplate_dev *piplate_spi = NULL;
static dev_t piplate_spi_num;
static struct cdev *piplate_spi_cdev;
static struct class *piplate_spi_class;
static struct device *piplate_spi_dev;

static const struct of_device_id piplate_dt_ids[] = {
	{ .compatible = "piplate" }, //This name must be the same as whatever is in the device tree
	{},
};

MODULE_DEVICE_TABLE(of, piplate_dt_ids);

static int piplate_open(struct inode *inode, struct file *filp){
	struct piplate_dev *dev = piplate_spi;

	filp->private_data = dev;

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Pi Plate File Opened");

	return 0;
}

static int piplate_release(struct inode *inode, struct file *filp){
	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Pi Plate File Released");

	filp->private_data = NULL;

	return 0;
}


/*
  Sends a message to the pi plate given the information in message m.
  m has already been copied from user space in ioctl, so this does
  not need to worry about user space access.
*/
static int piplate_spi_message(struct piplate_dev *dev, struct message *m){
	struct spi_message msg = { };
	int status;
	unsigned long j0;
	struct spi_transfer transfer = { };

	dev->tx_buf[0] = m->addr;
	dev->tx_buf[1] = m->cmd;
	dev->tx_buf[2] = m->p1;
	dev->tx_buf[3] = m->p2;

	transfer.tx_buf = &dev->tx_buf;
	transfer.len = 4;
	transfer.speed_hz = dev->max_speed_hz;
	transfer.delay_usecs = (m->useACK ? 5 : 60);

	spi_message_init(&msg);
	spi_message_add_tail(&transfer, &msg);

	if(debug_level == DEBUG_LEVEL_ALL){
		if(m->useACK)
			printk(KERN_DEBUG "Sending command to Pi Plate using ACK wire (TINKERplate, DAQC2plate, THERMOplate)");
		else
			printk(KERN_DEBUG "Sending command to Pi Plate without ACK wire (DAQCplate, RELAYplate, MOTORplate)");
		printk(KERN_DEBUG "Address: %d\n", m->addr);
		printk(KERN_DEBUG "Command number: %d\n", m->cmd);
	}

	//Confirm ACK bit is high before transfer begins
	if(m->useACK){
		j0 = jiffies;
		while(!gpio_get_value(ACK)){
			if(time_after(jiffies, j0 + (HZ/100))){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Timed out while confirming ACK bit high");
				status = -EIO;
				goto end;
			}
		}
	}

	gpio_set_value(FRAME, 1);

	status = spi_sync(dev->spi, &msg);

	if(status){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Unknown error while sending SPI command");
		goto end;
	}

	//Reuse the same transfer for receiving data now
	transfer.tx_buf = NULL;
	transfer.len = 1;
	transfer.rx_buf = dev->rx_buf;
	if(m->useACK)
		transfer.delay_usecs = 20;

	//Handles different delay systems. If using ACK, wait for that to go low. Otherwise, delay for 100us.
	if(m->bytesToReturn > 0 || m->bytesToReturn == -1){
		if(m->useACK){
			j0 = jiffies;
			while(gpio_get_value(ACK)){
				if(time_after(jiffies, j0 + (HZ/100))){
					if(debug_level >= DEBUG_LEVEL_ERR)
						printk(KERN_ERR "Timed out while waiting for ACK bit low");
					status = -EIO;
					goto end;
				}
			}
		}else{
			udelay(100);
		}
	}

	if(m->bytesToReturn > 0){
		int i;
		int max = m->bytesToReturn + m->useACK;//Newer ACK pi plates include extra verification response byte
		for(i = 0; i < max; i++){
			status = spi_sync(dev->spi, &msg);
			if(status){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Unknown error while receiving SPI data");
				goto end;
			}
			m->rBuf[i] = dev->rx_buf[0];
		}

		if(m->useACK){//Verify that last byte in sum of receiving values equals last received byte inverted
			int sum = 0;
			for(i = 0; i < m->bytesToReturn; i++)
				sum += m->rBuf[i];

			if((~(m->rBuf[m->bytesToReturn]) & 0xFF) != (sum & 0xFF)){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Error while receiving data: Did not match verification byte");
				status = -EIO;
				goto end;
			}
		}
	}else if(m->bytesToReturn == -1){//Just keep receiving bytes until passing 25 or receiving 0. (getID)
		int count = 0;
		int sum = 0;
		ktime_t start, stop;
		transfer.delay_usecs = 20;
		while (count < 25){
			start = ktime_get();
			status = spi_sync(dev->spi, &msg);
			stop = ktime_get();
			unsigned long delta = (stop - start);
			if(delta > 5000000)
				printk(KERN_INFO "Time: %lu us\n", delta);
			if(status)
				goto end;
			if(dev->rx_buf[0] != 0){
				m->rBuf[count] = dev->rx_buf[0];
				sum += dev->rx_buf[0];
				count++;
			}else{
				m->rBuf[count + 1] = '\0';
				if(m->useACK){
					status = spi_sync(dev->spi, &msg);
					if(status){
						if(debug_level >= DEBUG_LEVEL_ERR)
							printk(KERN_ERR "Unknown error while receiving SPI data");
						goto end;
					}
					if((~(dev->rx_buf[0]) & 0xFF) != (sum & 0xFF)){
						if(debug_level >= DEBUG_LEVEL_ERR)
							printk(KERN_ERR "Error while receiving data: Did not match verification byte");
						status = -EIO;
						goto end;
					}
				}
				count = 25;
			}
		}
	}

	gpio_set_value(FRAME, 0);

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Message sent successfully");

	return 0;

	//Goto statement that ensures the FRAME is set low if an error occurs at any point
	end:
		gpio_set_value(FRAME, 0);
		return status;
}

static int piplate_probe(struct spi_device *spi){
	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Probing SPI device");

	if(!piplate_spi){//Verify there wasn't an error when allocating space for spi struct.
		piplate_spi = kzalloc(sizeof *piplate_spi, GFP_KERNEL);
	}
	if (!piplate_spi){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failed to allocate memory for spi device\n");
		return -ENOMEM;
	}

	spi->mode = SPI_MODE_0;
	piplate_spi->spi = spi;
	piplate_spi->max_speed_hz = MAX_SPEED_HZ;

	spi_set_drvdata(spi, piplate_spi);

	return 0;
}

static int piplate_remove(struct spi_device *spi){
	struct piplate_dev *dev = spi_get_drvdata(spi);

	kfree(dev);

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "SPI device removed");

	return 0;
}

static long piplate_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
	struct piplate_dev *dev = filp->private_data;

	struct message *m = kmalloc(sizeof(struct message), GFP_DMA);

	int error;

	if(copy_from_user(m, (void *)arg, sizeof(struct message))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Could not copy input from user space, possible invalid pointer");
		return -ENOMEM;
	}

	switch(cmd){
		case PIPLATE_SENDCMD:
			if(mutex_lock_interruptible(&dev->lock))
				return -EINTR;

			if((error = piplate_spi_message(dev, m))){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Failed to send message");
				mutex_unlock(&dev->lock);
				return error;
			}

			mutex_unlock(&dev->lock);
			break;
		default:
			if(debug_level >= DEBUG_LEVEL_ERR)
				printk(KERN_ERR "Invalid command");
			return -EINVAL;
			break;
	}

	m->state = 1;

	if(copy_to_user((void *)arg, m, sizeof(struct message))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failed to copy message results back to user space");
		return -ENOMEM;
	}

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
	.open = piplate_open,
	.release = piplate_release,
	.unlocked_ioctl = piplate_ioctl,
};

static int __init piplate_spi_init(void){
	int error;

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Loading module...\n");

	if((error = gpio_request(FRAME, "FRAME"))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failiure to request FRAME pin\n");
		goto end;
	}

	if((error = gpio_request(ACK, "ACK"))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failiure to request ACK pin\n");
		goto free_frame;
	}

	if(gpio_direction_input(ACK) || gpio_direction_output(FRAME, 0)){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Can't set input/output mode for pins\n");
		error = -EPERM;
		goto free_ack;
	}

	if(!(piplate_spi = kzalloc(sizeof(*piplate_spi), GFP_KERNEL))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Can't allocate memory for spi device");
		error = -ENOMEM;
		goto free_ack;
	}

	mutex_init(&piplate_spi->lock);

	if((error = alloc_chrdev_region(&piplate_spi_num, 0, 1, DEV_NAME))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Error while allocating device number\n");
		goto free_mem;
	}

	if(!(piplate_spi_class = class_create(THIS_MODULE, DEV_NAME))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Error while creating device class\n");
		error = -ENOMEM;
		goto unregister_chrdev;
	}

	if(!(piplate_spi_dev = device_create(piplate_spi_class, NULL, piplate_spi_num, NULL, "%s", DEV_NAME))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Error while creating device\n");
		error = -ENOMEM;
		goto destroy_class;
	}

	if((error = spi_register_driver(&piplate_driver))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Error while registering driver\n");
		goto destroy_dev;
	}

	if(!(piplate_spi_cdev = cdev_alloc())){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Error allocating memory for cdev struct\n");
		error = -ENOMEM;
		goto unregister_spi;
	}

	piplate_spi_cdev->owner = THIS_MODULE;
	piplate_spi_cdev->ops = &piplate_fops;

	if((error = cdev_add(piplate_spi_cdev, piplate_spi_num, 1))){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failed to add cdev object\n");
		goto unregister_spi;
	}

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_INFO "Sucessfully loaded module");

	return 0;

	unregister_spi:
		spi_unregister_driver(&piplate_driver);
	destroy_dev:
		device_destroy(piplate_spi_class, piplate_spi_num);
	destroy_class:
		class_destroy(piplate_spi_class);
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
	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_INFO "Unloading module...");

	cdev_del(piplate_spi_cdev);
	spi_unregister_driver(&piplate_driver);
	device_destroy(piplate_spi_class, piplate_spi_num);
	class_destroy(piplate_spi_class);
	unregister_chrdev_region(piplate_spi_num, 1);
	kfree(piplate_spi);
	gpio_free(ACK);
	gpio_free(FRAME);

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_INFO "Sucessfully unloaded module");
}

module_init(piplate_spi_init);
module_exit(piplate_spi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tyler Stowell");
MODULE_DESCRIPTION("Module to control Pi Plates with SPI protocol");
MODULE_SUPPORTED_DEVICE(DEV_NAME);
