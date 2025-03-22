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
	DOSBox implementation of a combined Yamaha YMF262 and Yamaha YM3812 emulator.
	Enabling the opl3 bit will switch the emulator to stereo opl3 output instead of regular mono opl2
	Except for the table generation it's all integer math
	Can choose different types of generators, using muls and bigger tables, try different ones for slower platforms
	The generation was based on the MAME implementation but tried to have it use less memory and be faster in general
	MAME uses much bigger envelope tables and this will be the biggest cause of it sounding different at times

	//TODO Don't delay first operator 1 sample in opl3 mode
	//TODO Maybe not use class method pointers but a regular function pointers with operator as first parameter
	//TODO Fix panning for the Percussion channels, would any opl3 player use it and actually really change it though?
	//TODO Check if having the same accuracy in all frequency multipliers sounds better or not

	//DUNNO Keyon in 4op, switch to 2op without keyoff.
*/



#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "dbopl.h"


#ifndef PI
#define PI 3.14159265358979323846
#endif
#define GCC_UNLIKELY(x) (x)


#define OPLRATE		((double)(14318180.0 / 288.0))
#define TREMOLO_TABLE 52

//Try to use most precision for frequencies
//Else try to keep different waves in synch
//#define WAVE_PRECISION	1
#ifndef WAVE_PRECISION
//Wave bits available in the top of the 32bit range
//Original adlib uses 10.10, we use 10.22
#define WAVE_BITS	10
#else
//Need some extra bits at the top to have room for octaves and frequency multiplier
//We support to 8 times lower rate
//128 * 15 * 8 = 15350, 2^13.9, so need 14 bits
#define WAVE_BITS	14
#endif
#define WAVE_SH		( 32 - WAVE_BITS )
#define WAVE_MASK	( ( 1 << WAVE_SH ) - 1 )

//Use the same accuracy as the waves
#define LFO_SH ( WAVE_SH - 10 )
//LFO is controlled by our tremolo 256 sample limit
#define LFO_MAX ( 256UL << ( LFO_SH ) )


//Maximum amount of attenuation bits
//Envelope goes to 511, 9 bits
#if (DBOPL_WAVE == WAVE_TABLEMUL )
//Uses the value directly
#define ENV_BITS	( 9 )
#else
//Add 3 bits here for more accuracy and would have to be shifted up either way
#define ENV_BITS	( 9 )
#endif
//Limits of the envelope with those bits and when the envelope goes silent
#define ENV_MIN		0
#define ENV_EXTRA	( ENV_BITS - 9 )
#define ENV_MAX		( 511L << ENV_EXTRA )
#define ENV_LIMIT	( ( 12UL * 256UL) >> ( 3 - ENV_EXTRA ) )
#define ENV_SILENT( _X_ ) ( (_X_) >= ENV_LIMIT )

//Attack/decay/release rate counter shift
#define RATE_SH		24
#define RATE_MASK	( ( 1UL << RATE_SH ) - 1UL )
//Has to fit within 16bit lookuptable
#define MUL_SH		16

//Check some ranges
#if ENV_EXTRA > 3
#error Too many envelope bits
#endif

#ifdef PRECALC_TBL

#include "precalc.inc"
#define FreqMul 		PrecalcFreqMul
#define LinearRates 	PrecalcLinearRates
#define AttackRates 	PrecalcAttackRates
#define LfoAdd			PrecalcLfoAdd
#define NoiseAdd		PrecalcNoiseAdd

#else

#include <math.h>

#define FreqMul 		chip->freqMul
#define LinearRates 	chip->linearRates
#define AttackRates 	chip->attackRates
#define LfoAdd			chip->lfoAdd
#define NoiseAdd		chip->noiseAdd

#endif

#ifndef PRECALC_TBL
//How much to subtract from the base value for the final attenuation
static const uint8_t KslCreateTable[16] = {
	//0 will always be be lower than 7 * 8
	64, 32, 24, 19, 
	16, 12, 11, 10, 
	 8,  6,  5,  4,
	 3,  2,  1,  0,
};

#define M(_X_) ((uint8_t)( (_X_) * 2))
static const uint8_t FreqCreateTable[16] = {
	M(0.5), M(1 ), M(2 ), M(3 ), M(4 ), M(5 ), M(6 ), M(7 ),
	M(8  ), M(9 ), M(10), M(10), M(12), M(12), M(15), M(15)
};
#undef M

//We're not including the highest attack rate, that gets a special value
static const uint8_t AttackSamplesTable[13] = {
	69, 55, 46, 40,
	35, 29, 23, 20,
	19, 15, 11, 10,
	9
};
//On a real opl these values take 8 samples to reach and are based upon larger tables
static const uint8_t EnvelopeIncreaseTable[13] = {
	4,  5,  6,  7,
	8, 10, 12, 14,
	16, 20, 24, 28,
	32, 
};

#if ( DBOPL_WAVE == WAVE_HANDLER ) || ( DBOPL_WAVE == WAVE_TABLELOG )
static uint16_t ExpTable[ 256 ];
#endif

#if ( DBOPL_WAVE == WAVE_HANDLER )
//PI table used by WAVEHANDLER
static uint16_t SinTable[ 512 ];
#endif

#if ( DBOPL_WAVE > WAVE_HANDLER )
//Layout of the waveform table in 512 entry intervals
//With overlapping waves we reduce the table to half it's size

//	|    |//\\|____|WAV7|//__|/\  |____|/\/\|
//	|\\//|    |    |WAV7|    |  \/|    |    |
//	|06  |0126|17  |7   |3   |4   |4 5 |5   |

//6 is just 0 shifted and masked

static int16_t WaveTable[ 8 * 512 ];
//Distance into WaveTable the wave starts
static const uint16_t WaveBaseTable[8] = {
	0x000, 0x200, 0x200, 0x800,
	0xa00, 0xc00, 0x100, 0x400,

};
//Mask the counter with this
static const uint16_t WaveMaskTable[8] = {
	1023, 1023, 511, 511,
	1023, 1023, 512, 1023,
};

//Where to start the counter on at keyon
static const uint16_t WaveStartTable[8] = {
	512, 0, 0, 0,
	0, 512, 512, 256,
};
#endif

#if ( DBOPL_WAVE == WAVE_TABLEMUL )
static uint16_t MulTable[ 384 ];
#endif

//Generate a table index and table shift value using input value from a selected rate
static inline void EnvelopeSelect( uint8_t val, uint8_t *index, uint8_t *shift ) {
	if ( val < 13 * 4 ) {				//Rate 0 - 12
		*shift = 12 - ( val >> 2 );
		*index = val & 3;
	} else if ( val < 15 * 4 ) {		//rate 13 - 14
		*shift = 0;
		*index = val - 12 * 4;
	} else {							//rate 15 and up
		*shift = 0;
		*index = 12;
	}
}

static uint8_t KslTable[128];
static uint8_t TremoloTable[TREMOLO_TABLE];
#endif //PRECALC_TBL

//Start of a channel behind the chip struct start
static uint16_t ChanOffsetTable[32];
//Start of an operator behind the chip struct start
static uint16_t OpOffsetTable[64];

//Shift strength for the ksl value determined by ksl strength
static const uint8_t KslShiftTable[4] = {
	31,1,2,0
};

//The lower bits are the shift of the operator vibrato value
//The highest bit is right shifted to generate -1 or 0 for negation
//So taking the highest input value of 7 this gives 3, 7, 3, 0, -3, -7, -3, 0
static const int8_t VibratoTable[ 8 ] = {	
	1 - 0x00, 0 - 0x00, 1 - 0x00, 30 - 0x00, 
	1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80 
};


#if ( DBOPL_WAVE == WAVE_HANDLER )
/*
	Generate the different waveforms out of the sine/exponential table using handlers
*/
static inline Bits MakeVolume( Bitu wave, Bitu volume ) {
	Bitu total = wave + volume;
	Bitu index = total & 0xff;
	Bitu sig = ExpTable[ index ];
	Bitu exp = total >> 8;
#if 0
	//Check if we overflow the 31 shift limit
	if ( exp >= 32 ) {
		LOG_MSG( "WTF %d %d", total, exp );
	}
#endif
	return (sig >> exp);
}

