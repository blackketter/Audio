/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "effect_delay_ext.h"

//#define INTERNAL_TEST

// Use these with the audio adaptor board  (should be adjustable by the user...)
#define SPIRAM_MOSI_PIN  7
#define SPIRAM_MISO_PIN  12
#define SPIRAM_SCK_PIN   14

#define SPIRAM_CS_PIN    6


void AudioEffectDelayExternal::update(void)
{
	audio_block_t *block;
	uint32_t n, channel, read_offset;

	// grab incoming data and put it into the memory
	block = receiveReadOnly();
	if (memory_type >= AUDIO_MEMORY_UNDEFINED) {
		// ignore input and do nothing if undefined memory type
		release(block);
		return;
	}
	if (block) {
		if (head_offset + AUDIO_BLOCK_SAMPLES <= memory_length) {
			// a single write is enough
			write(head_offset, AUDIO_BLOCK_SAMPLES, block->data);
			head_offset += AUDIO_BLOCK_SAMPLES;
		} else {
			// write wraps across end-of-memory
			n = memory_length - head_offset;
			write(head_offset, n, block->data);
			head_offset = AUDIO_BLOCK_SAMPLES - n;
			write(0, head_offset, block->data + n);
		}
		release(block);
	} else {
		// if no input, store zeros, so later playback will
		// not be random garbage previously stored in memory
		if (head_offset + AUDIO_BLOCK_SAMPLES <= memory_length) {
			zero(head_offset, AUDIO_BLOCK_SAMPLES);
			head_offset += AUDIO_BLOCK_SAMPLES;
		} else {
			n = memory_length - head_offset;
			zero(head_offset, n);
			head_offset = AUDIO_BLOCK_SAMPLES - n;
			zero(0, head_offset);
		}
	}

	// transmit the delayed outputs
	for (channel = 0; channel < 8; channel++) {
		if (!(activemask & (1<<channel))) continue;
		block = allocate();
		if (!block) continue;
		// compute the delayed location where we read
		if (delay_length[channel] <= head_offset) {
			read_offset = head_offset - delay_length[channel];
		} else {
			read_offset = memory_length + head_offset - delay_length[channel];
		}
		if (read_offset + AUDIO_BLOCK_SAMPLES <= memory_length) {
			// a single read will do it
			read(read_offset, AUDIO_BLOCK_SAMPLES, block->data);
		} else {
			// read wraps across end-of-memory
			n = memory_length - read_offset;
			read(read_offset, n, block->data);
			read(0, AUDIO_BLOCK_SAMPLES - n, block->data + n);
		}
		transmit(block, channel);
		release(block);
	}
}

uint32_t AudioEffectDelayExternal::allocated[2] = {0, 0};

void AudioEffectDelayExternal::initialize(AudioEffectDelayMemoryType_t type, uint32_t samples)
{
	uint32_t memsize, avail;

	activemask = 0;
	head_offset = 0;
	memory_type = type;
	if (type == AUDIO_MEMORY_23LC1024) {
#ifdef INTERNAL_TEST
		memsize = 8000;
#else
		memsize = 65536;
#endif
		pinMode(SPIRAM_CS_PIN, OUTPUT);
		digitalWriteFast(SPIRAM_CS_PIN, HIGH);
	} else if (type == AUDIO_MEMORY_MEMORYBOARD) {
		memsize = 393216;
	} else {
		return;
	}
	avail = memsize - allocated[type];
	if (avail < AUDIO_BLOCK_SAMPLES*2+1) {
		memory_type = AUDIO_MEMORY_UNDEFINED;
		return;
	}
	if (samples > avail) samples = avail;
	memory_begin = allocated[type];
	allocated[type] += samples;
	memory_length = samples;

	SPI.setMOSI(SPIRAM_MOSI_PIN);
	SPI.setMISO(SPIRAM_MISO_PIN);
	SPI.setSCK(SPIRAM_SCK_PIN);

	SPI.begin();
	zero(0, memory_length);
}


#ifdef INTERNAL_TEST
static int16_t testmem[8000]; // testing only
#endif


#define SPISETTING SPISettings(20000000, MSBFIRST, SPI_MODE0)

void AudioEffectDelayExternal::read(uint32_t offset, uint32_t count, int16_t *data)
{
	uint32_t addr = memory_begin + offset;

#ifdef INTERNAL_TEST
	while (count) { *data++ = testmem[addr++]; count--; } // testing only
#else
	if (memory_type == AUDIO_MEMORY_23LC1024) {
		addr *= 2;
		SPI.beginTransaction(SPISETTING);
		digitalWriteFast(SPIRAM_CS_PIN, LOW);
		SPI.transfer16((0x03 << 8) | (addr >> 16));
		SPI.transfer16(addr & 0xFFFF);
		while (count) {
			*data++ = (int16_t)(SPI.transfer16(0));
			count--;
		}
		digitalWriteFast(SPIRAM_CS_PIN, HIGH);
		SPI.endTransaction();
	} else if (memory_type == AUDIO_MEMORY_MEMORYBOARD) {
		// TODO.... similar, but handle partition across 6 chips
	}
#endif
}

void AudioEffectDelayExternal::write(uint32_t offset, uint32_t count, const int16_t *data)
{
	uint32_t addr = memory_begin + offset;

#ifdef INTERNAL_TEST
	while (count) { testmem[addr++] = *data++; count--; } // testing only
#else
	if (memory_type == AUDIO_MEMORY_23LC1024) {
		addr *= 2;
		SPI.beginTransaction(SPISETTING);
		digitalWriteFast(SPIRAM_CS_PIN, LOW);
		SPI.transfer16((0x02 << 8) | (addr >> 16));
		SPI.transfer16(addr & 0xFFFF);
		while (count) {
			SPI.transfer16(*data++);
			count--;
		}
		digitalWriteFast(SPIRAM_CS_PIN, HIGH);
		SPI.endTransaction();
	} else if (memory_type == AUDIO_MEMORY_MEMORYBOARD) {
		// TODO.... similar, but handle partition across 6 chips
	}
#endif
}

void AudioEffectDelayExternal::zero(uint32_t offset, uint32_t count)
{
	uint32_t addr = memory_begin + offset;

#ifdef INTERNAL_TEST
	while (count) { testmem[addr++] = 0; count--; } // testing only
#else
	if (memory_type == AUDIO_MEMORY_23LC1024) {
		addr *= 2;
		SPI.beginTransaction(SPISETTING);
		//digitalWriteFast(SPIRAM_CS_PIN, LOW);
		//SPI.transfer16((0x01 << 8) | 0x40);  // not needed, defaults to this mode
		//digitalWriteFast(SPIRAM_CS_PIN, HIGH);
		//delayMicroseconds(1);
		//digitalWriteFast(SPIRAM_CS_PIN, LOW);
		//SPI.transfer16((0x05 << 8) | 0x40);  // not needed, defaults to this mode
		//digitalWriteFast(SPIRAM_CS_PIN, HIGH);
		//delayMicroseconds(1);
		digitalWriteFast(SPIRAM_CS_PIN, LOW);
		SPI.transfer16((0x02 << 8) | (addr >> 16));
		SPI.transfer16(addr & 0xFFFF);
		while (count) {
			SPI.transfer16(0);
			count--;
		}
		digitalWriteFast(SPIRAM_CS_PIN, HIGH);
		SPI.endTransaction();
	} else if (memory_type == AUDIO_MEMORY_MEMORYBOARD) {
		// TODO.... similar, but handle partition across 6 chips
	}
#endif
}







