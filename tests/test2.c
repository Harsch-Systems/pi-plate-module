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

	unsigned char address = 53;
	bool ack = 1;

	struct message m = BASE_MESSAGE;
	m.addr = address;//24 = relay, 9 = DAQC, 19 = MOTOR, 43 = thermo, 36 = DAQC2, 53 = TINKER
	m.cmd = 0x00;
	m.bytesToReturn = 1;
	m.useACK = ack;

	while(1){
		printf("Return: %d\n", ioctl(fileno(fp), PIPLATE_SENDCMD, &m));
		printf("Result: %d\n", m.rBuf[0]);
		if(m.rBuf[0] != address)//Pi-Plate RELAY, Pi-Plate THERMOplate, Pi-Plate DAQC2plate
			break;

		successes++;
	}

	printf("Error, made it through %d successes\n", successes);

	fclose(fp);

	return 0;
}