static Bits DB_FASTCALL WaveForm0( Bitu i, Bitu volume ) {
	Bits neg = 0 - (( i >> 9) & 1);//Create ~0 or 0
	Bitu wave = SinTable[i & 511];
	return (MakeVolume( wave, volume ) ^ neg) - neg;
}
static Bits DB_FASTCALL WaveForm1( Bitu i, Bitu volume ) {
	uint32_t wave = SinTable[i & 511];
	wave |= ( ( ( i ^ 512UL ) & 512UL) - 1UL) >> ( 32 - 12 );
	return MakeVolume( wave, volume );
}
static Bits DB_FASTCALL WaveForm2( Bitu i, Bitu volume ) {
	Bitu wave = SinTable[i & 511];
	return MakeVolume( wave, volume );
}
static Bits DB_FASTCALL WaveForm3( Bitu i, Bitu volume ) {
	Bitu wave = SinTable[i & 255];
	wave |= ( ( ( i ^ 256UL) & 256UL) - 1UL) >> ( 32 - 12 );
	return MakeVolume( wave, volume );
}
static Bits DB_FASTCALL WaveForm4( Bitu i, Bitu volume ) {
	Bits neg;
	Bitu wave;
	//Twice as fast
	i <<= 1;
	neg = 0UL - (( i >> 9 ) & 1);//Create ~0 or 0
	wave = SinTable[i & 511];
	wave |= ( ( ( i ^ 512UL ) & 512UL) - 1UL) >> ( 32 - 12 );
	return (MakeVolume( wave, volume ) ^ neg) - neg;
}
static Bits DB_FASTCALL WaveForm5( Bitu i, Bitu volume ) {
	Bitu wave;
	//Twice as fast
	i <<= 1;
	wave = SinTable[i & 511];
	wave |= ( ( ( i ^ 512UL ) & 512UL) - 1UL) >> ( 32 - 12 );
	return MakeVolume( wave, volume );
}
static Bits DB_FASTCALL WaveForm6( Bitu i, Bitu volume ) {
	Bits neg = 0 - (( i >> 9) & 1);//Create ~0 or 0
	return (MakeVolume( 0, volume ) ^ neg) - neg;
}
static Bits DB_FASTCALL WaveForm7( Bitu i, Bitu volume ) {
	//Negative is reversed here
	Bits neg = (( i >> 9) & 1L) - 1L;
	Bitu wave = (i << 3);
	//When negative the volume also runs backwards
	wave = ((wave ^ neg) - neg) & 4095UL;
	return (MakeVolume( wave, volume ) ^ neg) - neg;
}

static const WaveHandler WaveHandlerTable[8] = {
	WaveForm0, WaveForm1, WaveForm2, WaveForm3,
	WaveForm4, WaveForm5, WaveForm6, WaveForm7
};

#endif

/*
	Operator
*/

//We zero out when rate == 0
static inline void Operator_UpdateAttack( Operator* op, const Chip* chip ) {
	uint8_t rate = op->reg60 >> 4;
	if ( rate ) {
		uint8_t val = (rate << 2) + op->ksr;
		op->attackAdd = AttackRates[ val ];
		op->rateZero &= ~(1 << ATTACK);
	} else {
		op->attackAdd = 0;
		op->rateZero |= (1 << ATTACK);
	}
	//printf("UpdateAttack rate: %u, attackadd %lu, ratezero %u ", rate, (long unsigned) op->attackAdd, op->rateZero);
}
static inline void Operator_UpdateDecay( Operator* op, const Chip* chip ) {
	uint8_t rate = op->reg60 & 0xf;
	if ( rate ) {
		uint8_t val = (rate << 2) + op->ksr;
		op->decayAdd = LinearRates[ val ];
		op->rateZero &= ~(1 << DECAY);
	} else {
		op->decayAdd = 0;
		op->rateZero |= (1 << DECAY);
	}
}
static inline void Operator_UpdateRelease( Operator* op, const Chip* chip ) {
	uint8_t rate = op->reg80 & 0xf;
	if ( rate ) {
		uint8_t val = (rate << 2) + op->ksr;
		op->releaseAdd = LinearRates[ val ];
		op->rateZero &= ~(1 << RELEASE);
		if ( !(op->reg20 & MASK_SUSTAIN ) ) {
			op->rateZero &= ~( 1 << SUSTAIN );
		}	
	} else {
		op->rateZero |= (1 << RELEASE);
		op->releaseAdd = 0;
		if ( !(op->reg20 & MASK_SUSTAIN ) ) {
			op->rateZero |= ( 1 << SUSTAIN );
		}	
	}
}

static inline void Operator_UpdateAttenuation( Operator* op ) {
	uint8_t kslBase = (uint8_t)((op->chanData >> SHIFT_KSLBASE) & 0xff);
	uint32_t tl = op->reg40 & 0x3f;
	uint8_t kslShift = KslShiftTable[ op->reg40 >> 6 ];
	//Make sure the attenuation goes to the right bits
	op->totalLevel = (int32_t)(tl << ( ENV_BITS - 7 ));	//Total level goes 2 bits below max
	op->totalLevel += ( ((uint32_t)kslBase) << ENV_EXTRA ) >> kslShift;
}

static inline void Operator_UpdateFrequency( Operator* op ) {
	uint32_t freq = op->chanData & (( 1 << 10 ) - 1);
	uint32_t block = (op->chanData >> 10) & 0xff;
#ifdef WAVE_PRECISION
	block = 7UL - block;
	op->waveAdd = ( freq * op->freqMul ) >> block;
#else
	op->waveAdd = ( freq << block ) * op->freqMul;
#endif
	if ( op->reg20 & MASK_VIBRATO ) {
		op->vibStrength = (uint8_t)(freq >> 7u);

#ifdef WAVE_PRECISION
		op->vibrato = ( (uint32_t)op->vibStrength * op->freqMul ) >> block;
#else
		op->vibrato = ( (uint32_t)op->vibStrength << block ) * op->freqMul;
#endif
	} else {
		op->vibStrength = 0;
		op->vibrato = 0UL;
	}
}

static void Operator_UpdateRates( Operator* op, const Chip* chip ) {
	//Mame seems to reverse this where enabling ksr actually lowers
	//the rate, but pdf manuals says otherwise?
	uint8_t newKsr = (uint8_t)((op->chanData >> SHIFT_KEYCODE) & 0xff);
	if ( !( op->reg20 & MASK_KSR ) ) {
		newKsr >>= 2;
	}
	if ( op->ksr == newKsr )
		return;
	op->ksr = newKsr;
	Operator_UpdateAttack( op, chip );
	Operator_UpdateDecay( op, chip );
	Operator_UpdateRelease( op, chip );
}

static inline int32_t Operator_RateForward( Operator *op, uint32_t add ) {
	int32_t ret;
	op->rateIndex += add;
	ret = (int32_t)(op->rateIndex >> RATE_SH);
	op->rateIndex &= RATE_MASK;

	//printf("RateForward: add %lu, ret %ld\n", (long unsigned) add, (long)ret);
	return ret;
}

/* Messy - forward declaration of the table so we can use it within Operator_SetState which in turn is used by the functions after it.. bleh */
static const Operator_VolumeHandler VolumeHandlerTable[5];

static inline void Operator_SetState( Operator* op, uint8_t s ) {
	//printf("op %p set state %u\n", op, s);
	op->state = s;
	op->volHandler = VolumeHandlerTable[ s ];
}

static Bits Operator_Volume_OFF(Operator* op) {
	return ENV_MAX;
}

static Bits Operator_Volume_ATTACK(Operator* op) {
	int32_t vol = op->volume;
	int32_t change = Operator_RateForward( op, op->attackAdd );
	int32_t volCompl;

	if ( !change )
		return vol;

	/* BUG IN MICROSOFT C 8.x: COMPLEMENT CAN LOSE SIGN BIT CAUSING CORRUPTION HERE!! WTF!!!! */
	volCompl = -vol - 1L;
	vol += ( volCompl * change ) >> 3;

	if ( vol < ENV_MIN ) {
		op->volume = ENV_MIN;
		op->rateIndex = 0;
		Operator_SetState( op, DECAY );
		return ENV_MIN;
	}
	op->volume = vol;
	return vol;
}

static Bits Operator_Volume_SUSTAIN(Operator* op) {
	int32_t vol = op->volume;
	if ( op->reg20 & MASK_SUSTAIN ) {
		return vol;
	}
	//In sustain phase, but not sustaining, do regular release
	vol += Operator_RateForward( op, op->releaseAdd );
	if ( GCC_UNLIKELY(vol >= ENV_MAX) ) {
		op->volume = ENV_MAX;
		Operator_SetState( op,OFF );
		return ENV_MAX;
	}
	op->volume = vol;
	return vol;
}

static Bits Operator_Volume_RELEASE(Operator* op) {
	int32_t vol = op->volume;
	vol += Operator_RateForward( op, op->releaseAdd );
	if ( GCC_UNLIKELY(vol >= ENV_MAX) ) {
		op->volume = ENV_MAX;
		Operator_SetState( op, OFF );
		return ENV_MAX;
	}
	op->volume = vol;
	return vol;
}

static Bits Operator_Volume_DECAY(Operator* op) {
	int32_t vol = op->volume;

	vol += Operator_RateForward( op, op->decayAdd );
	//if ( GCC_UNLIKELY(vol >= sustainLevel) ) {
	if ( vol >= op->sustainLevel ) {
		//Check if we didn't overshoot max attenuation, then just go off
		//if ( GCC_UNLIKELY(vol >= ENV_MAX) ) {
		if ( vol >= ENV_MAX ) {
			op->volume = ENV_MAX;
			Operator_SetState( op, OFF );
			return ENV_MAX;
		}
		//Continue as sustain
		op->rateIndex = 0;
		Operator_SetState( op, SUSTAIN );
	}
	op->volume = vol;
	return vol;
}

