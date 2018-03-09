//
// Copyright (C) 2014 Jens Korinth, TU Darmstadt
//
// This file is part of Tapasco (TPC).
//
// Tapasco is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Tapasco is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Tapasco.  If not, see <http://www.gnu.org/licenses/>.
//
//! @file	memcheck-mt-ff.cc
//! @brief	Initializes the first TPC device and iterates over a number
//!  		of integer arrays of increasing size, allocating each array
//!  		on the device, copying to and from and then checking the
//!   		results. Basic regression test for platform implementations.
//!		Single-threaded variant.
//! @author	J. Korinth, TU Darmstadt (jk@esa.cs.tu-darmstadt.de)
//!
#include <stack>
#include <iostream>
#include <mutex>
#include <chrono>
#include <bitset>
#include <unistd.h>
#include <chrono>
#include <random>

#include <tapasco.hpp>
#include <platform.h>
#include "pcg-cpp-0.98/include/pcg_random.hpp"

using namespace std;
using namespace tapasco;

bool getVal(uint32_t v, uint32_t o) {
	return (v >> o) & 1;
}

void setVal(uint32_t &v, uint32_t n, uint32_t x) {
	v ^= (-x ^ v) & (1 << n);
}

const uint64_t iic_base = 0x00400000;
const uint64_t control_register = iic_base + 0x100;
const uint64_t status_register = iic_base + 0x104;
const uint64_t rx_fifo_pirq = iic_base + 0x120;
const uint64_t fifo_tx_register = iic_base + 0x108;
const uint64_t fifo_rx_register = iic_base + 0x10C;
const uint64_t isr_register = iic_base + 0x020;
const uint64_t gpo_register = iic_base + 0x124;
const uint64_t reset_register = iic_base + 0x040;
const uint64_t reset_key = 0xA;

#define SWITCH_ADDR 0x74
#define IIC_BUS_DDR3 0x10
#define IIC_SI5324_ADDRESS 0x68
#define IIC_570BA_ADDRESS 0x5d

typedef struct {
	bool tx_empty;
	bool rx_empty;
	bool tx_full;
	bool rx_full;
	bool srw;
	bool bb;
	bool aas;
	bool abgc;
} Status;

typedef struct {
	bool tx_half;
	bool not_addressed;
	bool addressed;
	bool not_busy;
	bool rx_full;
	bool tx_empty;
	bool error_compl;
	bool arb_lost;
} ISR;

