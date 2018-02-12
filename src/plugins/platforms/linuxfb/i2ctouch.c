#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/i2c-dev.h>
#include "i2cbusses.h"
#include "util.h"

int main(int argc, char *argv[])
{
	int i2cbus, address, file;
	int pec = 0;
	int force = 0;
	char filename[20];

	force = 0;
    i2cbus = 4;
    address = 0x38;
    
	file = open_i2c_dev(i2cbus, filename, sizeof(filename), 0);
	if (file < 0
	 || set_slave_addr(file, address, force))
		exit(1);

	if (pec && ioctl(file, I2C_PEC, 1) < 0) {
		fprintf(stderr, "Error: Could not set PEC: %s\n",
			strerror(errno));
		close(file);
		exit(1);
	}

	while (1)
	{
	int touch  = i2c_smbus_read_byte_data(file, 2);
	if (touch)
	{
		int y = i2c_smbus_read_byte_data(file, 4) + 256 *(i2c_smbus_read_byte_data(file,  3) & 15);
		int x = i2c_smbus_read_byte_data(file, 6) + 256 *i2c_smbus_read_byte_data(file,  5);
		
		printf("touch %d %d\n", x, y);

	}
	}
	
	close(file);
	
	exit(0);
}
