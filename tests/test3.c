#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>

#include "../module/piplate.h"

int main() {
	FILE *fp;
	fp = fopen("/dev/PiPlates", "r");
	if(fp == NULL){
		printf("error!");
		exit(1);
	}

	int successes = 0;

	unsigned char address = 43;
	bool ack = 1;

	struct message m = BASE_MESSAGE;
	m.addr = address;//24 = relay, 9 = DAQC, 19 = MOTOR, 43 = thermo, 36 = DAQC2, 53 = TINKER
	m.cmd = 0x00;
	m.p1 = 0;
	m.bytesToReturn = 1;
	m.useACK = ack;

	struct message m2 = BASE_MESSAGE;
	m2.addr = address;
	m2.cmd = 0x00;
	m2.bytesToReturn = 1;
	m2.useACK = ack;

	while(1){
		ioctl(fileno(fp), PIPLATE_SENDCMD, &m);
		printf("Result: %d, %d\n", m.rBuf[0], m.rBuf[1]);
//		if(strcmp("Pi-Plate DAQC", m.rBuf))//Pi-Plate RELAY, Pi-Plate THERMOplate, Pi-Plate DAQC2plate
//			break;

//		if(m.rBuf[0] != address)
//			break;

		successes++;
	}

	printf("Error, made it through %d successes\n", successes);

	fclose(fp);

	return 0;
}
