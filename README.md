# pi-plate-module

Using this device
-----------------

This driver is designed for the raspberry pi
"Pi-Plates" product. In order to use it, you
must have either the DAQCplate, DAQC2plate,
TINKERplate, THERMOplate, RELAYplate, or
MOTORplate attached to the raspberry pi.
To use the driver, send the ioctl command
"PIPLATE_SENDCMD" with the message struct as
an input:

struct message {  
	unsigned char addr;  
	unsigned char cmd;  
	unsigned char p1;  
	unsigned char p2;  
	unsigned char rxBuf[BUF_SIZE];  
	int bytesToReturn;  
	bool useACK;  
	bool state;  
}  

The plates are designed in one of
two ways: The DAQC2plate, TINKERplate, and
THERMOplate all use an extra pin (#23) in
order to acknowledge when they are ready to
send a response. The other three plates do
not. For this reason, the message struct
contains an extra parameter useACK in order
to specify the type of plate.
After a successful transfer, state will be set
to true and rxBuf will be filled with the
result.

Device Tree Overlay
-------------------

Use the device tree compiler (dtc) to compile the .dts file into a .dtbo file.  
dtc -@ -I dts -O dtb -o piplate.dtbo piplate_overlay.dts  

Then move the piplate.dtbo file into /boot/overlays  
sudo mv piplate.dtbo /boot/overlays/piplate.dtbo  

Finally, tell the kernel to use the device tree binary by adding dtoverlay=piplate to the end of /boot/config.txt and restart the pi.

Inserting the module
--------------------

When inserting the module, there is one optional module parameter to set, debug_level.
By default, any errrors will be logged by the kernel. Specifying debug_level=0 causes
nothing to be logged, and specifying debug_level=2 will cause the module to also log
debug information during normal execution.