static const Operator_VolumeHandler VolumeHandlerTable[5] = {
	Operator_Volume_OFF, // &Operator::TemplateVolume< Operator::OFF >,
	Operator_Volume_RELEASE, // &Operator::TemplateVolume< Operator::RELEASE >,
	Operator_Volume_SUSTAIN, // &Operator::TemplateVolume< Operator::SUSTAIN >,
	Operator_Volume_DECAY, // &Operator::TemplateVolume< Operator::DECAY >,
	Operator_Volume_ATTACK, // &Operator::TemplateVolume< Operator::ATTACK >
};

static inline Bitu Operator_ForwardVolume(Operator *op) {
	Bitu ret;
	ret = (Bitu)(op->currentLevel + op->volHandler(op));

	//	printf("current: %lu ret %lu\n", (long unsigned) op->currentLevel, (long unsigned) ret);

	return ret;
}


static inline Bitu Operator_ForwardWave(Operator *op) {
	op->waveIndex += op->waveCurrent;	
	return op->waveIndex >> WAVE_SH;
}


static inline bool Operator_Silent(Operator *op) {
	if ( !ENV_SILENT( op->totalLevel + op->volume ) )
		return false;
	if ( !(op->rateZero & ( 1 << op->state ) ) )
		return false;
	return true;
}

static inline void Operator_Prepare( Operator* op, const Chip* chip )  {
	op->currentLevel = (uint32_t)(op->totalLevel + (int32_t)(chip->tremoloValue & op->tremoloMask));
	op->waveCurrent = op->waveAdd;
	if ( op->vibStrength >> chip->vibratoShift ) {
		int32_t add = (int32_t)(op->vibrato >> chip->vibratoShift);
		//Sign extend over the shift value
		int32_t neg = chip->vibratoSign;
		//Negate the add with -1 or 0
		add = ( add ^ neg ) - neg;
		op->waveCurrent += (Bitu)add;
	}
}

static inline void Operator_KeyOn( Operator *op, uint8_t mask ) {
	if ( !op->keyOn ) {
		//Restart the frequency generator
#if ( DBOPL_WAVE > WAVE_HANDLER )
		op->waveIndex = op->waveStart;
#else
		op->waveIndex = 0;
#endif
		op->rateIndex = 0;
		Operator_SetState( op, ATTACK );
	}
	op->keyOn |= mask;
}

static inline void Operator_KeyOff( Operator *op, uint8_t mask ) {
	op->keyOn &= ~mask;
	if ( !op->keyOn ) {
		if ( op->state != OFF ) {
			Operator_SetState( op, RELEASE );
		}
	}
}

static inline Bits Operator_GetWave( Operator* op, Bitu index, Bitu vol ) {
#if ( DBOPL_WAVE == WAVE_HANDLER )
	return op->waveHandler( index, vol << ( 3 - ENV_EXTRA ) );
#elif ( DBOPL_WAVE == WAVE_TABLEMUL )
	return ((Bits)op->waveBase[ index & op->waveMask ] * (Bits)MulTable[ vol >> ENV_EXTRA ]) >> MUL_SH;
#elif ( DBOPL_WAVE == WAVE_TABLELOG )
	int32_t wave = (int32_t) op->waveBase[ index & op->waveMask ];
	uint32_t total = ( wave & 0x7fff ) + vol << ( 3 - ENV_EXTRA );
	int32_t sig = (int32_t) ExpTable[ total & 0xff ];
	uint32_t exp = total >> 8;
	int32_t neg = wave >> 16;
	return ((sig ^ neg) - neg) >> exp;
#else
#error "No valid wave routine"
#endif
}

static inline Bits Operator_GetSample( Operator *op, Bits modulation ) {
	Bitu vol = Operator_ForwardVolume(op);
	if ( ENV_SILENT( vol ) ) {
		//Simply forward the wave
		op->waveIndex += op->waveCurrent;
//		printf("mod %8ld, vol: %8lu, SILENT\n", (long)modulation, (unsigned long) vol);
		return 0;
	} else {
		Bitu index = Operator_ForwardWave(op);
		Bits ret;
		index += (Bitu)modulation;
		ret = Operator_GetWave( op, index, vol );
//		printf("mod: %8ld index: %8lu vol: %8lu ret: %8ld\n", (long)modulation, (unsigned long)index, (unsigned long)vol, (long)ret);
		return ret;
	}
}

static void Operator_Write20( Operator* op, const Chip* chip, uint8_t val ) {
	uint8_t change = (op->reg20 ^ val );
	if ( !change ) 
		return;
	op->reg20 = val;
	//Shift the tremolo bit over the entire register, saved a branch, YES!
	op->tremoloMask = (int8_t)(val) >> 7;
	op->tremoloMask &= ~(( 1 << ENV_EXTRA ) -1);
	//Update specific features based on changes
	if ( change & MASK_KSR ) {
		Operator_UpdateRates( op, chip );
	}
	//With sustain enable the volume doesn't change
	if ( op->reg20 & MASK_SUSTAIN || ( !op->releaseAdd ) ) {
		op->rateZero |= ( 1 << SUSTAIN );
	} else {
		op->rateZero &= ~( 1 << SUSTAIN );
	}
	//Frequency multiplier or vibrato changed
	if ( change & (0xf | MASK_VIBRATO) ) {
		op->freqMul = FreqMul[ val & 0xf ];
		Operator_UpdateFrequency( op );
	}
}

static void Operator_Write40( Operator *op, const Chip* chip, uint8_t val ) {
	if (!(op->reg40 ^ val )) 
		return;
	op->reg40 = val;
	Operator_UpdateAttenuation( op );
}

static void Operator_Write60( Operator* op, const Chip* chip, uint8_t val ) {
	uint8_t change = op->reg60 ^ val;
	op->reg60 = val;
	if ( change & 0x0f ) {
		Operator_UpdateDecay( op, chip );
	}
	if ( change & 0xf0 ) {
		Operator_UpdateAttack( op, chip );
	}
}

static void Operator_Write80( Operator* op, const Chip* chip, uint8_t val ) {
	uint8_t change = (op->reg80 ^ val );
	uint8_t sustain;
	if ( !change ) 
		return;
	op->reg80 = val;
	sustain = val >> 4;
	//Turn 0xf into 0x1f
	sustain |= ( sustain + 1) & 0x10;
	op->sustainLevel = sustain << ( ENV_BITS - 5 );
	if ( change & 0x0f ) {
		Operator_UpdateRelease( op, chip );
	}
}

static void Operator_WriteE0( Operator* op, const Chip* chip, uint8_t val ) {
	uint8_t waveForm;

	if ( !(op->regE0 ^ val) ) 
		return;

	//in opl3 mode you can always selet 7 waveforms regardless of waveformselect
	waveForm = val & ( ( 0x3 & chip->waveFormMask ) | (0x7 & chip->opl3Active ) );
	op->regE0 = val;
#if ( DBOPL_WAVE == WAVE_HANDLER )
	op->waveHandler = WaveHandlerTable[ waveForm ];
#else
	op->waveBase = WaveTable + WaveBaseTable[ waveForm ];
	op->waveStart = ((uint32_t)WaveStartTable[ waveForm ]) << WAVE_SH;
	op->waveMask = WaveMaskTable[ waveForm ];
#endif
}

static void Operator_Reset(Operator *op) {
	op->chanData = 0;
	op->freqMul = 0;
	op->waveIndex = 0;
	op->waveAdd = 0;
	op->waveCurrent = 0;
	op->keyOn = 0;
	op->ksr = 0;
	op->reg20 = 0;
	op->reg40 = 0;
	op->reg60 = 0;
	op->reg80 = 0;
	op->regE0 = 0;
#if (DBOPL_WAVE != WAVE_HANDLER)
    op->waveBase = 0;
    op->waveMask = 0;
    op->waveStart = 0;
#endif
    op->vibrato = 0;
    op->attackAdd = 0;
    op->decayAdd = 0;
    op->rateIndex = 0;
    op->tremoloMask = 0;
    op->vibStrength = 0;
	Operator_SetState( op, OFF );
	op->rateZero = (1 << OFF);
	op->sustainLevel = ENV_MAX;
	op->currentLevel = ENV_MAX;
	op->totalLevel = ENV_MAX;
	op->volume = ENV_MAX;
	op->releaseAdd = 0;
}


#define CH_OP(c, x) (((c) + (x>>1))->op[x & 1])

static Channel* Channel_Block_sm2AM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	if (Operator_Silent(&CH_OP(ch, 0)) && Operator_Silent(&CH_OP(ch, 1))) {
		ch->old[0] = ch->old[1] = 0L;
		return ch + 1;
	}
	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = (int32_t)(out0 + Operator_GetSample( &CH_OP(ch, 1), 0 ));

		output[ 0 ] += (int16_t) sample;
		output[ 1 ] += (int16_t) sample;
		output += 2;
	}

	return ( ch + 1 );
}

