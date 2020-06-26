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

//static int use_debug_mode = 0;
//module_param(use_debug_mode, bool, S_IRUGO);

MODULE_LICENSE("Dual BSD/GPL");

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
	{ .compatible = "piplate" },
	{},
};

MODULE_DEVICE_TABLE(of, piplate_dt_ids);

static int piplate_open(struct inode *inode, struct file *filp){
	struct piplate_dev *dev = piplate_spi;

	printk(KERN_INFO "Opening file\n");

	filp->private_data = dev;

	return 0;
}

static int piplate_release(struct inode *inode, struct file *filp){
	filp->private_data = NULL;

	return 0;
}

static int piplate_spi_message(struct piplate_dev *dev, struct message *m, int bytesToReturn){
	dev->tx_buf[0] = m->addr;
	dev->tx_buf[1] = m->cmd;
	dev->tx_buf[2] = m->p1;
	dev->tx_buf[3] = m->p2;

	struct spi_transfer transfer = {
		.tx_buf = &dev->tx_buf,
		.len = 4,
		.speed_hz = dev->max_speed_hz,
		.delay_usecs = (m->useACK ? 5 : 60),
	};

	struct spi_message msg = { };
	int status;
	unsigned long j0;

	spi_message_init(&msg);
	spi_message_add_tail(&transfer, &msg);

	//Confirm ACK bit is high
	if(m->useACK){
		j0 = jiffies;
		while(!gpio_get_value(ACK)){
			if(time_after(jiffies, j0 + (HZ/100))){
				status = -EIO;
				goto end;
			}
		}
	}

	gpio_set_value(FRAME, 1);

	status = spi_sync(dev->spi, &msg);

	if(status)
		goto end;

	transfer.tx_buf = NULL;
	transfer.len = 1;
	transfer.rx_buf = dev->rx_buf;
	if(m->useACK)
		transfer.delay_usecs = 20;

	if(bytesToReturn > 0 || bytesToReturn == -1){
		if(m->useACK){
			j0 = jiffies;
			while(gpio_get_value(ACK)){
				if(time_after(jiffies, j0 + (HZ/100))){
					status = -EIO;
					goto end;
				}
			}
		}else{
			udelay(100);
		}
	}
	if(bytesToReturn > 0){
		int i;
		int max = bytesToReturn + m->useACK;//Newer ACK pi plates include extra verification response byte
		for(i = 0; i < max; i++){
			status = spi_sync(dev->spi, &msg);
			if(status)
				goto end;
			m->rBuf[i] = dev->rx_buf[0];
		}

		if(m->useACK){
			int sum = 0;
			for(i = 0; i < bytesToReturn; i++)
				sum += m->rBuf[i];

			if((~(m->rBuf[bytesToReturn]) & 0xFF) != (sum & 0xFF)){
				status = -EIO;
				goto end;
			}
		}
	}else if(bytesToReturn == -1){
		int count = 0;
		int sum = 0;
		while (count < 25){
			status = spi_sync(dev->spi, &msg);
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
					if(status)
						goto end;
					if((~(dev->rx_buf[0]) & 0xFF) != (sum & 0xFF)){
						status = -EIO;
						goto end;
					}
				}
				count = 25;
			}
		}
	}

	gpio_set_value(FRAME, 0);

	return 0;

	end:
		gpio_set_value(FRAME, 0);
		return status;
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
	piplate_spi->max_speed_hz = MAX_SPEED_HZ;

	spi_set_drvdata(spi, piplate_spi);

	//TODO: Find available plates and save them for future reference.

	return 0;
}

static int piplate_remove(struct spi_device *spi){
	struct piplate_dev *dev = spi_get_drvdata(spi);

	kfree(dev);

	return 0;
}

static long piplate_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
	struct piplate_dev *dev = filp->private_data;

	struct message *m = kmalloc(sizeof(struct message), GFP_DMA);

	int error;

	if(copy_from_user(m, (void *)arg, sizeof(struct message)))
		return -ENOMEM;

	switch(cmd){
		case PIPLATE_SENDCMD:
			if(mutex_lock_interruptible(&dev->lock))
				return -EINTR;

			if((error = piplate_spi_message(dev, m, m->bytesToReturn))){
				mutex_unlock(&dev->lock);
				return error;
			}

			mutex_unlock(&dev->lock);
			break;
		default:
			return -EINVAL;
			break;
	}

	if(copy_to_user((void *)arg, m, sizeof(struct message)))
		return -ENOMEM;

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

	mutex_init(&piplate_spi->lock);

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

	if((registered = spi_register_driver(&piplate_driver))){
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
