/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


/*
 Pure C port by Oerg866

 Extra defines:
	PRECALC_TBL			Does *not* generate table. Ideal for static environments with one chip and fixed sample rates.
						If this is defined, you need a "PRECALC.INC" file for your sample rate.
	DUMP_TABLES			Creates a 'precalc.inc' file with table definitions when initializing the chip
 */
#ifndef DBOPL_H
#define DBOPL_H

 //Use 8 handlers based on a small logatirmic wavetabe and an exponential table for volume
#define WAVE_HANDLER	10
//Use a logarithmic wavetable with an exponential table for volume
#define WAVE_TABLELOG	11
//Use a linear wavetable with a multiply table for volume
#define WAVE_TABLEMUL	12

//Select the type of wave generator routine
#define DBOPL_WAVE WAVE_HANDLER

#ifdef DOS16
#include "16bitint.h"
#else
#include <inttypes.h>
#include <stdbool.h>
#endif
#ifndef DB_FASTCALL
#define DB_FASTCALL
#endif

typedef uintptr_t Bitu;
typedef intptr_t Bits;

struct _Chip;
struct _Operator;
struct _Channel;

#if (DBOPL_WAVE == WAVE_HANDLER)
typedef Bits ( DB_FASTCALL *WaveHandler) ( Bitu i, Bitu volume );
#endif

typedef Bits ( *Operator_VolumeHandler) ( struct _Operator* op );
typedef struct _Channel* ( *Channel_SynthHandler) ( struct _Channel* ch, struct _Chip* chip, uint32_t samples, int16_t* output );

//Different synth modes that can generate blocks of data
typedef enum {
	sm2AM = 0,
	sm2FM,
	sm3AM,
	sm3FM,
	sm4Start,
	sm3FMFM,
	sm3AMFM,
	sm3FMAM,
	sm3AMAM,
	sm6Start,
	sm2Percussion,
	sm3Percussion,
} SynthMode;

//Shifts for the values contained in chandata variable
enum {
	SHIFT_KSLBASE = 16,
	SHIFT_KEYCODE = 24,
};

typedef enum {
	MASK_KSR = 0x10,
	MASK_SUSTAIN = 0x20,
	MASK_VIBRATO = 0x40,
	MASK_TREMOLO = 0x80,
} Operator_StateMask;

typedef enum {
	OFF = 0,
	RELEASE,
	SUSTAIN,
	DECAY,
	ATTACK,
} Operator_State;

#pragma pack(1)

typedef struct _Operator {
	//Masks for operator 20 values

	Operator_VolumeHandler volHandler;

#if (DBOPL_WAVE == WAVE_HANDLER)
	WaveHandler waveHandler;	//Routine that generate a wave 
#else
	int16_t* waveBase;
	uint32_t waveMask;
	uint32_t waveStart;
#endif
	uint32_t waveIndex;			//WAVE_BITS shifted counter of the frequency index
	uint32_t waveAdd;				//The base frequency without vibrato
	uint32_t waveCurrent;			//waveAdd + vibratao

	uint32_t chanData;			//Frequency/octave and derived data coming from whatever channel controls this
	uint32_t freqMul;				//Scale channel frequency with this, TODO maybe remove?
	uint32_t vibrato;				//Scaled up vibrato strength
	int32_t sustainLevel;		//When stopping at sustain level stop here
	int32_t totalLevel;			//totalLevel is added to every generated volume
	uint32_t currentLevel;		//totalLevel + tremolo
	int32_t volume;				//The currently active volume
	
	uint32_t attackAdd;			//Timers for the different states of the envelope
	uint32_t decayAdd;
	uint32_t releaseAdd;
	uint32_t rateIndex;			//Current position of the evenlope

	uint8_t rateZero;				//Bits for the different states of the envelope having no changes
	uint8_t keyOn;				//Bitmask of different values that can generate keyon
	//Registers, also used to check for changes
	uint8_t reg20, reg40, reg60, reg80, regE0;
	//Active part of the envelope we're in
	uint8_t state;
	//0xff when tremolo is enabled
	uint8_t tremoloMask;
	//Strength of the vibrato
	uint8_t vibStrength;
	//Keep track of the calculated KSR so we can check for changes
	uint8_t ksr;
} Operator;

typedef struct _Channel {
	Operator op[2]; //Leave on top of struct for simpler pointer math.

	Channel_SynthHandler synthHandler;
	uint32_t chanData;		//Frequency/octave and derived values
	int32_t old[2];			//Old data for feedback

	uint8_t feedback;			//Feedback shift
	uint8_t regB0;			//Register values to check for changes
	uint8_t regC0;
	//This should correspond with reg104, bit 6 indicates a Percussion channel, bit 7 indicates a silent channel
	uint8_t fourMask;
	int16_t maskLeft;		//Sign extended values for both channel's panning
	int16_t maskRight;

} Channel;