static Channel* Channel_Block_sm2FM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;

	if ( Operator_Silent(&CH_OP(ch, 1)) ) {
		ch->old[0] = ch->old[1] = 0L;
		return (ch + 1);
	}
	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );

		out0 = ch->old[0];
		sample = (int32_t)Operator_GetSample( &CH_OP(ch, 1), out0 );

		output[ 0 ] += (int16_t) sample;
		output[ 1 ] += (int16_t) sample;
		output += 2;
	}

	return ( ch + 1 );
}

static Channel* Channel_Block_sm3AM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 0)) && Operator_Silent(&CH_OP(ch, 1)) ) {
		ch->old[0] = ch->old[1] = 0L;
		return ch + 1;
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = (int32_t)(out0 + Operator_GetSample( &CH_OP(ch, 1), 0 ));

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}

	return ( ch + 1 );
}

static Channel* Channel_Block_sm3FM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 1)) ) {
		ch->old[0] = ch->old[1] = 0L;
		return (ch + 1);
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = (int32_t)Operator_GetSample( &CH_OP(ch, 1), out0 );

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}

	return ( ch + 1 );
}

static Channel* Channel_Block_sm3FMFM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 3)) ) {
		ch->old[0] = ch->old[1] = 0L;
		return (ch + 2);
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		Bits next;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		
		out0 = ch->old[0];
		next = Operator_GetSample( &CH_OP(ch, 1), out0 );
		next = Operator_GetSample( &CH_OP(ch, 2), next );
		sample = (int32_t)Operator_GetSample( &CH_OP(ch, 3), next );

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}
	return( ch + 2 );
}

static Channel* Channel_Block_sm3AMFM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 0)) && Operator_Silent(&CH_OP(ch, 3)) ) {
		ch->old[0] = ch->old[1] = 0L;
		return (ch + 2);
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		Bits next;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = out0;
		next = Operator_GetSample( &CH_OP(ch, 1), 0 );
		next = Operator_GetSample( &CH_OP(ch, 2), next );
		sample += (int32_t)Operator_GetSample( &CH_OP(ch, 3), next );

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}
	return( ch + 2 );
}

static Channel* Channel_Block_sm3FMAM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 1)) && Operator_Silent(&CH_OP(ch, 3)) ) {
		ch->old[0] = ch->old[1] = 0;
		return (ch + 2);
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		Bits next;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = (int32_t)Operator_GetSample( &CH_OP(ch, 1), out0 );
		next = Operator_GetSample( &CH_OP(ch, 2), 0 );
		sample += (int32_t)Operator_GetSample( &CH_OP(ch, 3), next );

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}

	return( ch + 2 );
}

static Channel* Channel_Block_sm3AMAM( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	uint32_t i;
	
	if ( Operator_Silent(&CH_OP(ch, 0)) && Operator_Silent(&CH_OP(ch, 2)) && Operator_Silent(&CH_OP(ch, 3)) ) {
		ch->old[0] = ch->old[1] = 0;
		return (ch + 2);
	}

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	for ( i = 0; i < samples; i++ ) {
		//Do unsigned shift so we can shift out all bits but still stay in 10 bit range otherwise
		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		int32_t sample;
		int32_t out0;
		Bits next;
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );
		out0 = ch->old[0];
		sample = out0;
		next = Operator_GetSample( &CH_OP(ch, 1), 0 );
		sample += (int32_t)Operator_GetSample( &CH_OP(ch, 2), next );
		sample += (int32_t)Operator_GetSample( &CH_OP(ch, 3), 0 );

		output[ 0 ] += (int16_t) sample & ch->maskLeft;
		output[ 1 ] += (int16_t) sample & ch->maskRight;
		output += 2;
	}

	return( ch + 2 );
}

static void Channel_GeneratePercussion_OPL2( Channel *ch, Chip* chip, int16_t* output );
static void Channel_GeneratePercussion_OPL3( Channel *ch, Chip* chip, int16_t* output );

static Channel* Channel_Block_sm2Percussion( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	Bitu i;

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	Operator_Prepare( &CH_OP(ch, 4), chip );
	Operator_Prepare( &CH_OP(ch, 5), chip );

	for ( i = 0; i < samples; i++ ) {
		Channel_GeneratePercussion_OPL2( ch, chip, output );
		output += 2;
	}

	return( ch + 3 );
}

static Channel* Channel_Block_sm3Percussion( Channel* ch, Chip* chip, uint32_t samples, int16_t* output ) {
	Bitu i;

	//Init the operators with the the current vibrato and tremolo values
	Operator_Prepare( &CH_OP(ch, 0), chip );
	Operator_Prepare( &CH_OP(ch, 1), chip );

	Operator_Prepare( &CH_OP(ch, 2), chip );
	Operator_Prepare( &CH_OP(ch, 3), chip );

	Operator_Prepare( &CH_OP(ch, 4), chip );
	Operator_Prepare( &CH_OP(ch, 5), chip );

	for ( i = 0; i < samples; i++ ) {
		Channel_GeneratePercussion_OPL3( ch, chip, output );
		output += 2;
	}

	return( ch + 3 );
}

/*
	Channel
*/

static void Channel_Reset(Channel *ch) {
	Operator_Reset(&ch->op[0]);
	Operator_Reset(&ch->op[1]);

	ch->old[0] = ch->old[1] = 0;
	ch->chanData = 0;
	ch->regB0 = 0;
	ch->regC0 = 0;
	ch->maskLeft = -1;
	ch->maskRight = -1;
	ch->feedback = 31;
	ch->fourMask = 0;
	ch->synthHandler = Channel_Block_sm2FM;
}


void Channel_SetChanData( Channel* ch, const Chip* chip, uint32_t data ) {
	uint32_t change = ch->chanData ^ data;
	ch->chanData = data;
	(&CH_OP(ch, 0))->chanData = data;
	(&CH_OP(ch, 1))->chanData = data;
	//Since a frequency update triggered this, always update frequency
	Operator_UpdateFrequency( &CH_OP(ch, 0) );
	Operator_UpdateFrequency( &CH_OP(ch, 1) );

	if ( change & ( 0xffUL << SHIFT_KSLBASE ) ) {
		Operator_UpdateAttenuation( &CH_OP(ch, 0) );
		Operator_UpdateAttenuation( &CH_OP(ch, 1) );
	}
	if ( change & ( 0xffUL << SHIFT_KEYCODE ) ) {
		Operator_UpdateRates( &CH_OP(ch, 0), chip );
		Operator_UpdateRates( &CH_OP(ch, 1), chip );
	}
}

void Channel_UpdateFrequency( Channel* ch, const Chip* chip, uint8_t fourOp ) {
	//Extrace the frequency bits
	uint32_t data = ch->chanData & 0xffff;
	uint32_t kslBase = KslTable[ data >> 6 ];
	uint32_t keyCode = ( data & 0x1c00) >> 9;
	if ( chip->reg08 & 0x40 ) {
		keyCode |= ( data & 0x100)>>8;	/* notesel == 1 */
	} else {
		keyCode |= ( data & 0x200)>>9;	/* notesel == 0 */
	}
	//Add the keycode and ksl into the highest bits of chanData
	data |= (keyCode << SHIFT_KEYCODE) | ( kslBase << SHIFT_KSLBASE );
	Channel_SetChanData( ch + 0, chip, data );
	if ( fourOp & 0x3f ) {
		Channel_SetChanData( ch + 1, chip, data );
	}
}

void Channel_UpdateSynth( Channel *ch, const Chip* chip ) {
	//Select the new synth mode
	if ( chip->opl3Active ) {
		//4-op mode enabled for this channel
		if ( (chip->reg104 & ch->fourMask) & 0x3f ) {
			Channel* chan0, *chan1;
			uint8_t synth;
			//Check if it's the 2nd channel in a 4-op
			if ( !(ch->fourMask & 0x80 ) ) {
				chan0 = ch;
				chan1 = ch + 1;
			} else {
				chan0 = ch - 1;
				chan1 = ch;
			}

			synth = ( (chan0->regC0 & 1) << 0 )| (( chan1->regC0 & 1) << 1 );
			
			switch ( synth ) {
			case 0:
				chan0->synthHandler = Channel_Block_sm3FMFM;// &Channel::BlockTemplate< sm3FMFM >;
				break;
			case 1:
				chan0->synthHandler = Channel_Block_sm3AMFM;// &Channel::BlockTemplate< sm3AMFM >;
				break;
			case 2:
				chan0->synthHandler = Channel_Block_sm3FMAM;// &Channel::BlockTemplate< sm3FMAM >;
				break;
			case 3:
				chan0->synthHandler = Channel_Block_sm3AMAM;// &Channel::BlockTemplate< sm3AMAM >;
				break;
			}
		//Disable updating percussion channels
		} else if ((ch->fourMask & 0x40) && ( chip->regBD & 0x20) ) {

		//Regular dual op, am or fm
		} else if (ch->regC0 & 1 ) {
			ch->synthHandler = Channel_Block_sm3AM;//&Channel::BlockTemplate< sm3AM >;
		} else {
			ch->synthHandler = Channel_Block_sm3FM;//&Channel::BlockTemplate< sm3FM >;
		}
		ch->maskLeft = (ch->regC0 & 0x10 ) ? -1 : 0;
		ch->maskRight = (ch->regC0 & 0x20 ) ? -1 : 0;
	//opl2 active
	} else { 
		//Disable updating percussion channels
		if ( (ch->fourMask & 0x40) && ( chip->regBD & 0x20 ) ) {

		//Regular dual op, am or fm
		} else if (ch->regC0 & 1 ) {
			ch->synthHandler = Channel_Block_sm2AM;//&Channel::BlockTemplate< sm2AM >;
		} else {
			ch->synthHandler = Channel_Block_sm2FM;//&Channel::BlockTemplate< sm2FM >;
		}
	}
}

