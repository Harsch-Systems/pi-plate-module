#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "piplate.h"

int main() {
	FILE *fp;
	fp = fopen("/dev/PiPlates", "r");
	if(fp == NULL){
		printf("error!");
		exit(1);
	}

	struct message m = BASE_MESSAGE;
	m.addr = 24;//53 is tinker, 24 is relay, 36 is DAQC2
	m.cmd = 0x01;
	//m.p1 = 2;
	//m.p2 =
	m.bytesToReturn = -1;
	m.useACK = 0;

	int i;
	for(i = 0; i < 1000; i++){
		printf("Return: %d\n", ioctl(fileno(fp), PIPLATE_SENDCMD, &m));
		printf("Result: %s\n", m.rBuf);
	}

	fclose(fp);

	return 0;
}
