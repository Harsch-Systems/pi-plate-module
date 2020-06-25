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
	m.addr = 24;
	//m.p1 =
	//m.p2 =
	m.useACK = 0;

	printf("Return: %d\n", ioctl(fileno(fp), PIPLATE_GETADDR, &m));

	printf("Result: %s\n", m.rBuf);

	fclose(fp);

	return 0;
}
