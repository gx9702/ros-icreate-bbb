/*
 * LMS303.cpp
 *	For use with LMS303 Accelerometer as found in AltIMU-10. Note, must set dataRate to enable
 *	measurements.
 *
 *  Created on: Jul 3, 2014
 *      Author: John Boyd
 *
 *  Reference:
 *  	http://www.inmotion.pt/store/altimu10-v3-gyro-accelerometer-compass-and-altimeter-l3gd20h
 *  	http://inmotion.pt/documentation/pololu/POL-2469/LSM303D.pdf
 */

#include <ros/console.h>
#include <imu/LMS303.h>

using namespace std;

LMS303::LMS303(int bus, int address) {
	I2CBus = bus;
	I2CAddress = address;

	accelX = 0;
	accelY = 0;
	accelZ = 0;

	pitch = 0;
	roll = 0;

	reset();	// Reset device to default settings
	enableMagnetometer();
	enableAccelerometer();
	enableTempSensor();
	readFullSensorState();
}

int LMS303::reset() {
	ROS_DEBUG("Resetting LMS303 accelerometer...");
	// Reset control registers
	writeI2CDeviceByte(REG_CTRL0, 0x80);	// Reboot LMS303 memory
	writeI2CDeviceByte(REG_CTRL1, 0x00);	// Reset Accel settings
	writeI2CDeviceByte(REG_CTRL2, 0x00);	// Reset Accel settings
	writeI2CDeviceByte(REG_CTRL3, 0x00);	// Reset interrupt settings
	writeI2CDeviceByte(REG_CTRL4, 0x00);	// Reset interrupt settings
	writeI2CDeviceByte(REG_CTRL5, 0x00);	// Reset TEMP/MAG settings
	writeI2CDeviceByte(REG_CTRL6, 0x00);	// Reset MAG settings
	writeI2CDeviceByte(REG_CTRL7, 0x00);	// Reset TEMP/MAG/ACCEL settings
	writeI2CDeviceByte(REG_FIFO_CTRL, 0x00);	// Set FIFO mode to Bypass
	writeI2CDeviceByte(REG_FIFO_SRC, 0x00);	// Set FIFO mode to Bypass

	// Clear memory
	memset(dataBuffer, 0, LMS303_I2C_BUFFER);	// Clear dataBuffer
	memset(accelFIFO, 0, ACCEL_FIFO_SIZE);	// Clear accelFIFO

	sleep(1);
	ROS_DEBUG("Done.");
	return 0;
}

int LMS303::readFullSensorState() {
	/* Since this device is actually multiple sensors from different companies manufactured on
	 * one piece of silicon, the I2C communication blocks for each sensor are not identical.
	 * As such, a block read across both the beginning magnetometer registers and the accelerometer
	 * registers causes the devices to glitch and give corrupt data. The line below is a
	 * simple block read command, but with the addresses offset so that the block read doesn't
	 * try to read from any of the magnetometer addresses (registers 0x00-0x0E).
	 */
	if(accelFIFOMode == ACCEL_FIFO_STREAM) {	// Average the accel measurements stored in FIFO
		// Read the rest of the memory excluding the Accel output registers (because they will burst
		// FIFO data and ruin the burst sequence for the entire memory map.


		readI2CDevice(REG_TEMP_OUT_L, &dataBuffer[REG_TEMP_OUT_L], (REG_OUT_Z_H_M-REG_TEMP_OUT_L)+1);
		readI2CDevice(REG_WHO_AM_I, &dataBuffer[REG_WHO_AM_I], 1);
		readI2CDevice(REG_INT_CTRL_M, &dataBuffer[REG_INT_CTRL_M], (REG_STATUS_A-REG_INT_CTRL_M)+1);
		readI2CDevice(REG_FIFO_CTRL, &dataBuffer[REG_FIFO_CTRL], LMS303_I2C_BUFFER-REG_FIFO_CTRL);

		// Read accel FIFO afterwards to prevent I2C glitch
		int slotsRead = readAccelFIFO(accelFIFO);	// Read Accel FIFO
		averageAccelFIFO(slotsRead);
	}
	else {	// No accel output averaging
		readI2CDevice(REG_WHO_AM_I, &dataBuffer[REG_WHO_AM_I], LMS303_I2C_BUFFER-REG_WHO_AM_I);

		accelX = convertAcceleration(REG_OUT_X_H_A, REG_OUT_X_L_A);
		accelY = convertAcceleration(REG_OUT_Y_H_A, REG_OUT_Y_L_A);
		accelZ = convertAcceleration(REG_OUT_Z_H_A, REG_OUT_Z_L_A);
	}

	// Check WHO_AM_I register, to make sure I am reading from the registers I think I am.
	if (dataBuffer[REG_WHO_AM_I]!=0x49){
		ROS_ERROR("MAJOR FAILURE: DATA WITH LMS303 HAS LOST SYNC!");
		return (1);
	}

	getTemperature();

	magX = convertMagnetism(REG_OUT_X_H_M, REG_OUT_X_L_M);
	magY = convertMagnetism(REG_OUT_Y_H_M, REG_OUT_Y_L_M);
	magZ = convertMagnetism(REG_OUT_Z_H_M, REG_OUT_Z_L_M);

	calculatePitchAndRoll();

	return(0);
}

