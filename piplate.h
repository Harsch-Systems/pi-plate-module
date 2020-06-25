#define BUF_SIZE 128
#define DEV_NAME "PiPlates"
#define MAX_SPEED_HZ 500000

#define FRAME 25
#define ACK 23

//Ioctl Definitions:
#define PIPLATE_IOC_MAG 'Q'

#define PIPLATE_GETADDR _IOWR(PIPLATE_IOC_MAG, 0, int)
#define PIPLATE_GETID _IOWR(PIPLATE_IOC_MAG, 1, int)