void Channel_WriteA0( Channel* ch, const Chip* chip, uint8_t val ) {
	uint8_t fourOp = chip->reg104 & chip->opl3Active & ch->fourMask;
	uint32_t change;
	//Don't handle writes to silent fourop channels
	if ( fourOp > 0x80 )
		return;
	change = (ch->chanData ^ val ) & 0xff;
	if ( change ) {
		ch->chanData ^= change;
		Channel_UpdateFrequency( ch, chip, fourOp );
	}
}

void Channel_WriteB0( Channel* ch, const Chip* chip, uint8_t val ) {
	uint8_t fourOp = chip->reg104 & chip->opl3Active & ch->fourMask;
	Bitu change;
	//Don't handle writes to silent fourop channels
	if ( fourOp > 0x80 )
		return;
	change = (ch->chanData ^ ( (unsigned int)val << 8u ) ) & 0x1f00u;
	if ( change ) {
		ch->chanData ^= change;
		Channel_UpdateFrequency( ch, chip, fourOp );
	}
	//Check for a change in the keyon/off state
	if ( !(( val ^ ch->regB0) & 0x20))
		return;
	
	ch->regB0 = val;
	
	if ( val & 0x20 ) {
//		printf("Keyon\n");
		Operator_KeyOn( &CH_OP(ch, 0), 0x1 );
		Operator_KeyOn( &CH_OP(ch, 1), 0x1 );
		if ( fourOp & 0x3f ) {
			Operator_KeyOn( &CH_OP(ch + 1, 0), 1 );
			Operator_KeyOn( &CH_OP(ch + 1, 1), 1 );
		}
	} else {
		Operator_KeyOff( &CH_OP(ch, 0), 0x1 );
		Operator_KeyOff( &CH_OP(ch, 1), 0x1 );
		if ( fourOp & 0x3f ) {
			Operator_KeyOff( &CH_OP(ch + 1, 0), 1 );
			Operator_KeyOff( &CH_OP(ch + 1, 1), 1 );
		}
	}
}

void Channel_WriteC0( Channel* ch, const Chip* chip, uint8_t val ) {
	uint8_t change = val ^ ch->regC0;
	if (!change)
		return;
	ch->regC0 = val;
	ch->feedback = (ch->regC0 >> 1) & 7;
	if (ch->feedback) {
		//We shift the input to the right 10 bit wave index value
		ch->feedback = 9 - ch->feedback;
	}
	else {
		ch->feedback = 31;
	}
	Channel_UpdateSynth( ch, chip );
}

static inline uint32_t Chip_ForwardNoise( Chip* chip ) {
	uint32_t count;
	chip->noiseCounter += NoiseAdd;
	count = chip->noiseCounter >> LFO_SH;
	chip->noiseCounter &= ((1UL<<LFO_SH) - 1);
	for ( ; count > 0; --count ) {
		//Noise calculation from mame
		chip->noiseValue ^= ( 0x800302UL ) & ( 0UL - (chip->noiseValue & 1UL ) );
		chip->noiseValue >>= 1;
	}
	return chip->noiseValue;
}

static void Channel_GeneratePercussion_OPL2( Channel *ch, Chip* chip, int16_t* output ) {
	//BassDrum
	int32_t mod = (int32_t)((uint32_t)(ch->old[0] + ch->old[1]) >> ch->feedback);

	ch->old[0] = ch->old[1];
	ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );

	//When bassdrum is in AM mode first operator is ignoed
	if ( ch->regC0 & 1 ) {
		mod = 0;
	} else {
		mod = ch->old[0];
	}

	{
		int32_t sample = (int32_t)Operator_GetSample( &CH_OP(ch, 1), mod );

		//Precalculate stuff used by other outputs
		uint32_t noiseBit = Chip_ForwardNoise(chip) & 0x1;
		uint32_t c2 = (uint32_t)Operator_ForwardWave(&CH_OP(ch, 2));
		uint32_t c5 = (uint32_t)Operator_ForwardWave(&CH_OP(ch, 5));
		uint32_t phaseBit = (((c2 & 0x88) ^ ((c2<<5) & 0x80)) | ((c5 ^ (c5<<2)) & 0x20)) ? 0x02 : 0x00;
	
		{
			//Hi-Hat
			uint32_t hhVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 2));
			if ( !ENV_SILENT( hhVol ) ) {
				uint32_t hhIndex = (phaseBit<<8) | (0x34 << ( phaseBit ^ (noiseBit << 1 )));
				sample += (int32_t)Operator_GetWave(&CH_OP(ch, 2), hhIndex, hhVol );
			}
		}

		{
			//Snare Drum
			uint32_t sdVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 3));
			if ( !ENV_SILENT( sdVol ) ) {
				uint32_t sdIndex = ( 0x100 + (c2 & 0x100) ) ^ ( noiseBit << 8 );
				sample += (int32_t)Operator_GetWave(&CH_OP(ch, 3), sdIndex, sdVol );
			}
		}

		{
			//Tom-tom
			sample += (int32_t)Operator_GetSample(&CH_OP(ch, 4), 0);
		}

		{
			//Top-Cymbal
			uint32_t tcVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 5));
			if ( !ENV_SILENT( tcVol ) ) {
				uint32_t tcIndex = (1 + phaseBit) << 8;
				sample += (int32_t)Operator_GetWave( &CH_OP(ch, 5), tcIndex, tcVol );
			}
		}

		sample <<= 1;
		output[0] += (int16_t) sample;
		output[1] += (int16_t) sample;
	}
}

static void Channel_GeneratePercussion_OPL3( Channel *ch, Chip* chip, int16_t* output ) {
	//BassDrum
	int32_t mod = (int32_t)((uint32_t)(ch->old[0] + ch->old[1]) >> ch->feedback);

	ch->old[0] = ch->old[1];
	ch->old[1] = (int32_t)Operator_GetSample( &CH_OP(ch, 0), mod );

	//When bassdrum is in AM mode first operator is ignoed
	if ( ch->regC0 & 1 ) {
		mod = 0;
	} else {
		mod = ch->old[0];
	}

	{
		int32_t sample = (int32_t)Operator_GetSample( &CH_OP(ch, 1), mod );

		//Precalculate stuff used by other outputs
		uint32_t noiseBit = Chip_ForwardNoise(chip) & 0x1;
		uint32_t c2 = (uint32_t)Operator_ForwardWave(&CH_OP(ch, 2));
		uint32_t c5 = (uint32_t)Operator_ForwardWave(&CH_OP(ch, 5));
		uint32_t phaseBit = (((c2 & 0x88) ^ ((c2<<5) & 0x80)) | ((c5 ^ (c5<<2)) & 0x20)) ? 0x02 : 0x00;
	
		{
			//Hi-Hat
			uint32_t hhVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 2));
			if ( !ENV_SILENT( hhVol ) ) {
				uint32_t hhIndex = (phaseBit<<8) | (0x34 << ( phaseBit ^ (noiseBit << 1 )));
				sample += (int32_t)Operator_GetWave(&CH_OP(ch, 2), hhIndex, hhVol );
			}
		}

		{
			//Snare Drum
			uint32_t sdVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 3));
			if ( !ENV_SILENT( sdVol ) ) {
				uint32_t sdIndex = ( 0x100 + (c2 & 0x100) ) ^ ( noiseBit << 8 );
				sample += (int32_t)Operator_GetWave(&CH_OP(ch, 3), sdIndex, sdVol );
			}
		}

		{
			//Tom-tom
			sample += (int32_t)Operator_GetSample(&CH_OP(ch, 4), 0);
		}

		{
			//Top-Cymbal
			uint32_t tcVol = (uint32_t)Operator_ForwardVolume(&CH_OP(ch, 5));
			if ( !ENV_SILENT( tcVol ) ) {
				uint32_t tcIndex = (1 + phaseBit) << 8;
				sample += (int32_t)Operator_GetWave( &CH_OP(ch, 5), tcIndex, tcVol );
			}
		}

		sample <<= 1;
		output[0] += (int16_t) sample;
		output[1] += (int16_t) sample;
	}
}


/*
	Chip
*/

static void InitTables( void );

#ifdef DUMP_TABLES
static void DumpTables( Chip* chip );
#endif

/* MSC800 memset is sus... */
static inline void Chip_Memset(void *ptr, uint8_t value, uint32_t size) {
    uint8_t *u8ptr = (uint8_t *) ptr;
    while (size--) {
        *u8ptr = value;
        u8ptr++;
    }
}