int LMS303::enableTempSensor() {
	char buf[1] = {0x00};
	readI2CDevice(REG_CTRL5, buf, 1);	// Read current register state
	buf[0] |= 0x80;	// Set TEMP_EN bit

	if(writeI2CDeviceByte(REG_CTRL5, buf[0])) {	// Write back to register
		ROS_ERROR("ERROR: Failed to enable temperature sensor.");
		return 1;
	}

	return 0;
}

float LMS303::getTemperature() {
	// Read temperature registers directly into dataBuffer
	// Datasheet is not clear, so temp conversion may be inaccurate.
	// Not verified with negative temperatures;

	short temp = ((dataBuffer[REG_TEMP_OUT_H] << 8) | (dataBuffer[REG_TEMP_OUT_L])) ;
    
    //type short is 16bit 2s comp and the temp is given in 12bit 2s comp
    //we must mask the MSB, flip the bits if needed and remember the sign
    // to convert correctly
    float sign = 1.0;

    if(temp & 0x0800) {
        temp = temp ^ 0x0FFF;
        sign = -1.0;
    }
    else
    {
        temp &= 0x0FFF;
    }

    //Factory calibrated to 25 celsius
	celsius = sign*(float)(temp / 8.0) + 25;
	return celsius;
}

int LMS303::enableMagnetometer() {
	setMagDataRate(DR_MAG_100HZ);	// Set dataRate to enable device.
	setMagScale(SCALE_MAG_8gauss);	// Set accelerometer SCALE

	char buf[1] = {0x00};
	readI2CDevice(REG_CTRL7, buf, 1);	// Read current register state
	buf[0] &= 0xF8;		// Clear low-power bit and mode bits
	if(writeI2CDeviceByte(REG_CTRL7, buf[0])) {
		ROS_ERROR("Failed to enable magnetometer!");
		return 1;
	}

	return 0;
}

int LMS303::setMagScale(LMS303_MAG_SCALE scale) {	// Set magnetometer output rate
	char buf = (char)scale << 5;		// Clear low-power bit and mode bits
	buf &= 0x60;	// Ensure protected bits are not written to
	if(writeI2CDeviceByte(REG_CTRL6, buf)) {
		ROS_ERROR("Failed to set magnetometer scale!");
		magScale = 0;
		return 1;
	}

	switch(scale){
	case SCALE_MAG_2gauss: {
		magScale = .00008;
		break;
	}
	case SCALE_MAG_4gauss: {
		magScale = .00016;
		break;
	}
	case SCALE_MAG_8gauss: {
		magScale = .00032;
		break;
	}
	case SCALE_MAG_12gauss: {
		magScale = .000479;
		break;
	}
	default: {
		ROS_ERROR("ERROR! Invalid magnetometer scale.");
		magScale = 0;
		return 1;
		break;
	}
	}
	return 0;
}

int LMS303::setMagDataRate(LMS303_MAG_DATA_RATE dataRate) {	// Set magnetometer SCALE
	char buf[1] = {0x00};
	readI2CDevice(REG_CTRL5, buf, 1);	// Read current register state
	buf[0] &= 0x83;		// Clear resolution and dataRate bits
	buf[0] |= 0x60;	// Set resolution bits to high resolution
	buf[0] |= (char)dataRate << 2;	// Set dataRate bits
	if(writeI2CDeviceByte(REG_CTRL5, buf[0])) {	// write back to ctrl register
		ROS_ERROR("Failed to set magnetometer dataRate!");
		return 1;
	}
	return 0;
}

float LMS303::convertMagnetism(int msb_reg_addr, int lsb_reg_addr){
	short temp = dataBuffer[msb_reg_addr];
	temp = (temp<<8) | dataBuffer[lsb_reg_addr];
	return ((float)temp * magScale);	// Convert to gauss
}

