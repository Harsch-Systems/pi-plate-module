#ifndef PIPLATE_H_INCLUDED
#define PIPLATE_H_INCLUDED

#include <stdbool.h>
#include <linux/ioctl.h>

#define BUF_SIZE 128
#define DEV_NAME "PiPlates"
#define MAX_SPEED_HZ 500000

#define FRAME 25
#define ACK 23

//Ioctl Definitions:
#define PIPLATE_IOC_MAG 'Q'

#define PIPLATE_GETADDR _IOWR(PIPLATE_IOC_MAG, 0, int)
#define PIPLATE_GETID _IOWR(PIPLATE_IOC_MAG, 1, int)

struct message {
	unsigned char addr;
	unsigned char p1;
	unsigned char p2;
	unsigned char rBuf[BUF_SIZE];
	bool useACK;
};

const struct message BASE_MESSAGE = {
	.addr = 0,
	.p1 = 0,
	.p2 = 0,
	.rBuf = NULL,
	.useACK = 0,
};

#endif /* PIPLATE_H_INCLUDED */