typedef struct _Chip {
	//18 channels with 2 operators each. Leave on top of struct for simpler pointer math.
	Channel chan[18];

	//This is used as the base counter for vibrato and tremolo
	uint32_t lfoCounter;

	uint32_t noiseCounter;
	uint32_t noiseValue;

#ifndef PRECALC_TBL
	uint32_t lfoAdd;
	uint32_t noiseAdd;
	//Frequency scales for the different multiplications
    uint32_t freqMul[16];// = {};
	//Rates for decay and release for rate of this chip
    uint32_t linearRates[76];// = {};
	//Best match attack rates for the rate of this chip
    uint32_t attackRates[76];// = {};
#endif

	uint8_t reg104;
	uint8_t reg08;
	uint8_t reg04;
	uint8_t regBD;
	uint8_t vibratoIndex;
	uint8_t tremoloIndex;
	int8_t vibratoSign;
	uint8_t vibratoShift;
	uint8_t tremoloValue;
	uint8_t vibratoStrength;
	uint8_t tremoloStrength;
	//Mask for allowed wave forms
	uint8_t waveFormMask;
	//0 or -1 when enabled
	int8_t opl3Active;
	//Running in opl3 mode
	bool opl3Mode;

} Chip;

#pragma pack()

/*
void Operator_SetState		( Operator* op, uint8_t s );
void Operator_UpdateAttack	( Operator* op, const Chip* chip );
void Operator_UpdateRelease	( Operator* op, const Chip* chip );
void Operator_UpdateDecay	( Operator* op, const Chip* chip );

void Operator_UpdateAttenuation	( Operator* op );
void Operator_UpdateRates		( Operator* op, const Chip* chip );
void Operator_UpdateFrequency	( Operator* op );

void Operator_Write20	( Operator* op, const Chip* chip, uint8_t val );
void Operator_Write40	( Operator* op, const Chip* chip, uint8_t val );
void Operator_Write60	( Operator* op, const Chip* chip, uint8_t val );
void Operator_Write80	( Operator* op, const Chip* chip, uint8_t val );
void Operator_WriteE0	( Operator* op, const Chip* chip, uint8_t val );

bool Operator_Silent	( Operator* op );
void Operator_Prepare	( Operator* op, const Chip* chip );

void Operator_KeyOn		( Operator* op, uint8_t mask );
void Operator_KeyOff	( Operator* op, uint8_t mask );

int32_t Operator_RateForward( Operator* op, uint32_t add );
Bitu Operator_ForwardWave	( Operator* op );
Bitu Operator_ForwardVolume	( Operator* op );

Bits Operator_GetSample	( Operator* op, Bits modulation );
Bits Operator_GetWave	( Operator* op, Bitu index, Bitu vol );

//Forward the channel data to the operators of the channel
void Channel_SetChanData( Channel* ch, const Chip* chip, uint32_t data );
//Change in the chandata, check for new values and if we have to forward to operators
void Channel_UpdateFrequency( Channel* ch, const Chip* chip, uint8_t fourOp );
void Channel_UpdateSynth( Channel* ch, const Chip* chip);
void Channel_WriteA0( Channel* ch, const Chip* chip, uint8_t val );
void Channel_WriteB0( Channel* ch, const Chip* chip, uint8_t val );
void Channel_WriteC0( Channel* ch, const Chip* chip, uint8_t val );

//Return the maximum amount of samples before and LFO change
uint32_t Chip_ForwardLFO( Chip* chip, uint32_t samples );
uint32_t Chip_ForwardNoise( Chip* chip );
void Chip_WriteBD( Chip* chip, uint8_t val );
void Chip_WriteReg( Chip* chip, uint16_t reg, uint8_t val );
uint32_t Chip_WriteAddr( Chip* chip, uint32_t port, uint8_t val );
int Chip_GenerateBlock2( Chip* chip, Bitu total, int16_t* output );
int Chip_GenerateBlock3( Chip* chip, Bitu total, int16_t* output );
//Update the synth handlers in all channels
void Chip_UpdateSynths( Chip* chip );
*/
void Chip_WriteReg( Chip* chip, uint16_t reg, uint8_t val );
int Chip_Generate( Chip* chip, int16_t* buffer, uint32_t samples );
void Chip_Setup( Chip *chip, uint32_t rate );
void Chip_Reset( Chip* chip, bool opl3Mode, uint32_t rate );

#endif