int LMS303::enableAccelerometer() {
	setAccelDataRate(DR_ACCEL_16OOHZ);	// Set dataRate to enable device.
	setAccelScale(SCALE_ACCEL_8g);	// Set accelerometer SCALE
	setAccelFIFOMode(ACCEL_FIFO_STREAM);	// Enable FIFO for easy output averaging

	char buf[1] = {0x00};
	readI2CDevice(REG_CTRL1, buf, 1);	// Read current register state
	buf[0] |= 0x07;		// Set X,Y,Z enable bits

	if(writeI2CDeviceByte(REG_CTRL1, buf[0])!=0){
			ROS_ERROR("Failure to enable accelerometer!");
			return 1;
	}
	return 0;
}

int LMS303::setAccelScale(LMS303_ACCEL_SCALE scale) {
	char buf[1];
	readI2CDevice(REG_CTRL2, buf, 1);	// Read current value
	buf[0] &= 0b11000111;	// Clear scale bits
	buf[0] |= (char)scale << 3;	// Set new scale bits
	if(writeI2CDeviceByte(REG_CTRL2, buf[0])) {	// Set accelerometer SCALE
		ROS_ERROR("Failed to set accelerometer scale!");
		accelScale = 0;
		return 1;
	}

	switch(scale){
	case SCALE_ACCEL_2g: {
		accelScale = .000061;
		break;
	}
	case SCALE_ACCEL_4g: {
		accelScale = .000122;
		break;
	}
	case SCALE_ACCEL_6g: {
		accelScale = .000183;
		break;
	}
	case SCALE_ACCEL_8g: {
		accelScale = .000244;
		break;
	}
	case SCALE_ACCEL_16g: {
		accelScale = .000732;
		break;
	}
	default: {
		ROS_ERROR("ERROR! Invalid accelerometer scale.");
		accelScale = 0;
		return 1;
		break;
	}
	}
	return 0;
}

void LMS303::calculatePitchAndRoll() {
	double accelXSquared = this->accelX * this->accelX;
	double accelYSquared = this->accelY * this->accelY;
	double accelZSquared = this->accelZ * this->accelZ;
	this->pitch = 180 * atan(accelX/sqrt(accelYSquared + accelZSquared))/M_PI;
	this->roll = 180 * atan(accelY/sqrt(accelXSquared + accelZSquared))/M_PI;
}

float LMS303::convertAcceleration(int msb_reg_addr, int lsb_reg_addr){
	short temp = dataBuffer[msb_reg_addr];
	temp = (temp<<8) | dataBuffer[lsb_reg_addr];
	return ((float)temp * accelScale);	// Convert to g's
}

float LMS303::convertAcceleration(int accel) {
	return ((float)accel * accelScale);	// Convert to g's
}

int LMS303::setAccelDataRate(LMS303_ACCEL_DATA_RATE dataRate){

	char buf[1];
	readI2CDevice(REG_CTRL1, buf, 1);	// Read current value
	buf[0] &= 0x0F;	// Clear scale bits
	buf[0] |= (char)dataRate << 4;	// Set new scale bits

	if(writeI2CDeviceByte(REG_CTRL1, buf[0])!=0){
		ROS_ERROR("Failure to update dataRate value!");
		return 1;
	}
	return 0;
}

LMS303_ACCEL_DATA_RATE LMS303::getAccelDataRate(){

	char buf[1];
	if(readI2CDevice(REG_CTRL1, buf, 1)!=0){
		ROS_ERROR("Failure to read dataRate value");
		return DR_ACCEL_ERROR;
	}

	return (LMS303_ACCEL_DATA_RATE)buf[0];
}

int LMS303::writeI2CDeviceByte(char address, char value) {
	char namebuf[MAX_BUS];
	snprintf(namebuf, sizeof(namebuf), "/dev/i2c-%d", I2CBus);
	int file;
	if ((file = open(namebuf, O_RDWR)) < 0){
		ROS_ERROR("Failed to open LMS303 Sensor on %s I2C Bus", namebuf);
		return(1);
	}
	if (ioctl(file, I2C_SLAVE, I2CAddress) < 0){
		ROS_ERROR("I2C_SLAVE address %d failed...", I2CAddress);
		return(2);
	}

	char buffer[2];
	buffer[0] = address;
	buffer[1] = value;
	if ( write(file, buffer, 2) != 2) {
		ROS_ERROR("Failure to write values to I2C Device address.");
		return(3);
	}
	close(file);
	return 0;
}