ISR read_isr_register() {
	uint32_t ret = 0xFFAAFFAA;
	platform::platform_read_ctl(isr_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	ISR t;
	t.tx_half = getVal(ret, 7);
	t.not_addressed = getVal(ret, 6);
	t.addressed = getVal(ret, 5);
	t.not_busy = getVal(ret, 4);
	t.rx_full = getVal(ret, 3);
	t.tx_empty = getVal(ret, 2);
	t.error_compl = getVal(ret, 1);
	t.arb_lost = getVal(ret, 0);
	return t;
}

void reset_isr_register() {
	uint32_t ret = 0xFFAAFFAA;
	platform::platform_read_ctl(isr_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	ret = 0;
	platform::platform_write_ctl(isr_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

Status read_status_register() {
	uint32_t ret = 0xFFAAFFAA;
	platform::platform_read_ctl(status_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	Status t;
	t.tx_empty = getVal(ret, 7);
	t.rx_empty = getVal(ret, 6);
	t.tx_full = getVal(ret, 5);
	t.rx_full = getVal(ret, 4);
	t.srw = getVal(ret, 3);
	t.bb = getVal(ret, 2);
	t.aas = getVal(ret, 1);
	t.abgc = getVal(ret, 0);
	return t;
}

void enableDevice() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 0, 1);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void disableDevice() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 0, 0);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void startTransfer() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 2, 1);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void endTransfer() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 2, 0);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void enableGeneralCall() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 6, 1);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void disableGeneralCall() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 6, 0);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void resetTXFIFO() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 1, 1);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void disableTXFIFOReset() {
	uint32_t ret = 0;
	platform::platform_read_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	setVal(ret, 1, 0);
	platform::platform_write_ctl(control_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void setFIFOPIRQ(uint8_t v) {
	uint32_t ret = v;
	platform::platform_write_ctl(rx_fifo_pirq, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void enqueueWord(uint8_t v, bool start, bool stop) {
	uint32_t f = v;
	setVal(f, 8, start);
	setVal(f, 9, stop);
	std::cout << "Enqueue " << std::hex << f << std::dec << std::endl;
	platform::platform_write_ctl(fifo_tx_register, sizeof(f), &f, platform::PLATFORM_CTL_FLAGS_RAW);
}

uint8_t readWord() {
	uint32_t f = 0xFF;
	platform::platform_read_ctl(fifo_rx_register, sizeof(f), &f, platform::PLATFORM_CTL_FLAGS_RAW);
	return f;
}

void printStatus(Status t) {
	std::cout << "Status: " << std::endl;
	std::cout << "TX Empty: " << t.tx_empty << std::endl;
	std::cout << "RX Empty: " << t.rx_empty << std::endl;
	std::cout << "TX Full: " << t.tx_full << std::endl;
	std::cout << "RX Full: " << t.rx_full << std::endl;
	std::cout << "Slave Read/Write: " << t.srw << std::endl;
	std::cout << "Bus Busy: " << t.bb << std::endl;
	std::cout << "Addressed as Slave: " << t.aas << std::endl;
	std::cout << "Addressed by general call: " << t.abgc << std::endl;
}

void printISR(ISR t) {
	std::cout << "ISR: " << std::endl;
	std::cout << "TX Half: " << 		t.tx_half << std::endl;
	std::cout << "Not addressed: " << 	t.not_addressed << std::endl;
	std::cout << "Addressed: " << 		t.addressed << std::endl;
	std::cout << "Not Busy: " << 		t.not_busy << std::endl;
	std::cout << "RX Full: " << 		t.rx_full << std::endl;
	std::cout << "TX Empty: " << 		t.tx_empty << std::endl;
	std::cout << "Error/Complete: " << 	t.error_compl << std::endl;
	std::cout << "Arb Lost: " << 		t.arb_lost << std::endl;
}

void resetDevice() {
	platform::platform_write_ctl(reset_register, sizeof(reset_key), &reset_key, platform::PLATFORM_CTL_FLAGS_RAW);
}

bool writeRegister(uint8_t addr, uint8_t *data, uint16_t words) {
	resetDevice();
	// Init
	setFIFOPIRQ(0xF);
	resetTXFIFO();
	reset_isr_register();
	ISR i = read_isr_register();
	enableDevice();
	disableTXFIFOReset();
	disableGeneralCall();

	std::cout << "Init done" << std::endl;

	// Wait till FIFO is empty
	Status t = read_status_register();
	while(!(t.tx_empty && t.rx_empty && !t.bb)) {
		usleep(1000);
		t = read_status_register();
	}
	std::cout << "FIFOs empty, ready to go" << std::endl;

	enqueueWord(addr << 1 | 0, true, false);
	for (int i = 0; i < words; ++i) {
		enqueueWord(data[i], false, i == (words - 1));
	}

	t = read_status_register();
	while (!t.tx_empty && t.bb) {
		usleep(1000);
		t = read_status_register();
	}
	i = read_isr_register();
	disableDevice();
	if(i.error_compl) {
		std::cout << "Write unsuccessful." << std::endl;
		return true;
	} else {
		std::cout << "Wrote request" << std::endl;
		return false;
	}
}

void readRegister(uint8_t addr, uint8_t *data, uint16_t words) {
	resetDevice();
	// Init
	setFIFOPIRQ(0xF);
	resetTXFIFO();
	enableDevice();
	disableTXFIFOReset();
	disableGeneralCall();
	reset_isr_register();

	std::cout << "Init done" << std::endl;

	// Wait till FIFO is empty
	Status t = read_status_register();
	while (!(t.tx_empty && t.rx_empty && !t.bb)) {
		usleep(1000);
		t = read_status_register();
	}
	std::cout << "FIFOs empty, ready to go" << std::endl;

	enqueueWord(addr << 1 | 1, true, false);
	enqueueWord(words, false, true);

	std::cout << "Wrote request" << std::endl;

	for (int i = 0; i < words; ++i) {
		t = read_status_register();
		while (t.rx_empty) {
			usleep(1000);
			t = read_status_register();
		}
		data[i] = readWord();
	}

	ISR i = read_isr_register();
	printISR(i);

	disableDevice();
}

void readRegisterFull(uint8_t addr, uint8_t slaveReg, uint8_t *data, uint16_t words) {
	resetDevice();
	// Init
	setFIFOPIRQ(0xF);
	resetTXFIFO();
	enableDevice();
	disableTXFIFOReset();
	disableGeneralCall();

	std::cout << "Init done" << std::endl;

	// Wait till FIFO is empty
	Status t = read_status_register();
	while (!(t.tx_empty && t.rx_empty && !t.bb)) {
		usleep(1000);
		t = read_status_register();
	}
	std::cout << "FIFOs empty, ready to go" << std::endl;

	enqueueWord(addr << 1 | 0, true, false);
	enqueueWord(slaveReg, false, false);
	enqueueWord(addr << 1 | 1, true, false);
	enqueueWord(words, false, true);

	std::cout << "Wrote request" << std::endl;

	for (int i = 0; i < words; ++i) {
		t = read_status_register();
		while (t.rx_empty) {
			usleep(1000);
			t = read_status_register();
		}
		data[i] = readWord();
	}

	disableDevice();
}

uint8_t getSwitchPosition() {
	uint8_t ret;
	readRegister(SWITCH_ADDR, &ret, 1);
	return ret;
}

bool setSwitchPosition(uint8_t position) {
	return writeRegister(SWITCH_ADDR, &position, 1);
}

void resetSwitch() {
	uint32_t ret = 1;
	platform::platform_write_ctl(gpo_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	ret = 0;
	usleep(100000);
	platform::platform_write_ctl(gpo_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void resetClock() {
	uint32_t ret = 2;
	platform::platform_write_ctl(gpo_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
	ret = 0;
	usleep(1000000);
	platform::platform_write_ctl(gpo_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

void releaseResetAll() {
	uint32_t ret = 0x00;
	platform::platform_write_ctl(gpo_register, sizeof(ret), &ret, platform::PLATFORM_CTL_FLAGS_RAW);
}

int main(int argc, char **argv) {
	Tapasco tapasco;

	//resetDevice();
	resetSwitch();
	resetClock();

	if(setSwitchPosition(IIC_BUS_DDR3)) {
		std::cout << "Failed to write switch position register." << std::endl;
		return -1;
	}
	std::cout << (uint64_t) getSwitchPosition() << std::endl;

	uint8_t WriteBuffer[10];
	WriteBuffer[0] = 0;
	WriteBuffer[1] = 0x54;	// Reg 0: Free run, Clock always on, No Bypass (Normal Op)
	WriteBuffer[2] = 0xE4;	// Reg 1: CLKIN2 is second priority
	WriteBuffer[3] = 0x12;	// Reg 2: BWSEL set to 1
	WriteBuffer[4] = 0x15;	// Reg 3: CKIN1 selected,  No Digital Hold, Output clocks disabled during ICAL
	WriteBuffer[5] = 0x92;	// Reg 4: Automatic Revertive, HIST_DEL = 0x12
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 6);
	uint8_t ReadBuffer[10];
	readRegisterFull(IIC_SI5324_ADDRESS, 0, ReadBuffer, 5);
	for(int i = 0; i < 5; ++i) {
		if(WriteBuffer[i + 1] != ReadBuffer[i]) {
			std::cout << "Register " << i << " failed." << std::endl;
			return -1;
		}
	}

	WriteBuffer[0] = 10;
	WriteBuffer[1] = 0x08;	// Reg 10: CKOUT2 disabled, CKOUT1 enabled
	WriteBuffer[2] = 0x40;	// Reg 11: CKIN1, CKIN2 enabled
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 3);

	WriteBuffer[0] = 25;
	WriteBuffer[1] = 0xA0;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 2);

	WriteBuffer[0] = 31;
	WriteBuffer[1] = 0x00;
	WriteBuffer[2] = 0x00;
	WriteBuffer[3] = 0x03;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 4);

	WriteBuffer[0] = 40;
	WriteBuffer[1] = 0xC2;
	WriteBuffer[2] = 0x49;
	WriteBuffer[3] = 0xEF;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 4);

	WriteBuffer[0] = 43;
	WriteBuffer[1] = 0x00;
	WriteBuffer[2] = 0x77;
	WriteBuffer[3] = 0x0B;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 4);

	WriteBuffer[0] = 46;
	WriteBuffer[1] = 0x00;
	WriteBuffer[2] = 0x77;
	WriteBuffer[3] = 0x0B;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 4);

	WriteBuffer[0] = 136;
	WriteBuffer[1] = 0x40;
	writeRegister(IIC_SI5324_ADDRESS, WriteBuffer, 2);

	ReadBuffer[0] = -1;
	readRegisterFull(IIC_SI5324_ADDRESS, 136, ReadBuffer, 1);
	while(ReadBuffer[0] != 0) {
		usleep(10000);
		readRegister(IIC_SI5324_ADDRESS, ReadBuffer, 1);
	}
	return 0;
}