void Chip_Reset( Chip* chip, bool _opl3Mode, uint32_t rate ) {
	uint16_t i;
	Chip_Memset(chip, 0, sizeof(Chip));

	for (i = 0; i < 18; i++) {
		Channel_Reset(&chip->chan[i]);
	}

	chip->opl3Mode = _opl3Mode;
	chip->reg08 = 0;
	chip->reg04 = 0;
	chip->regBD = 0;
	chip->reg104 = 0;
	chip->opl3Active = 0;

	InitTables();

	Chip_Setup(chip, rate);

#ifdef DUMP_TABLES
	DumpTables(chip);
#endif
}

static inline uint32_t Chip_ForwardLFO( Chip* chip, uint32_t samples ) {
	//Check hom many samples there can be done before the value changes
	uint32_t todo = LFO_MAX - chip->lfoCounter;
	uint32_t count = (todo + LfoAdd - 1) / LfoAdd;

	//Current vibrato value, runs 4x slower than tremolo
	chip->vibratoSign = ( VibratoTable[ chip->vibratoIndex >> 2] ) >> 7;
	chip->vibratoShift = ( VibratoTable[ chip->vibratoIndex >> 2] & 7) + chip->vibratoStrength; 
	chip->tremoloValue = TremoloTable[ chip->tremoloIndex ] >> chip->tremoloStrength;

	if ( count > samples ) {
		count = samples;
		chip->lfoCounter += count * LfoAdd;
	} else {
		chip->lfoCounter += count * LfoAdd;
		chip->lfoCounter &= (LFO_MAX - 1UL);
		//Maximum of 7 vibrato value * 4
		chip->vibratoIndex = ( chip->vibratoIndex + 1 ) & 31;
		//Clip tremolo to the the table size
		if ( chip->tremoloIndex + 1 < TREMOLO_TABLE  )
			++(chip->tremoloIndex);
		else
			chip->tremoloIndex = 0;
	}

	return count;
}


static inline void Chip_WriteBD( Chip* chip, uint8_t val ) {
	uint8_t change = chip->regBD ^ val;
	if ( !change )
		return;
	chip->regBD = val;
	//TODO could do this with shift and xor?
	chip->vibratoStrength = (val & 0x40) ? 0x00 : 0x01;
	chip->tremoloStrength = (val & 0x80) ? 0x00 : 0x02;
	if ( val & 0x20 ) {
		//Drum was just enabled, make sure channel 6 has the right synth
		if ( change & 0x20 ) {
			if ( chip->opl3Active ) {
				chip->chan[6].synthHandler = Channel_Block_sm3Percussion;// &Channel::BlockTemplate< sm3Percussion >; 
			} else {
				chip->chan[6].synthHandler = Channel_Block_sm2Percussion;// &Channel::BlockTemplate< sm2Percussion >; 
			}
		}
		//Bass Drum
		if ( val & 0x10 ) {
			Operator_KeyOn(&(chip->chan[6].op[0]), 0x2 );
			Operator_KeyOn(&(chip->chan[6].op[1]), 0x2 );
		} else {
			Operator_KeyOff(&(chip->chan[6].op[0]), 0x2 );
			Operator_KeyOff(&(chip->chan[6].op[1]), 0x2 );
		}
		//Hi-Hat
		if ( val & 0x1 ) {
			Operator_KeyOn(&(chip->chan[7].op[0]), 0x2 );
		} else {
			Operator_KeyOff(&(chip->chan[7].op[0]), 0x2 );
		}
		//Snare
		if ( val & 0x8 ) {
			Operator_KeyOn(&(chip->chan[7].op[1]), 0x2 );
		} else {
			Operator_KeyOff(&(chip->chan[7].op[1]), 0x2 );
		}
		//Tom-Tom
		if ( val & 0x4 ) {
			Operator_KeyOn(&(chip->chan[8].op[0]), 0x2 );
		} else {
			Operator_KeyOff(&(chip->chan[8].op[0]), 0x2 );
		}
		//Top Cymbal
		if ( val & 0x2 ) {
			Operator_KeyOn(&(chip->chan[8].op[1]), 0x2 );
		} else {
			Operator_KeyOff(&(chip->chan[8].op[1]), 0x2 );
		}
	//Toggle keyoffs when we turn off the percussion
	} else if ( change & 0x20 ) {
		//Trigger a reset to setup the original synth handler
		//This makes it call
		Channel_UpdateSynth( &chip->chan[6], chip );
		Operator_KeyOff(&(chip->chan[6].op[0]), 0x2 );
		Operator_KeyOff(&(chip->chan[6].op[1]), 0x2 );
		Operator_KeyOff(&(chip->chan[7].op[0]), 0x2 );
		Operator_KeyOff(&(chip->chan[7].op[1]), 0x2 );
		Operator_KeyOff(&(chip->chan[8].op[0]), 0x2 );
		Operator_KeyOff(&(chip->chan[8].op[1]), 0x2 );
	}
}


#define REGOP( chip, _FUNC_ )															\
	index = ( ( reg >> 3) & 0x20 ) | ( reg & 0x1f );								\
	if ( OpOffsetTable[ index ] ) {													\
		Operator* regOp = (Operator*)( ((char *)chip ) + OpOffsetTable[ index ]-1 );	\
		_FUNC_( regOp, chip, val );													\
	}

#define REGCHAN( chip, _FUNC_ )																\
	index = ( ( reg >> 4) & 0x10 ) | ( reg & 0xf );										\
	if ( ChanOffsetTable[ index ] ) {													\
		Channel* regChan = (Channel*)( ((char *)chip ) + ChanOffsetTable[ index ]-1 );	\
		_FUNC_( regChan, chip, val );														\
	}

//Update the 0xc0 register for all channels to signal the switch to mono/stereo handlers
static inline void Chip_UpdateSynths( Chip* chip ) {
	uint16_t i;
	for (i = 0; i < 18; i++) {
		Channel_UpdateSynth(&chip->chan[i], chip);
	}
}

void Chip_WriteReg( Chip* chip, uint16_t reg, uint8_t val ) {
	Bitu index;
//	printf("WriteReg %p %u %u\n", chip, reg, val);
	switch ( (reg & 0xf0) >> 4 ) {
	case 0x00 >> 4:
		if ( reg == 0x01 ) {
			//When the chip is running in opl3 compatible mode, you can't actually disable the waveforms
			chip->waveFormMask = ( (val & 0x20) || chip->opl3Mode ) ? 0x7 : 0x0; 
		} else if ( reg == 0x104 ) {
			//Only detect changes in lowest 6 bits
			if ( !((chip->reg104 ^ val) & 0x3f) )
				return;
			//Always keep the highest bit enabled, for checking > 0x80
			chip->reg104 = 0x80 | ( val & 0x3f );
			//Switch synths when changing the 4op combinations
			Chip_UpdateSynths(chip);
		} else if ( reg == 0x105 ) {
			//MAME says the real opl3 doesn't reset anything on opl3 disable/enable till the next write in another register
			if ( !((chip->opl3Active ^ val) & 1 ) )
				return;
			chip->opl3Active = ( val & 1 ) ? 0xff : 0;
			//Just update the synths now that opl3 must have been enabled
			//This isn't how the real card handles it but need to switch to stereo generating handlers
			Chip_UpdateSynths(chip);
		} else if ( reg == 0x08 ) {
			chip->reg08 = val;
		}
	case 0x10 >> 4:
		break;
	case 0x20 >> 4:
	case 0x30 >> 4:
		REGOP( chip, Operator_Write20 );
		break;
	case 0x40 >> 4:
	case 0x50 >> 4:
		REGOP( chip, Operator_Write40 );
		break;
	case 0x60 >> 4:
	case 0x70 >> 4:
		REGOP( chip, Operator_Write60 );
		break;
	case 0x80 >> 4:
	case 0x90 >> 4:
		REGOP( chip, Operator_Write80 );
		break;
	case 0xa0 >> 4:
		REGCHAN( chip, Channel_WriteA0 );
		break;
	case 0xb0 >> 4:
		if ( reg == 0xbd ) {
			Chip_WriteBD( chip, val );
		} else {
			REGCHAN( chip, Channel_WriteB0 );
		}
		break;
	case 0xc0 >> 4:
		REGCHAN( chip, Channel_WriteC0 );
	case 0xd0 >> 4:
		break;
	case 0xe0 >> 4:
	case 0xf0 >> 4:
		REGOP( chip, Operator_WriteE0 );
		break;
	}
}


uint32_t Chip_WriteAddr( Chip* chip, uint32_t port, uint8_t val ) {
	switch ( port & 3 ) {
	case 0:
		return val;
	case 2:
		if ( chip->opl3Active || (val == 0x05u) )
			return 0x100u | val;
		else 
			return val;
	}
	return 0u;
}

