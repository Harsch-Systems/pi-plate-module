#ifndef PIPLATE_H_INCLUDED
#define PIPLATE_H_INCLUDED

#include <stdbool.h>
#include <linux/ioctl.h>

#define BUF_SIZE 128
#define DEV_NAME "PiPlates"
#define MAX_SPEED_HZ 500000

//Ioctl Definitions:
#define PIPLATE_IOC_MAG 'Q'

#define PIPLATE_SENDCMD _IOWR(PIPLATE_IOC_MAG, 0, int)

struct message {
	unsigned char addr;
	unsigned char cmd;
	unsigned char p1;
	unsigned char p2;
	unsigned char rBuf[BUF_SIZE];
	int bytesToReturn;
	bool useACK;
	bool state;
};

const struct message BASE_MESSAGE = {
	.addr = 0,
	.cmd = 0,
	.p1 = 0,
	.p2 = 0,
	.bytesToReturn = 0,
	.useACK = 0,
	.state = 0,
};

#endif /* PIPLATE_H_INCLUDED */
