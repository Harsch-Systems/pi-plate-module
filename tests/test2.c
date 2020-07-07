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

	struct message m = BASE_MESSAGE;
	m.addr = 43;//24 = relay, 43 = thermo
	m.cmd = 0x01;
	m.bytesToReturn = -1;
	m.useACK = 1;

	while(1){
		printf("Return: %d\n", ioctl(fileno(fp), PIPLATE_SENDCMD, &m));
		printf("Result: %s\n", m.rBuf);
		if(strcmp("Pi-Plate THERMOplate", m.rBuf))//Pi-Plate RELAY, Pi-Plate THERMOplate
			break;
	}

	printf("Error");

	fclose(fp);

	return 0;
}