int Chip_GenerateBlock2( Chip* chip, Bitu total, int16_t* output ) {
	int16_t* base = output;
	while ( total > 0UL ) {
		uint32_t samples = Chip_ForwardLFO( chip, (uint32_t)total );
		Channel* ch;
//		_asm{int 3};
		// memset(output, 0, (size_t) (sizeof(int16_t) * samples * 2));
		Chip_Memset(output, 0, samples << 2);
//		int count = 0;
		for( ch = chip->chan; ch < chip->chan + 9; ) {
//			count++;
//			printf("total = %lu, samples = %lu ch = %p limit %p\n", total,  samples, ch, chip->chan+9);
			ch = ch->synthHandler( ch, chip, samples, output );
//			getch();
//			break;
		}

		total -= samples;
		output += samples * 2;
	}
	return output - base;
}

int Chip_GenerateBlock3( Chip* chip, Bitu total, int16_t* output  ) {
	int16_t* base = output;
	while ( total > 0UL ) {
		uint32_t samples = Chip_ForwardLFO( chip, (uint32_t)total );
		Channel* ch;
		Chip_Memset(output, 0, samples << 2);
//		int count = 0;
		for( ch = chip->chan; ch < chip->chan + 18; ) {
//			count++;
			ch = (ch->synthHandler)(ch, chip, samples, output );
		}
		total -= samples;
		output += samples * 2;
	}
	return output - base;
}

int Chip_Generate( Chip* chip, int16_t* buffer, uint32_t samples) {
	if ( !chip->opl3Active ) {
		return Chip_GenerateBlock2( chip, samples, buffer );
	} else {
		return Chip_GenerateBlock3( chip, samples, buffer );
	}
}

#ifdef DUMP_TABLES
void DumpTables( Chip* chip ) {
	FILE *f = fopen("precalc.inc", "w");
	uint16_t i;

	fprintf(f, "static uint32_t PrecalcFreqMul[16] = { \n");
	for (i = 0; i < 16; i++) {
		fprintf(f, "%luUL, ", chip->freqMul[i]);
	}
	fprintf(f, "\n}; \n");

	fprintf(f, "static uint32_t PrecalcLinearRates[76] = { \n");
	for (i = 0; i < 76; i++) {
		fprintf(f, "%luUL, ", chip->linearRates[i]);
	}
	fprintf(f, "\n}; \n");

	fprintf(f, "static uint32_t PrecalcAttackRates[76] = { \n");
	for (i = 0; i < 76; i++) {
		fprintf(f, "%luUL, ", chip->attackRates[i]);
	}
	fprintf(f, "\n}; \n");

#if ( DBOPL_WAVE == WAVE_HANDLER ) || ( DBOPL_WAVE == WAVE_TABLELOG )
	fprintf(f, "static uint16_t ExpTable[256] = { \n");
	for (i = 0; i < 256; i++) {
		fprintf(f, "%u, ", ExpTable[i]);
		if (i % 16 == 15) fprintf(f, "\n");
	}
	fprintf(f, "\n}; \n");
#endif

#if ( DBOPL_WAVE == WAVE_HANDLER )
	fprintf(f, "static uint16_t SinTable[512] = { \n");
	for (i = 0; i < 512; i++) {
		fprintf(f, "%u, ", SinTable[i]);
		if (i % 16 == 15) fprintf(f, "\n");
	}
	fprintf(f, "\n}; \n");
#endif

#if (( DBOPL_WAVE == WAVE_TABLELOG ) || ( DBOPL_WAVE == WAVE_TABLEMUL ))
	fprintf(f, "static uint16_t WaveTable[8 * 512] = { \n");
	for (i = 0; i < 8 * 512; i++) {
		fprintf(f, "%u, ", WaveTable[i]);
		if (i % 16 == 15) fprintf(f, "\n");
	}
	fprintf(f, "\n}; \n");
#endif

	fprintf(f, "static uint8_t KslTable[128] = { \n");
	for (i = 0; i < 128; i++) {
		fprintf(f, "%u, ", KslTable[i]);
		if (i % 16 == 15) fprintf(f, "\n");
	}
	fprintf(f, "\n}; \n");
	
	fprintf(f, "static uint8_t TremoloTable[TREMOLO_TABLE] = { \n");
	for (i = 0; i < TREMOLO_TABLE; i++) {
		fprintf(f, "%u, ", TremoloTable[i]);
		if (i % 16 == 15) fprintf(f, "\n");
	}
	fprintf(f, "\n}; \n");

	fprintf(f, "#define PrecalcNoiseAdd (%luUL)\n", (long unsigned) chip->noiseAdd);
	fprintf(f, "#define PrecalcLfoAdd (%luUL)\n", (long unsigned) chip->lfoAdd);
	fclose(f);
}
#endif

void Chip_Setup( Chip* chip, uint32_t rate ) {
#ifndef PRECALC_TBL
	double scale = OPLRATE / (double)rate;
#endif

	uint16_t i;

	//Noise counter is run at the same precision as general waves
	chip->noiseCounter = 0;
	chip->noiseValue = 1;	//Make sure it triggers the noise xor the first time
	//The low frequency oscillation counter
	//Every time his overflows vibrato and tremoloindex are increased
	chip->lfoCounter = 0;
	chip->vibratoIndex = 0;
	chip->tremoloIndex = 0;

#ifndef PRECALC_TBL
	chip->noiseAdd = (uint32_t)( 0.5 + scale * ( 1UL << LFO_SH ) );
	chip->lfoAdd = (uint32_t)( 0.5 + scale * ( 1UL << LFO_SH ) );

	//With higher octave this gets shifted up
	//-1 since the freqCreateTable = *2
#ifdef WAVE_PRECISION
	{
		double freqScale = ( 1 << 7 ) * scale * ( 1UL << ( WAVE_SH - 1 - 10));
		for ( i = 0; i < 16; i++ ) {
			chip->freqMul[i] = (uint32_t)( 0.5 + freqScale * FreqCreateTable[ i ] );
		}
	}
#else 
	{
		uint32_t freqScale = (uint32_t)( 0.5 + scale * ( 1UL << ( WAVE_SH - 1 - 10)));
		for ( i = 0; i < 16; i++ ) {
			chip->freqMul[i] = freqScale * FreqCreateTable[ i ];
		}	
	}
#endif

	//-3 since the real envelope takes 8 steps to reach the single value we supply
	for ( i = 0; i < 76; i++ ) {
		uint8_t index, shift;
		EnvelopeSelect( (uint8_t) i, &index, &shift );
		chip->linearRates[i] = (uint32_t)( scale * (((uint32_t) EnvelopeIncreaseTable[ index ]) << ( RATE_SH + ENV_EXTRA - shift - 3 )));
	}
//	int32_t attackDiffs[62];
	//Generate the best matching attack rate
	for ( i = 0; i < 62; i++ ) {
		uint8_t index, shift;
		EnvelopeSelect( (uint8_t) i, &index, &shift );
		{
			//Original amount of samples the attack would take
			int32_t originalAmount = (int32_t)((uint32_t)( (((uint32_t) AttackSamplesTable[ index ]) << shift) / scale));
			int32_t guessAdd = (int32_t)((uint32_t)( scale * (((uint32_t) EnvelopeIncreaseTable[ index ]) << ( RATE_SH - shift - 3 ))));
			int32_t bestAdd = guessAdd;
			uint32_t bestDiff = 1UL << 30;
			uint16_t passes;

			for( passes = 0; passes < 16; passes ++ ) {
				int32_t volume = ENV_MAX;
				int32_t samples = 0L;
				uint32_t count = 0UL;
				int32_t diff;
				uint32_t lDiff;
				double correct;
				while ( volume > 0L && samples < (originalAmount * 2L) ) {
					int32_t change;
					count += (uint32_t)guessAdd;
					change = (int32_t)(count >> RATE_SH);
					count &= RATE_MASK;
					if (change) { // less than 1 %
						/* BUG IN MICROSOFT C 8.x: COMPLEMENT CAN LOSE SIGN BIT CAUSING CORRUPTION HERE!! WTF!!!! */
						/* volume += ( ~volume * change ) >> 3; */
						int32_t volCompl = -volume - 1L;
						volume += ( volCompl * change ) >> 3;
					}
					samples++;

				}

				diff = originalAmount - samples;
				lDiff = labs( diff );
				//Init last on first pass
				if ( lDiff < bestDiff ) {
					bestDiff = lDiff;
					bestAdd = guessAdd;
					//We hit an exactly matching sample count
					if ( bestDiff == 0UL )
						break;
				}
				//Linear correction factor, not exactly perfect but seems to work
				correct = (double) (originalAmount - diff) / (double)originalAmount;
				guessAdd = (int32_t)(guessAdd * correct);
				//Below our target
				if ( diff < 0L ) {
					//Always add one here for rounding, an overshoot will get corrected by another pass decreasing
					guessAdd++;
				}
			}
			chip->attackRates[i] = (uint32_t)bestAdd;
		}	

		//Keep track of the diffs for some debugging
//		attackDiffs[i] = bestDiff;
	}

	for ( i = 62; i < 76; i++ ) {
		//This should provide instant volume maximizing
		chip->attackRates[i] = 8UL << RATE_SH;
	}

#endif // PRECALC_TBL

	//Setup the channels with the correct four op flags
	//Channels are accessed through a table so they appear linear here
	chip->chan[ 0].fourMask = 0x00 | ( 1 << 0 );
	chip->chan[ 1].fourMask = 0x80 | ( 1 << 0 );
	chip->chan[ 2].fourMask = 0x00 | ( 1 << 1 );
	chip->chan[ 3].fourMask = 0x80 | ( 1 << 1 );
	chip->chan[ 4].fourMask = 0x00 | ( 1 << 2 );
	chip->chan[ 5].fourMask = 0x80 | ( 1 << 2 );

	chip->chan[ 9].fourMask = 0x00 | ( 1 << 3 );
	chip->chan[10].fourMask = 0x80 | ( 1 << 3 );
	chip->chan[11].fourMask = 0x00 | ( 1 << 4 );
	chip->chan[12].fourMask = 0x80 | ( 1 << 4 );
	chip->chan[13].fourMask = 0x00 | ( 1 << 5 );
	chip->chan[14].fourMask = 0x80 | ( 1 << 5 );

	//mark the percussion channels
	chip->chan[ 6].fourMask = 0x40;
	chip->chan[ 7].fourMask = 0x40;
	chip->chan[ 8].fourMask = 0x40;

	//Clear Everything in opl3 mode
	Chip_WriteReg( chip, 0x105, 0x1 );
	for ( i = 0; i < 512; i++ ) {
		if ( i == 0x105 )
			continue;
		Chip_WriteReg( chip, i, 0xff );
		Chip_WriteReg( chip, i, 0x0 );
	}
	Chip_WriteReg( chip, 0x105, 0x0 );
	//Clear everything in opl2 mode
	for ( i = 0; i < 255; i++ ) {
		Chip_WriteReg( chip, i, 0xff );
		Chip_WriteReg( chip, i, 0x0 );
	}
}

