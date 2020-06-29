#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "../module/piplate.h"

int main() {
	FILE *fp;
	fp = fopen("/dev/PiPlates", "r");
	if(fp == NULL){
		printf("error!");
		exit(1);
	}

	struct message m = BASE_MESSAGE;
	m.addr = 24;
	m.cmd = 0x00;
	m.bytesToReturn = 1;
	m.useACK = 0;

	printf("Return: %d\n", ioctl(fileno(fp), PIPLATE_SENDCMD, &m));
	printf("Result: %d\n", m.rBuf[0]);

	fclose(fp);

	return 0;
}
