#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>

#include "piplate.h"

#define FRAME 25
#define ACK 23

//If a transfer ends up sleeping when holding chip select and exceeds this time, it needs to retry. (nanoseconds)
#define TIME_MAX 1300000

//Maximum string length returned by getID().
#define MAX_ID_LEN 25

//The amount of time it takes the plates to reset in microseconds.
#define PLATE_CHILL 80

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
	unsigned char rx_buf[BUF_SIZE];
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
		printk(KERN_DEBUG "Pi Plate File Opened\n");

	return 0;
}

static int piplate_release(struct inode *inode, struct file *filp){
	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Pi Plate File Released\n");

	filp->private_data = NULL;

	return 0;
}


/*
  Sends a message to the pi plate given the information in message m.
  m has already been copied from user space in ioctl, so this does
  not need to worry about user space access.
*/
static int piplate_spi_message(struct piplate_dev *dev, struct message *m){
	struct spi_message tx_msg = { };
	struct spi_transfer tx_transfer = { };
	int status;
	unsigned long j0;
	ktime_t t0;
	int attempts = MAX_ATTEMPTS + 1;

	//To allow it to retry if the transfer fails.
	start:
		if(attempts < MAX_ATTEMPTS){
			printk(KERN_INFO "Restaring...\n");
			gpio_set_value(FRAME, 0);
			udelay(PLATE_CHILL);
			if(attempts <= 0){
				printk(KERN_INFO "Done.\n");
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Ran out of attempts due to multiple timing failures\n");
				return -EIO;
			}
		}
		attempts--;

	dev->tx_buf[0] = m->addr;
	dev->tx_buf[1] = m->cmd;
	dev->tx_buf[2] = m->p1;
	dev->tx_buf[3] = m->p2;

	tx_transfer.tx_buf = &dev->tx_buf;
	tx_transfer.len = 4;
	tx_transfer.speed_hz = dev->max_speed_hz;
	tx_transfer.delay_usecs = 40;

	spi_message_init(&tx_msg);
	spi_message_add_tail(&tx_transfer, &tx_msg);

	if(debug_level == DEBUG_LEVEL_ALL){
		if(m->useACK)
			printk(KERN_DEBUG "Sending command to Pi Plate using ACK wire (TINKERplate, DAQC2plate, THERMOplate)\n");
		else
			printk(KERN_DEBUG "Sending command to Pi Plate without ACK wire (DAQCplate, RELAYplate, MOTORplate)\n");
		printk(KERN_DEBUG "Address: %d\n", m->addr);
		printk(KERN_DEBUG "Command number: %d\n", m->cmd);
	}

	//Confirm ACK bit is high before transfer begins
	if(m->useACK){
		j0 = jiffies;
		while(!gpio_get_value(ACK)){
			if(time_after(jiffies, j0 + (HZ/100))){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Timed out while confirming ACK bit high\n");
				status = -EIO;
				goto end;
			}
		}
	}

	gpio_set_value(FRAME, 1);

	t0 = ktime_get();

	status = spi_sync(dev->spi, &tx_msg);

	if(ktime_get() - t0 > TIME_MAX)//If this process slept for a long time during spi_sync, it needs to restart.
		goto start;

	if(status){
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Unknown error while sending SPI command\n");
		goto end;
	}

	if(m->bytesToReturn){
		int count = 0;

		//Handles different delay systems. If using ACK, wait for that to go low. Otherwise, delay for 100us.
		if(m->useACK){
			j0 = jiffies;
			while(gpio_get_value(ACK)){
				if(time_after(jiffies, j0 + (HZ/100))){
					if(debug_level >= DEBUG_LEVEL_ERR)
						printk(KERN_ERR "Timed out while waiting for ACK bit low\n");
					status = -EIO;
					goto end;
				}
			}
		}else{
			udelay(85);
		}

		if(!m->useACK){
			//Creates the receiving transfers. Each byte is a transfer in one message because that allows it to be atomic.
			//m->useACK case is done differently because this method is more accurate, but doesn't work for them.
			struct spi_message rx_msg = { };
			struct spi_transfer rx_transfer = { };

			spi_message_init(&rx_msg);

			rx_transfer.len = 1;
			rx_transfer.delay_usecs = 3;
			rx_transfer.rx_buf = &dev->rx_buf;
			rx_transfer.speed_hz = dev->max_speed_hz;

			spi_message_add_tail(&rx_transfer, &rx_msg);

			if(m->bytesToReturn > 0){
				for(count = 0; count < m->bytesToReturn; count++){
					status = spi_sync(dev->spi, &rx_msg);
					if(status)
						goto end;

					/*
					* If an error has occured and the plate can't give a response, the MISO bus
					* will stay high and register 0xFF. Restarting based of this can greatly improve
					* accuracy, but it is important to note that there are a handful of functions that
					* might return this normally. So, we only restart if we haven't before.
					*/
					if((dev->rx_buf[0] & 0xFF) == 0xFF && attempts == MAX_ATTEMPTS)
						goto start;

					m->rBuf[count] = dev->rx_buf[0];
					udelay(75);
				}
			}else{
				while(count < 25){
					status = spi_sync(dev->spi, &rx_msg);
					if(status)
						goto end;

					if(dev->rx_buf[0] >= 0x7F)//It didn't receive a valid ASCII character
						goto start;

					if(dev->rx_buf[0] != '\0'){
						m->rBuf[count] = dev->rx_buf[0];
						count ++;
					}else{
						m->rBuf[count + 1] = '\0';
						count = 25;
					}
					udelay(75);
				}
			}
		}else{
			int sum = 0;
			int verifier = 0;

			struct spi_message rx_msg = { };
			struct spi_transfer rx_transfer = { };

			int rx_len = ((m->bytesToReturn >= 0) ? m->bytesToReturn : MAX_ID_LEN) + 1;

			spi_message_init(&rx_msg);

			rx_transfer.len = 1;
			rx_transfer.delay_usecs = 20;
			rx_transfer.rx_buf = &dev->rx_buf;
			rx_transfer.speed_hz = dev->max_speed_hz;

			spi_message_add_tail(&rx_transfer, &rx_msg);


			if(m->bytesToReturn > 0){
				for(count = 0; count < rx_len; count++){
					status = spi_sync(dev->spi, &rx_msg);
					if(status){
						if(debug_level >= DEBUG_LEVEL_ERR)
							printk(KERN_ERR "Unknown error while receiving SPI data\n");
						goto end;
					}
					m->rBuf[count] = dev->rx_buf[0];
					if(count < rx_len - 1)
						sum += dev->rx_buf[0];
				}

				verifier = dev->rx_buf[0];
			}else{
				while(count < rx_len){
					status = spi_sync(dev->spi, &rx_msg);
					if(status){
						if(debug_level >= DEBUG_LEVEL_ERR)
							printk(KERN_ERR "Unknown error while receiving SPI data\n");
						goto end;
					}

					if(dev->rx_buf[0] != 0){
						m->rBuf[count] = dev->rx_buf[0];
						sum += dev->rx_buf[0];
						count++;
					}else{
						m->rBuf[count + 1] = '\0';

						status = spi_sync(dev->spi, &rx_msg);

						if(status){
							if(debug_level >= DEBUG_LEVEL_ERR)
				 				printk(KERN_ERR "Unknown error while receiving SPI data\n");
							goto end;
						}

						verifier = dev->rx_buf[0];

						count = rx_len;
					}
				}
			}

			if((~verifier & 0xFF) != (sum & 0xFF)){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Error while receiving data: Did not match verification byte\n");
				status = -EIO;
				goto end;
			}
		}
	}

	gpio_set_value(FRAME, 0);

	udelay(PLATE_CHILL);

	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Message sent successfully\n");

	return 0;

	//Goto statement that ensures the FRAME is set low if an error occurs at any point
	end:
		gpio_set_value(FRAME, 0);
		return status;
}

