#define BUF_SIZE 128
#define DEV_NAME "PiPlates"
#define MAX_SPEED_HZ 5000000

#define FRAME 25
#define ACK 23

//Ioctl Definitions:
#define IOC_MAG 'Q'

struct piplate_dev {
	struct spi_device *spi;
	unsigned char tx_buf[4];
	unsigned char rx_buf[BUF_SIZE];
	unsigned int max_speed_hz;
	spinlock_t spinlock;
	bool opened;
};