static bool doneTables = false;
static void InitTables( void ) {
	uint16_t i;
	uint16_t oct;
	
	if ( doneTables )
		return;

	doneTables = true;

#ifndef PRECALC_TBL

#if ( DBOPL_WAVE == WAVE_HANDLER ) || ( DBOPL_WAVE == WAVE_TABLELOG )
	//Exponential volume table, same as the real adlib
	for ( i = 0; i < 256; i++ ) {
		//Save them in reverse
		ExpTable[i] = (uint16_t)( 0.5 + ( pow(2.0, (255.0 - (double) i) * ( 1.0 /256.0 ) )-1.0) * 1024.0 );
		ExpTable[i] += 1024; //or remove the -1 oh well :)
		//Preshift to the left once so the final volume can shift to the right
		ExpTable[i] *= 2;
	}
#endif
#if ( DBOPL_WAVE == WAVE_HANDLER )
	//Add 0.5 for the trunc rounding of the integer cast
	//Do a PI sinetable instead of the original 0.5 PI
	for ( i = 0; i < 512; i++ ) {
		SinTable[i] = (int16_t)( 0.5 - log10( sin( (i + 0.5) * (PI / 512.0) ) ) / log10(2.0)*256 );
	}
#endif
#if ( DBOPL_WAVE == WAVE_TABLEMUL )
	//Multiplication based tables
	for ( i = 0; i < 384; i++ ) {
		int s = i * 8;
		//TODO maybe keep some of the precision errors of the original table?
		double val = ( 0.5 + ( pow(2.0, -1.0 + ( 255 - s) * ( 1.0 /256 ) )) * ( 1UL << MUL_SH ));
		MulTable[i] = (uint16_t)(val);
	}

	//Sine Wave Base
	for ( i = 0; i < 512; i++ ) {
		WaveTable[ 0x0200 + i ] = (int16_t)(sin( ((double) i + 0.5) * (PI / 512.0) ) * 4084.0);
		WaveTable[ 0x0000 + i ] = -WaveTable[ 0x200 + i ];
	}
	//Exponential wave
	for ( i = 0; i < 256; i++ ) {
		WaveTable[ 0x700 + i ] = (int16_t)( 0.5 + ( pow(2.0, -1.0 + ( 255.0 - (double) i * 8.0) * ( 1.0 /256.0 ) ) ) * 4085.0 );
		WaveTable[ 0x6ff - i ] = -WaveTable[ 0x700 + i ];
	}
#endif
#if ( DBOPL_WAVE == WAVE_TABLELOG )
	//Sine Wave Base
	for ( i = 0; i < 512; i++ ) {
		WaveTable[ 0x0200 + i ] = (int16_t)( 0.5 - log10( sin( (i + 0.5) * (PI / 512.0) ) ) / log10(2.0)*256.0 );
		WaveTable[ 0x0000 + i ] = ((int16_t)0x8000) | WaveTable[ 0x200 + i];
	}
	//Exponential wave
	for ( i = 0; i < 256; i++ ) {
		WaveTable[ 0x700 + i ] = i * 8;
		WaveTable[ 0x6ff - i ] = ((int16_t)0x8000) | i * 8;
	} 
#endif

	//	|    |//\\|____|WAV7|//__|/\  |____|/\/\|
	//	|\\//|    |    |WAV7|    |  \/|    |    |
	//	|06  |0126|27  |7   |3   |4   |4 5 |5   |

#if (( DBOPL_WAVE == WAVE_TABLELOG ) || ( DBOPL_WAVE == WAVE_TABLEMUL ))
	for ( i = 0; i < 256; i++ ) {
		//Fill silence gaps
		WaveTable[ 0x400 + i ] = WaveTable[0];
		WaveTable[ 0x500 + i ] = WaveTable[0];
		WaveTable[ 0x900 + i ] = WaveTable[0];
		WaveTable[ 0xc00 + i ] = WaveTable[0];
		WaveTable[ 0xd00 + i ] = WaveTable[0];
		//Replicate sines in other pieces
		WaveTable[ 0x800 + i ] = WaveTable[ 0x200 + i ];
		//double speed sines
		WaveTable[ 0xa00 + i ] = WaveTable[ 0x200 + i * 2 ];
		WaveTable[ 0xb00 + i ] = WaveTable[ 0x000 + i * 2 ];
		WaveTable[ 0xe00 + i ] = WaveTable[ 0x200 + i * 2 ];
		WaveTable[ 0xf00 + i ] = WaveTable[ 0x200 + i * 2 ];
	} 
#endif

	//Create the ksl table
	for ( oct = 0; oct < 8; oct++ ) {
		int base = oct * 8;
		for ( i = 0; i < 16; i++ ) {
			int val = base - KslCreateTable[i];
			if ( val < 0 )
				val = 0;
			//*4 for the final range to match attenuation range
			KslTable[ oct * 16 + i ] = val * 4;
		}
	}
	//Create the Tremolo table, just increase and decrease a triangle wave
	for ( i = 0; i < TREMOLO_TABLE / 2; i++ ) {
		uint8_t val = i << ENV_EXTRA;
		TremoloTable[i] = val;
		TremoloTable[TREMOLO_TABLE - 1 - i] = val;
	}

#else
	(void) oct;
#endif // PRECALC_TBL

	// These are instance specific so we generate them even with PRECALC_TBL

	//Create a table with offsets of the channels from the start of the chip
	for ( i = 0; i < 32; i++ ) {
		Bitu index = i & 0xf;
		if ( index >= 9 ) {
			ChanOffsetTable[i] = 0;
			continue;
		}
		//Make sure the four op channels follow each other
		if ( index < 6 ) {
			index = (index % 3) * 2 + ( index / 3 );
		}
		//Add back the bits for highest ones
		if ( i >= 16 )
			index += 9;
		ChanOffsetTable[i] = 1+(uint16_t)(index*sizeof(Channel));
	}
	//Same for operators
	for ( i = 0; i < 64; i++ ) {
		Bitu chNum;
		Bitu opNum;
		if ( i % 8 >= 6 || ( (i / 8) % 4 == 3 ) ) {
			OpOffsetTable[i] = 0;
			continue;
		}
		chNum = (i / 8) * 3 + (i % 8) % 3;
		//Make sure we use 16 and up for the 2nd range to match the chanoffset gap
		if ( chNum >= 12 )
			chNum += 16 - 12;
		opNum = ( i % 8 ) / 3;
		OpOffsetTable[i] = ChanOffsetTable[chNum]+(uint16_t)(opNum*sizeof(Operator));
	}
#if 0
	DBOPL::Chip* chip = 0;
	//Stupid checks if table's are correct
	for ( Bitu i = 0; i < 18; i++ ) {
		uint32_t find = (uint16_t)( &(chip->chan[ i ]) );
		for ( Bitu c = 0; c < 32; c++ ) {
			if ( ChanOffsetTable[c] == find+1 ) {
				find = 0;
				break;
			}
		}
		if ( find ) {
			find = find;
		}
	}
	for ( Bitu i = 0; i < 36; i++ ) {
		uint32_t find = (uint16_t)( &(chip->chan[ i / 2 ].op[i % 2]) );
		for ( Bitu c = 0; c < 64; c++ ) {
			if ( OpOffsetTable[c] == find+1 ) {
				find = 0;
				break;
			}
		}
		if ( find ) {
			find = find;
		}
	}
#endif
}