static int piplate_probe(struct spi_device *spi){
	if(debug_level == DEBUG_LEVEL_ALL)
		printk(KERN_DEBUG "Probing SPI device\n");

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
		printk(KERN_DEBUG "SPI device removed\n");

	return 0;
}

static long piplate_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
	struct piplate_dev *dev = filp->private_data;

	struct message *m;

	int error;

	if(!(m = kmalloc(sizeof(struct message), GFP_DMA))){
		return -ENOMEM;
	}

	if(mutex_lock_interruptible(&dev->lock))
		return -EINTR;

	if(copy_from_user(m, (void *)arg, sizeof(struct message))){
		mutex_unlock(&dev->lock);
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Could not copy input from user space, possible invalid pointer\n");
		return -ENOMEM;
	}

	switch(cmd){
		case PIPLATE_SENDCMD:
			if((error = piplate_spi_message(dev, m))){
				if(debug_level >= DEBUG_LEVEL_ERR)
					printk(KERN_ERR "Failed to send message\n");
				mutex_unlock(&dev->lock);
				return error;
			}

			break;
		default:
			mutex_unlock(&dev->lock);
			if(debug_level >= DEBUG_LEVEL_ERR)
				printk(KERN_ERR "Invalid command\n");
			return -EINVAL;
			break;
	}

	m->state = 1;

	if(copy_to_user((void *)arg, m, sizeof(struct message))){
		mutex_unlock(&dev->lock);
		if(debug_level >= DEBUG_LEVEL_ERR)
			printk(KERN_ERR "Failed to copy message results back to user space\n");
		return -ENOMEM;
	}

	mutex_unlock(&dev->lock);

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