int LMS303::readI2CDevice(char address, char data[], int size){
    char namebuf[MAX_BUS];
   	snprintf(namebuf, sizeof(namebuf), "/dev/i2c-%d", I2CBus);
    int file;
    if ((file = open(namebuf, O_RDWR)) < 0){
            ROS_ERROR("Failed to open LMS303 Sensor on %s I2C Bus", namebuf);
            return(1);
    }
    if (ioctl(file, I2C_SLAVE, I2CAddress) < 0){
            ROS_ERROR("I2C_SLAVE address 0x1d failed...");
            return(2);
    }

    // According to the LMS303 datasheet on page 22, to read from the device, we must first
    // send the address to read from in write mode. The MSB of the address must be 1 to enable "block reading"
    // then we may read as many bytes as desired.

    char temp = address;
    if(size > 1) temp |= 0b10000000;	// Set MSB to enable burst if reading multiple bytes.
    char buf[1] = { temp };
    if(write(file, buf, 1) !=1){
    	ROS_ERROR("Failed to set address to read from in readFullSensorState() ");
    }

    if ( read(file, data, size) != size) {
        ROS_ERROR("Failure to read value from I2C Device address.");
    }

    close(file);
    return 0;
}

int LMS303::setAccelFIFOMode(LMS303_ACCEL_FIFO_MODE mode) {
	char val[1] = {0x00};
	readI2CDevice(REG_CTRL0, val, 1);	// Read current CTRL0 register


	switch (mode) {
	case ACCEL_FIFO_BYPASS: {
		val[0] = 0x00;		// Leave FIFO mode bits as 0x00 for bypass mode
		break;
	}
	case ACCEL_FIFO_STREAM: {
		val[0] = 0x40;
		writeI2CDeviceByte(REG_CTRL0, (val[0] | 0x40) );	// Enable FIFO bit in CTRL0 register
		break;
	}
	default: {
		val[0] = 0x00;	// Same as bypass mode
		break;
	}
	}
	writeI2CDeviceByte(REG_FIFO_CTRL, val[0]);

	if(getAccelFIFOMode() != mode){
		ROS_ERROR("Error setting LMS303 Accelerometer mode!");
	}
	else accelFIFOMode = mode;
	return 0;
}

LMS303_ACCEL_FIFO_MODE LMS303::getAccelFIFOMode() {
	char val[1] = { 0x00 };
	readI2CDevice(REG_FIFO_CTRL, val, 1);	// Read current FIFO mode

	switch (val[0]) {
	case 0x00: return ACCEL_FIFO_BYPASS;
		break;
	case 0x40: return ACCEL_FIFO_STREAM;
		break;
	default: return ACCEL_FIFO_ERROR;
		break;
	}

	return ACCEL_FIFO_ERROR;
}

int LMS303::readAccelFIFO(char buffer[]) {
	char val[1] = { 0x00 };
	readI2CDevice(REG_FIFO_SRC, val, 1);	// Read current FIFO mode
	val[0] &= 0x0F;	// Mask all but FIFO slot count bits

	readI2CDevice(REG_OUT_X_L_A, accelFIFO, ACCEL_FIFO_SIZE);	// Read current FIFO mode

	return (int)val[0]+1;	// Return the number of FIFO slots that held new data
}

int LMS303::averageAccelFIFO(int slots){
	if(slots <= 0) {
		ROS_ERROR("Error! Divide by 0 in averageAccelFIFO()!");
		return 1;
	}

	int sumX = 0;
	int sumY = 0;
	int sumZ = 0;

	for(int i=0; i<slots; i++) {
		// Reset temp variables
		short tempX = 0x0000;
		short tempY = 0x0000;
		short tempZ = 0x0000;

		// Convert 2's compliment for X, Y & Z
		tempX = this->accelFIFO[(i*6)+1];
		tempX = (tempX << 8) | this->accelFIFO[i*6];
		tempX = ~tempX + 1;

		tempY = this->accelFIFO[(i*6)+3];
		tempY = (tempY << 8) | this->accelFIFO[(i*6)+2];
		tempY = ~tempY + 1;

		tempZ = this->accelFIFO[(i*6)+5];
		tempZ = (tempZ << 8) | this->accelFIFO[(i*6)+4];
		tempZ = ~tempZ + 1;


		// Sum X, Y and Z outputs
		sumX += (int)tempX;
		sumY += (int)tempY;
		sumZ += (int)tempZ;
	}

	accelX = convertAcceleration(sumX / slots);
	accelY = convertAcceleration(sumY / slots);
	accelZ = convertAcceleration(sumZ / slots);

	return 0;
}

imu::Vector<3> LMS303::read_acc() {
	imu::Vector<3> acc(accelX, accelY, accelZ);
	return acc;
}

imu::Vector<3> LMS303::read_mag() {
	imu::Vector<3> mag(magX,magY,magZ);
	return mag;
}

LMS303::~LMS303() {
	// TODO Auto-generated destructor stub
}
