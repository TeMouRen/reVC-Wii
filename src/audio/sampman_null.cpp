#include "common.h"
#if !defined(AUDIO_OAL) && !defined(AUDIO_MSS)

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "MemoryMgr.h"

#ifdef GAMECUBE
#include <gccore.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#endif

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;

#ifdef GAMECUBE
namespace
{
static const bool8 GC_AUDIO_ENABLED =
#if GC_AUDIO_MASTER_ENABLE
	TRUE;
#else
	FALSE;
#endif

#if GC_AUDIO_DEBUG_LOG
#define GC_AUDIO_LOG(...) printf(__VA_ARGS__)
#else
#define GC_AUDIO_LOG(...) ((void)0)
#endif

static const uint32 GC_OUTPUT_RATE = 48000;
static const uint32 GC_DMA_BUFFER_SIZE = 8192;
static const uint32 GC_DMA_BUFFER_FRAMES = GC_DMA_BUFFER_SIZE / (sizeof(int16) * 2);
static const uint32 GC_DMA_BUFFER_COUNT = 3;
static const uint32 GC_SOURCE_BUFFER_FRAMES = 4096;

static const uint32 GC_ADPCM_FRAME_SIZE = 8;
static const uint32 GC_ADPCM_SAMPLES_PER_FRAME = 14;

static const uint32 VB_BLOCK_SIZE = 0x2000;
static const uint32 VAG_LINE_SIZE = 0x10;
static const uint32 VAG_SAMPLES_IN_LINE = 28;
static const uint32 NUM_VAG_LINES_IN_BLOCK = VB_BLOCK_SIZE / VAG_LINE_SIZE;
static const uint32 NUM_VAG_SAMPLES_IN_BLOCK = NUM_VAG_LINES_IN_BLOCK * VAG_SAMPLES_IN_LINE;
static const uint32 GC_SFX_PATH_CAPACITY = 128;
static const char *GC_SAMPLE_BANK_DESC_PATH = "AUDIO\\SFX\\SFX.SDT";
static const char *GC_SAMPLE_BANK_DATA_PATH = "AUDIO\\SFX\\SFX.RAW";

enum eDmaBufferState
{
	GC_BUFFER_FREE = 0,
	GC_BUFFER_READY,
	GC_BUFFER_PLAYING,
};

struct tAdpcmHist
{
	int16 hist1;
	int16 hist2;
};

struct tGcSampleChannel
{
	bool8 initialised;
	bool8 active;
	bool8 paused;
	bool8 effectFlag;
	bool8 reverbFlag;
	bool8 is2D;
	uint8 volume;
	uint8 pan;
	uint32 frequency;
	uint32 baseFrequency;
	uint32 loopStartBytes;
	int32 loopEndBytes;
	uint32 loopCount;
	float emittingVolume;
	float maxDistance;
	CVector position;
	const int16 *sampleData;
	uint32 sampleCount;
	uint32 cursorQ16;
	uint32 stepQ16;
};

static FILE *gSampleDescFile = NULL;
static FILE *gSampleDataFile = NULL;
static bool8 gSampleBankLoaded[MAX_SFX_BANKS];
static uint32 gSampleBankDiscStartOffset[MAX_SFX_BANKS];
static uint32 gSampleBankSize[MAX_SFX_BANKS];
static uint8 *gSampleBankMemoryStartAddress[MAX_SFX_BANKS];
static int32 gPedSlotSfx[MAX_PEDSFX];
static uint8 *gPedSlotSfxAddr[MAX_PEDSFX];
static uint8 gCurrentPedSlot = 0;
static tGcSampleChannel gSampleChannels[MAXCHANNELS + MAX2DCHANNELS];
static uint32 gSampleDataEndOffset = 0;

class CGcStreamDecoder
{
public:
	virtual ~CGcStreamDecoder() {}

	virtual bool IsOpened() const = 0;
	virtual uint32 GetLengthMS() const = 0;
	virtual uint32 GetSampleRate() const = 0;
	virtual void SeekMS(uint32 milliseconds) = 0;
	virtual uint32 DecodeFrames(int16 *buffer, uint32 maxFrames) = 0;
};

class CVagDecoder
{
	const double f[5][2] = {
		{ 0.0, 0.0 },
		{ 60.0 / 64.0, 0.0 },
		{ 115.0 / 64.0, -52.0 / 64.0 },
		{ 98.0 / 64.0, -55.0 / 64.0 },
		{ 122.0 / 64.0, -60.0 / 64.0 }
	};

	double s_1;
	double s_2;

	static int16 Quantize(double sample)
	{
		int32 value = int32(sample + 0.5);
		if (value < -32768)
			value = -32768;
		else if (value > 32767)
			value = 32767;
		return int16(value);
	}

public:
	CVagDecoder()
	{
		ResetState();
	}

	void ResetState()
	{
		s_1 = 0.0;
		s_2 = 0.0;
	}

	void DecodeLine(const uint8 *inbuf, int16 *outbuf)
	{
		double samples[VAG_SAMPLES_IN_LINE];

		int predict_nr = *inbuf++;
		int shift_factor = predict_nr & 0xF;
		predict_nr >>= 4;
		int flags = *inbuf++;
		if (flags == 7) {
			memset(outbuf, 0, sizeof(int16) * VAG_SAMPLES_IN_LINE);
			return;
		}

		for (uint32 i = 0; i < VAG_SAMPLES_IN_LINE; i += 2) {
			int d = *inbuf++;
			int16 s = int16((d & 0x0F) << 12);
			samples[i] = double(s >> shift_factor);
			s = int16((d & 0xF0) << 8);
			samples[i + 1] = double(s >> shift_factor);
		}

		for (uint32 i = 0; i < VAG_SAMPLES_IN_LINE; i++) {
			samples[i] = samples[i] + s_1 * f[predict_nr][0] + s_2 * f[predict_nr][1];
			s_2 = s_1;
			s_1 = samples[i];
			outbuf[i] = Quantize(samples[i] + 0.5);
		}
	}
};

static inline uint32 CountOfPS2Table()
{
#ifdef PS2_AUDIO_PATHS
	return uint32(sizeof(PS2StreamedNameTable) / sizeof(PS2StreamedNameTable[0]));
#else
	return 0;
#endif
}

static inline uint32 CountOfStreamedTable()
{
	return uint32(sizeof(StreamedNameTable) / sizeof(StreamedNameTable[0]));
}

static inline bool8 IsThisTrackAt16KHz(uint32 track)
{
	return track == STREAMED_SOUND_RADIO_KCHAT || track == STREAMED_SOUND_RADIO_VCPR || track == STREAMED_SOUND_RADIO_POLICE;
}

static inline bool8 IsSupportedGcRadioTrack(tTrack track)
{
	return track >= STREAMED_SOUND_RADIO_WILD && track <= STREAMED_SOUND_RADIO_WAVE
#ifndef GTA_PS2
		|| track == STREAMED_SOUND_RADIO_MP3_PLAYER
#endif
		;
}

static inline bool8 IsDirectPs2RadioWhitelistTrack(tTrack track)
{
	return track >= STREAMED_SOUND_RADIO_WILD && track <= STREAMED_SOUND_RADIO_WAVE;
}

static const char *GetRadioTrackLabel(tTrack track)
{
	switch (track) {
	case STREAMED_SOUND_RADIO_WILD: return "WILD";
	case STREAMED_SOUND_RADIO_FLASH: return "FLASH";
	case STREAMED_SOUND_RADIO_KCHAT: return "KCHAT";
	case STREAMED_SOUND_RADIO_FEVER: return "FEVER";
	case STREAMED_SOUND_RADIO_VROCK: return "VROCK";
	case STREAMED_SOUND_RADIO_VCPR: return "VCPR";
	case STREAMED_SOUND_RADIO_ESPANTOSO: return "ESPANT";
	case STREAMED_SOUND_RADIO_EMOTION: return "EMOTION";
	case STREAMED_SOUND_RADIO_WAVE: return "WAVE";
	case STREAMED_SOUND_RADIO_POLICE: return "POLICE";
	default: return "UNKNOWN";
	}
}

static bool8 CanOpenTrackPath(const char *path)
{
	if (path == NULL)
		return FALSE;

	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return FALSE;

	fclose(file);
	return TRUE;
}

static void ResetSampleChannel(tGcSampleChannel &channel)
{
	channel.initialised = FALSE;
	channel.active = FALSE;
	channel.paused = FALSE;
	channel.effectFlag = FALSE;
	channel.reverbFlag = FALSE;
	channel.is2D = FALSE;
	channel.volume = MAX_VOLUME;
	channel.pan = 63;
	channel.frequency = DIGITALRATE;
	channel.baseFrequency = DIGITALRATE;
	channel.loopStartBytes = 0;
	channel.loopEndBytes = -1;
	channel.loopCount = 1;
	channel.emittingVolume = 0.0f;
	channel.maxDistance = 0.0f;
	channel.position = CVector(0.0f, 0.0f, 0.0f);
	channel.sampleData = NULL;
	channel.sampleCount = 0;
	channel.cursorQ16 = 0;
	channel.stepQ16 = 0;
}

static void ResetSampleBankState()
{
	for (uint32 i = 0; i < MAX_SFX_BANKS; i++) {
		gSampleBankLoaded[i] = FALSE;
		gSampleBankDiscStartOffset[i] = 0;
		gSampleBankSize[i] = 0;
		gSampleBankMemoryStartAddress[i] = NULL;
	}

	for (uint32 i = 0; i < MAX_PEDSFX; i++) {
		gPedSlotSfx[i] = NO_SAMPLE;
		gPedSlotSfxAddr[i] = NULL;
	}
	gCurrentPedSlot = 0;
	gSampleDataEndOffset = 0;

	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++)
		ResetSampleChannel(gSampleChannels[i]);
}

static void CloseSampleBankFiles()
{
	if (gSampleDescFile) {
		fclose(gSampleDescFile);
		gSampleDescFile = NULL;
	}
	if (gSampleDataFile) {
		fclose(gSampleDataFile);
		gSampleDataFile = NULL;
	}
}

static void FreeSampleBankMemory()
{
	for (uint32 i = 0; i < MAX_SFX_BANKS; i++) {
		if (gSampleBankMemoryStartAddress[i] == NULL)
			continue;
#ifdef WII
		MemoryMgrFreeMem2(gSampleBankMemoryStartAddress[i]);
#else
		free(gSampleBankMemoryStartAddress[i]);
#endif
		gSampleBankMemoryStartAddress[i] = NULL;
	}

	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++)
		ResetSampleChannel(gSampleChannels[i]);
	for (uint32 i = 0; i < MAX_PEDSFX; i++)
		gPedSlotSfxAddr[i] = NULL;
}

static uint8 *AllocAlignedAudioBuffer(uint32 sizeBytes)
{
#ifdef WII
	return (uint8*)MemoryMgrMallocMem2(sizeBytes, 32);
#else
	return (uint8*)memalign(32, sizeBytes);
#endif
}

static bool8 OpenSampleBankDataPath(const char *path)
{
	if (path == NULL)
		return FALSE;
	gSampleDataFile = fopen(path, "rb");
	return gSampleDataFile != NULL;
}

static bool8 InitialiseSampleBankPaths()
{
	gSampleDescFile = fopen(GC_SAMPLE_BANK_DESC_PATH, "rb");
	if (gSampleDescFile == NULL) {
		printf("[GC-AUDIO] ERROR: missing sample desc %s\n", GC_SAMPLE_BANK_DESC_PATH);
		return FALSE;
	}

	if (!OpenSampleBankDataPath(GC_SAMPLE_BANK_DATA_PATH)) {
		printf("[GC-AUDIO] ERROR: missing sample data %s\n", GC_SAMPLE_BANK_DATA_PATH);
		CloseSampleBankFiles();
		return FALSE;
	}

	if (fseek(gSampleDataFile, 0, SEEK_END) != 0) {
		printf("[GC-AUDIO] ERROR: seek failed for sample data %s\n", GC_SAMPLE_BANK_DATA_PATH);
		CloseSampleBankFiles();
		return FALSE;
	}

	long endOffset = ftell(gSampleDataFile);
	if (endOffset <= 0) {
		printf("[GC-AUDIO] ERROR: invalid sample data size for %s (size=%ld)\n", GC_SAMPLE_BANK_DATA_PATH, endOffset);
		CloseSampleBankFiles();
		return FALSE;
	}
	gSampleDataEndOffset = uint32(endOffset);
	rewind(gSampleDataFile);
	return TRUE;
}

static bool8 InitialiseSampleTable(tSample *samples)
{
	if (gSampleDescFile == NULL || samples == NULL)
		return FALSE;

	size_t expected = sizeof(tSample) * TOTAL_AUDIO_SAMPLES;
	size_t got = fread(samples, 1, expected, gSampleDescFile);
	if (got != expected) {
		printf("[GC-AUDIO] ERROR: sample table short read (%u/%u bytes)\n", (uint32)got, (uint32)expected);
		return FALSE;
	}

	fclose(gSampleDescFile);
	gSampleDescFile = NULL;
	return TRUE;
}

static bool8 BuildSampleBankOffsets(const tSample *samples)
{
	if (samples == NULL)
		return FALSE;

	int32 nBank = SFX_BANK_0;
	for (int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		if (nBank >= MAX_SFX_BANKS)
			break;
		if (BankStartOffset[nBank] == BankStartOffset[SFX_BANK_0] + i) {
			gSampleBankDiscStartOffset[nBank] = samples[i].nOffset;
			nBank++;
		}
	}

	if (gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS] <= gSampleBankDiscStartOffset[SFX_BANK_0]) {
		printf("[GC-AUDIO] ERROR: invalid sample bank offsets bank0=%u ped=%u\n",
		       gSampleBankDiscStartOffset[SFX_BANK_0],
		       gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS]);
		return FALSE;
	}
	if (gSampleDataEndOffset <= gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS]) {
		printf("[GC-AUDIO] ERROR: sample data ends before ped bank end=%u ped=%u\n",
		       gSampleDataEndOffset,
		       gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS]);
		return FALSE;
	}

	gSampleBankSize[SFX_BANK_0] = gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS] - gSampleBankDiscStartOffset[SFX_BANK_0];
	gSampleBankSize[SFX_BANK_PED_COMMENTS] = gSampleDataEndOffset - gSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS];
	return gSampleBankSize[SFX_BANK_0] != 0;
}

static uint32 ComputeChannelStepQ16(uint32 frequency)
{
	if (frequency == 0)
		return 0;
	uint64 step = (uint64(frequency) << 16) / GC_OUTPUT_RATE;
	if (step == 0)
		step = 1;
	return uint32(step);
}

static void ByteSwapPcm16Buffer(void *buffer, uint32 sizeBytes)
{
#ifdef RW_BIG_ENDIAN
	if (buffer == NULL || (sizeBytes & 1) != 0)
		return;

	uint16 *samples = (uint16*)buffer;
	uint32 count = sizeBytes / sizeof(uint16);
	for (uint32 i = 0; i < count; i++) {
		uint16 value = samples[i];
		samples[i] = uint16((value >> 8) | (value << 8));
	}
#else
	(void)buffer;
	(void)sizeBytes;
#endif
}

static inline uint8 ClampVolume127(uint32 value)
{
	return uint8(Min(value, (uint32)MAX_VOLUME));
}

static inline uint8 ClampPan127(uint32 value)
{
	return uint8(Min(value, (uint32)127));
}

static bool8 ResolveSampleAddress(const tSample *samples, uint32 nSfx, uint8 nBank, const int16 *&sampleData, uint32 &sampleCount)
{
	sampleData = NULL;
	sampleCount = 0;

	if (samples == NULL || nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;

	const tSample &sample = samples[nSfx];
	if (sample.nSize == 0)
		return FALSE;

	if (nBank == SFX_BANK_PED_COMMENTS) {
		int32 pedSlot = SampleManager._GetPedCommentSlot(nSfx);
		if (pedSlot < 0 || pedSlot >= MAX_PEDSFX || gPedSlotSfxAddr[pedSlot] == NULL)
			return FALSE;
		sampleData = (const int16*)gPedSlotSfxAddr[pedSlot];
	} else {
		if (nBank >= MAX_SFX_BANKS)
			return FALSE;
		if (!gSampleBankLoaded[nBank] || gSampleBankMemoryStartAddress[nBank] == NULL)
			return FALSE;
		if (sample.nOffset < gSampleBankDiscStartOffset[nBank])
			return FALSE;

		uint32 bankRelativeOffset = sample.nOffset - gSampleBankDiscStartOffset[nBank];
		if (bankRelativeOffset + sample.nSize > gSampleBankSize[nBank])
			return FALSE;

		sampleData = (const int16*)(gSampleBankMemoryStartAddress[nBank] + bankRelativeOffset);
	}

	sampleCount = sample.nSize / sizeof(int16);
	return sampleCount != 0;
}

static uint32 BytesToSampleIndex(uint32 bytes)
{
	return bytes / sizeof(int16);
}

static int32 BytesToLoopEndSampleIndex(int32 bytes)
{
	if (bytes < 0)
		return -1;
	return bytes / int32(sizeof(int16));
}

static bool RenderNextSampleChannelFrame(tGcSampleChannel &channel, int16 &outL, int16 &outR)
{
	if (!channel.active || channel.paused || channel.sampleData == NULL || channel.sampleCount == 0 || channel.stepQ16 == 0)
		return false;

	uint32 sampleIndex = channel.cursorQ16 >> 16;
	if (sampleIndex >= channel.sampleCount) {
		if (channel.loopCount == 0 || channel.loopCount > 1) {
			uint32 loopStart = BytesToSampleIndex(channel.loopStartBytes);
			if (loopStart >= channel.sampleCount)
				loopStart = 0;
			if (channel.loopCount > 1)
				channel.loopCount--;
			channel.cursorQ16 = loopStart << 16;
			sampleIndex = loopStart;
		} else {
			channel.active = FALSE;
			return false;
		}
	}

	int16 mono = channel.sampleData[sampleIndex];
	outL = mono;
	outR = mono;

	channel.cursorQ16 += channel.stepQ16;

	if (channel.loopCount == 0 || channel.loopCount > 1) {
		int32 loopEnd = BytesToLoopEndSampleIndex(channel.loopEndBytes);
		if (loopEnd >= 0 && uint32(loopEnd) < channel.sampleCount) {
			uint32 currentIndex = channel.cursorQ16 >> 16;
			if (currentIndex >= uint32(loopEnd)) {
				uint32 loopStart = BytesToSampleIndex(channel.loopStartBytes);
				if (loopStart >= channel.sampleCount)
					loopStart = 0;
				if (channel.loopCount > 1)
					channel.loopCount--;
				channel.cursorQ16 = loopStart << 16;
			}
		}
	}

	return true;
}

static uint32 MixSampleChannelIntoBuffer(tGcSampleChannel &channel, int32 *mixBuffer, uint32 outputFrames)
{
	if (!channel.active || channel.sampleData == NULL || channel.sampleCount == 0)
		return 0;

	uint32 framesRendered = 0;
	uint32 volume = channel.volume < MAX_VOLUME ? channel.volume : MAX_VOLUME;
	uint32 leftGain = volume;
	uint32 rightGain = volume;

	if (channel.pan <= 63)
		rightGain = (volume * channel.pan) / 63;
	else
		leftGain = (volume * (127 - channel.pan)) / 64;

	while (framesRendered < outputFrames) {
		int16 left;
		int16 right;
		if (!RenderNextSampleChannelFrame(channel, left, right))
			break;

		uint32 outIndex = framesRendered * 2;
		mixBuffer[outIndex + 0] += (int32(left) * int32(leftGain)) / MAX_VOLUME;
		mixBuffer[outIndex + 1] += (int32(right) * int32(rightGain)) / MAX_VOLUME;
		framesRendered++;
	}

	return framesRendered;
}

static const char *ResolveDirectPs2RadioPath(tTrack track)
{
	static bool8 s_scanned[STREAMED_SOUND_RADIO_WAVE + 1];
	static const char *s_cachedPath[STREAMED_SOUND_RADIO_WAVE + 1];

	static const char *const s_wildCandidates[] = {
		"AUDIO\\MUSIC\\wild.vb",
	};
	static const char *const s_flashCandidates[] = {
		"AUDIO\\MUSIC\\Flash.vb",
	};
	static const char *const s_kchatCandidates[] = {
		"AUDIO\\MUSIC\\KCHAT.VB",
		"AUDIO\\MUSIC\\Kchat.vb",
		"AUDIO\\MUSIC\\GTA PS2 Radio VB to WAV\\work\\original\\Kchat.vb",
	};
	static const char *const s_feverCandidates[] = {
		"AUDIO\\MUSIC\\fever.vb",
	};
	static const char *const s_vrockCandidates[] = {
		"AUDIO\\MUSIC\\vrock.vb",
	};
	static const char *const s_vcprCandidates[] = {
		"AUDIO\\MUSIC\\VCPR.VB",
		"AUDIO\\MUSIC\\vcpr.vb",
		"AUDIO\\MUSIC\\GTA PS2 Radio VB to WAV\\work\\original\\vcpr.vb",
	};
	static const char *const s_espantCandidates[] = {
		"AUDIO\\MUSIC\\espant.vb",
	};
	static const char *const s_emotionCandidates[] = {
		"AUDIO\\MUSIC\\emotion.vb",
	};
	static const char *const s_waveCandidates[] = {
		"AUDIO\\MUSIC\\wave.vb",
	};

	if (!IsDirectPs2RadioWhitelistTrack(track))
		return NULL;

	if (s_scanned[track])
		return s_cachedPath[track];

	s_scanned[track] = TRUE;

	const char *const *candidates = NULL;
	uint32 candidateCount = 0;
	switch (track) {
	case STREAMED_SOUND_RADIO_WILD:
		candidates = s_wildCandidates;
		candidateCount = ARRAY_SIZE(s_wildCandidates);
		break;
	case STREAMED_SOUND_RADIO_FLASH:
		candidates = s_flashCandidates;
		candidateCount = ARRAY_SIZE(s_flashCandidates);
		break;
	case STREAMED_SOUND_RADIO_KCHAT:
		candidates = s_kchatCandidates;
		candidateCount = ARRAY_SIZE(s_kchatCandidates);
		break;
	case STREAMED_SOUND_RADIO_FEVER:
		candidates = s_feverCandidates;
		candidateCount = ARRAY_SIZE(s_feverCandidates);
		break;
	case STREAMED_SOUND_RADIO_VROCK:
		candidates = s_vrockCandidates;
		candidateCount = ARRAY_SIZE(s_vrockCandidates);
		break;
	case STREAMED_SOUND_RADIO_VCPR:
		candidates = s_vcprCandidates;
		candidateCount = ARRAY_SIZE(s_vcprCandidates);
		break;
	case STREAMED_SOUND_RADIO_ESPANTOSO:
		candidates = s_espantCandidates;
		candidateCount = ARRAY_SIZE(s_espantCandidates);
		break;
	case STREAMED_SOUND_RADIO_EMOTION:
		candidates = s_emotionCandidates;
		candidateCount = ARRAY_SIZE(s_emotionCandidates);
		break;
	case STREAMED_SOUND_RADIO_WAVE:
		candidates = s_waveCandidates;
		candidateCount = ARRAY_SIZE(s_waveCandidates);
		break;
	default:
		break;
	}

	for (uint32 i = 0; i < candidateCount; i++) {
		if (!CanOpenTrackPath(candidates[i]))
			continue;

		s_cachedPath[track] = candidates[i];
			GC_AUDIO_LOG("[GC-AUDIO] whitelist radio %s -> %s\n", GetRadioTrackLabel(track), candidates[i]);
		break;
	}

	if (s_cachedPath[track] == NULL)
		GC_AUDIO_LOG("[GC-AUDIO] whitelist radio %s missing direct PS2 VB candidates\n", GetRadioTrackLabel(track));

	return s_cachedPath[track];
}

static inline int8 SignNibble(uint8 value)
{
	return (value & 0x8) ? int8(value | 0xF0) : int8(value & 0x0F);
}

static inline int16 ClampToInt16(int32 value)
{
	if (value < -32768)
		return -32768;
	if (value > 32767)
		return 32767;
	return int16(value);
}

static inline uint32 GcSampleCountToByteCount(uint32 sampleCount)
{
	uint32 frames = sampleCount / GC_ADPCM_SAMPLES_PER_FRAME;
	uint32 extraSamples = sampleCount % GC_ADPCM_SAMPLES_PER_FRAME;
	uint32 extraNibbles = extraSamples == 0 ? 0 : extraSamples + 2;
	return frames * GC_ADPCM_FRAME_SIZE + ((extraNibbles + 1) / 2);
}

static inline uint32 GcByteCountToSampleCount(uint32 byteCount)
{
	uint32 nibbleCount = byteCount * 2;
	uint32 frames = nibbleCount / 16;
	uint32 extraNibbles = nibbleCount % 16;
	uint32 extraSamples = extraNibbles < 2 ? 0 : extraNibbles - 2;
	return frames * GC_ADPCM_SAMPLES_PER_FRAME + extraSamples;
}

static inline uint32 MsToSamples(uint32 sampleRate, uint32 milliseconds)
{
	return uint32((uint64(milliseconds) * sampleRate) / 1000ULL);
}

static inline uint32 SamplesToMs(uint32 sampleRate, uint64 samples)
{
	return uint32((samples * 1000ULL) / sampleRate);
}

static bool8 EndsWithIgnoreCase(const char *path, const char *suffix)
{
	if (path == NULL || suffix == NULL)
		return FALSE;

	size_t pathLen = strlen(path);
	size_t suffixLen = strlen(suffix);
	if (pathLen < suffixLen)
		return FALSE;

	const char *lhs = path + pathLen - suffixLen;
	for (size_t i = 0; i < suffixLen; i++) {
		char a = lhs[i];
		char b = suffix[i];
		if (a >= 'A' && a <= 'Z')
			a = char(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = char(b - 'A' + 'a');
		if (a != b)
			return FALSE;
	}
	return TRUE;
}

static uint16 ReadBE16(FILE *file)
{
	uint8 bytes[2];
	if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
		return 0;
	return uint16((bytes[0] << 8) | bytes[1]);
}

static uint32 ReadBE32(FILE *file)
{
	uint8 bytes[4];
	if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
		return 0;
	return (uint32(bytes[0]) << 24) | (uint32(bytes[1]) << 16) | (uint32(bytes[2]) << 8) | uint32(bytes[3]);
}

static bool8 ProbeIdspLengthMs(const char *path, uint32 &lengthMs)
{
	lengthMs = 0;

	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return FALSE;

	char magic[4];
	if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) || memcmp(magic, "IDSP", 4) != 0) {
		fclose(file);
		return FALSE;
	}

	ReadBE32(file); // reserved
	uint32 channelCount = ReadBE32(file);
	uint32 sampleRate = ReadBE32(file);
	uint32 sampleCount = ReadBE32(file);
	fclose(file);

	if (channelCount == 0 || channelCount > 2 || sampleRate == 0 || sampleCount == 0)
		return FALSE;

	lengthMs = SamplesToMs(sampleRate, sampleCount);
	return lengthMs != 0;
}

static bool8 ProbeVbLengthMs(const char *path, uint32 sampleRate, uint32 &lengthMs)
{
	lengthMs = 0;

	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return FALSE;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return FALSE;
	}

	long fileSize = ftell(file);
	fclose(file);

	if (fileSize <= 0 || sampleRate == 0)
		return FALSE;

	uint32 blockCount = uint32(fileSize) / (2 * VB_BLOCK_SIZE);
	if (blockCount == 0)
		return FALSE;

	lengthMs = SamplesToMs(sampleRate, uint64(blockCount) * NUM_VAG_SAMPLES_IN_BLOCK);
	return lengthMs != 0;
}

static void DecodeGcAdpcmFrame(const uint8 *frame, const int16 *coefs, tAdpcmHist &hist, int16 *outbuf, uint32 sampleCount)
{
	uint8 predictorScale = frame[0];
	int scale = (1 << (predictorScale & 0x0F)) * 2048;
	int predictor = predictorScale >> 4;
	int16 coef1 = coefs[predictor * 2 + 0];
	int16 coef2 = coefs[predictor * 2 + 1];

	for (uint32 i = 0; i < sampleCount; i++) {
		uint8 byte = frame[1 + (i >> 1)];
		int adpcmSample = (i & 1) == 0 ? SignNibble(byte >> 4) : SignNibble(byte & 0x0F);
		int distance = scale * adpcmSample;
		int predicted = coef1 * hist.hist1 + coef2 * hist.hist2;
		int corrected = predicted + distance;
		int scaled = (corrected + 1024) >> 11;
		int16 decoded = ClampToInt16(scaled);

		hist.hist2 = hist.hist1;
		hist.hist1 = decoded;
		outbuf[i] = decoded;
	}
}

class CIdspStreamDecoder : public CGcStreamDecoder
{
	FILE *m_pFile;
	bool m_bOpened;

	uint32 m_ChannelCount;
	uint32 m_SampleRate;
	uint32 m_SampleCount;
	uint32 m_InterleaveSize;
	uint32 m_HeaderSize;
	uint32 m_AudioDataOffset;
	uint32 m_AudioDataSize;
	uint32 m_BlockCount;
	uint32 m_BlockSamples;

	int16 m_Coefs[2][16];
	tAdpcmHist m_InitialHist[2];
	tAdpcmHist m_CurrentHist[2];
	uint8 *m_pBlockData[2];

	uint32 m_CurrentSample;
	uint32 m_CurrentBlock;
	uint32 m_CurrentFrameInBlock;
	bool m_bBlockRead;

	int16 m_PendingSamples[GC_ADPCM_SAMPLES_PER_FRAME * 2];
	uint32 m_PendingCount;
	uint32 m_PendingPos;

	bool ReadBlock(uint32 blockIndex)
	{
		if (m_ChannelCount == 0 || blockIndex >= m_BlockCount)
			return false;

		for (uint32 ch = 0; ch < m_ChannelCount && ch < 2; ch++) {
			uint32 offset = m_AudioDataOffset + blockIndex * m_InterleaveSize * m_ChannelCount + ch * m_InterleaveSize;
			if (fseek(m_pFile, long(offset), SEEK_SET) != 0)
				return false;
			if (fread(m_pBlockData[ch], 1, m_InterleaveSize, m_pFile) != m_InterleaveSize)
				return false;
		}

		m_bBlockRead = true;
		return true;
	}

	void ResetPending()
	{
		m_PendingCount = 0;
		m_PendingPos = 0;
	}

public:
	CIdspStreamDecoder(const char *path)
		: m_pFile(NULL),
		  m_bOpened(false),
		  m_ChannelCount(0),
		  m_SampleRate(0),
		  m_SampleCount(0),
		  m_InterleaveSize(0),
		  m_HeaderSize(0),
		  m_AudioDataOffset(0),
		  m_AudioDataSize(0),
		  m_BlockCount(0),
		  m_BlockSamples(0),
		  m_CurrentSample(0),
		  m_CurrentBlock(0),
		  m_CurrentFrameInBlock(0),
		  m_bBlockRead(false),
		  m_PendingCount(0),
		  m_PendingPos(0)
	{
		m_pBlockData[0] = NULL;
		m_pBlockData[1] = NULL;
		memset(m_Coefs, 0, sizeof(m_Coefs));
		memset(m_InitialHist, 0, sizeof(m_InitialHist));
		memset(m_CurrentHist, 0, sizeof(m_CurrentHist));
		memset(m_PendingSamples, 0, sizeof(m_PendingSamples));

		m_pFile = fopen(path, "rb");
		if (m_pFile == NULL)
			return;

		char magic[4];
		if (fread(magic, 1, sizeof(magic), m_pFile) != sizeof(magic) || memcmp(magic, "IDSP", 4) != 0)
			return;

		ReadBE32(m_pFile); // reserved
		m_ChannelCount = ReadBE32(m_pFile);
		m_SampleRate = ReadBE32(m_pFile);
		m_SampleCount = ReadBE32(m_pFile);
		ReadBE32(m_pFile); // loop start
		ReadBE32(m_pFile); // loop end
		m_InterleaveSize = ReadBE32(m_pFile);
		ReadBE32(m_pFile); // stream info size
		ReadBE32(m_pFile); // channel info size
		m_HeaderSize = ReadBE32(m_pFile);
		m_AudioDataSize = ReadBE32(m_pFile);

		if (m_ChannelCount == 0 || m_ChannelCount > 2 || m_SampleRate == 0 || m_SampleCount == 0 || m_InterleaveSize == 0)
			return;

		m_AudioDataOffset = m_HeaderSize;
		m_BlockCount = m_AudioDataSize / (m_InterleaveSize * m_ChannelCount);
		m_BlockSamples = GcByteCountToSampleCount(m_InterleaveSize);
		if (m_BlockCount == 0 || m_BlockSamples == 0)
			return;

		for (uint32 ch = 0; ch < m_ChannelCount; ch++) {
			if (fseek(m_pFile, long(0x40 + ch * 0x60), SEEK_SET) != 0)
				return;
			ReadBE32(m_pFile); // sample count
			ReadBE32(m_pFile); // nibble count
			ReadBE32(m_pFile); // sample rate
			ReadBE16(m_pFile); // looping
			ReadBE16(m_pFile); // padding
			ReadBE32(m_pFile); // loop start address
			ReadBE32(m_pFile); // loop end address
			ReadBE32(m_pFile); // current address
			for (uint32 i = 0; i < 16; i++)
				m_Coefs[ch][i] = int16(ReadBE16(m_pFile));
			ReadBE16(m_pFile); // gain
			ReadBE16(m_pFile); // start pred/scale
			m_InitialHist[ch].hist1 = int16(ReadBE16(m_pFile));
			m_InitialHist[ch].hist2 = int16(ReadBE16(m_pFile));
			m_CurrentHist[ch] = m_InitialHist[ch];
			ReadBE16(m_pFile); // loop pred/scale
			ReadBE16(m_pFile); // loop hist1
			ReadBE16(m_pFile); // loop hist2
		}

		for (uint32 ch = 0; ch < m_ChannelCount; ch++)
			m_pBlockData[ch] = new uint8[m_InterleaveSize];
		m_bOpened = true;
		SeekMS(0);
	}

	~CIdspStreamDecoder()
	{
		if (m_pFile)
			fclose(m_pFile);
		for (uint32 ch = 0; ch < 2; ch++)
			delete[] m_pBlockData[ch];
	}

	bool IsOpened() const
	{
		return m_bOpened;
	}

	uint32 GetLengthMS() const
	{
		return m_SampleRate == 0 ? 0 : SamplesToMs(m_SampleRate, m_SampleCount);
	}

	uint32 GetSampleRate() const
	{
		return m_SampleRate;
	}

	void SeekMS(uint32 milliseconds)
	{
		if (!IsOpened())
			return;

		uint32 targetSample = MsToSamples(m_SampleRate, milliseconds);
		if (targetSample >= m_SampleCount)
			targetSample = 0;

		const uint32 warmupBlocks = 2;
		uint32 targetBlock = targetSample / m_BlockSamples;
		uint32 decodeStartBlock = targetBlock > warmupBlocks ? targetBlock - warmupBlocks : 0;

		m_CurrentBlock = decodeStartBlock;
		m_CurrentFrameInBlock = 0;
		m_CurrentSample = decodeStartBlock * m_BlockSamples;
		m_bBlockRead = false;
		ResetPending();

		for (uint32 ch = 0; ch < m_ChannelCount && ch < 2; ch++)
			m_CurrentHist[ch] = m_InitialHist[ch];

		uint32 discardSamples = targetSample - m_CurrentSample;
		while (discardSamples != 0) {
			int16 scratch[512 * 2];
			uint32 decoded = DecodeFrames(scratch, Min(discardSamples, uint32(512)));
			if (decoded == 0)
				break;
			discardSamples -= decoded;
		}
	}

	uint32 DecodeFrames(int16 *buffer, uint32 maxFrames)
	{
		if (!IsOpened() || buffer == NULL || maxFrames == 0)
			return 0;

		uint32 framesWritten = 0;
		uint32 framesPerBlock = m_InterleaveSize / GC_ADPCM_FRAME_SIZE;
		int16 temp[2][GC_ADPCM_SAMPLES_PER_FRAME];

		while (framesWritten < maxFrames && m_CurrentSample < m_SampleCount) {
			if (m_PendingPos < m_PendingCount) {
				uint32 available = m_PendingCount - m_PendingPos;
				uint32 toCopy = Min(available, maxFrames - framesWritten);
				memcpy(buffer + framesWritten * 2, m_PendingSamples + m_PendingPos * 2, sizeof(int16) * toCopy * 2);
				m_PendingPos += toCopy;
				framesWritten += toCopy;
				m_CurrentSample += toCopy;
				if (m_PendingPos >= m_PendingCount)
					ResetPending();
				continue;
			}

			if (!m_bBlockRead && !ReadBlock(m_CurrentBlock))
				break;

			if (m_CurrentFrameInBlock >= framesPerBlock) {
				m_CurrentBlock++;
				m_CurrentFrameInBlock = 0;
				m_bBlockRead = false;
				continue;
			}

			uint32 samplesThisFrame = Min(GC_ADPCM_SAMPLES_PER_FRAME, m_SampleCount - m_CurrentSample);
			for (uint32 ch = 0; ch < m_ChannelCount && ch < 2; ch++) {
				const uint8 *framePtr = m_pBlockData[ch] + m_CurrentFrameInBlock * GC_ADPCM_FRAME_SIZE;
				DecodeGcAdpcmFrame(framePtr, m_Coefs[ch], m_CurrentHist[ch], temp[ch], samplesThisFrame);
			}
			for (uint32 i = 0; i < samplesThisFrame; i++) {
				m_PendingSamples[i * 2 + 0] = temp[0][i];
				m_PendingSamples[i * 2 + 1] = (m_ChannelCount > 1) ? temp[1][i] : temp[0][i];
			}
			m_PendingCount = samplesThisFrame;
			m_PendingPos = 0;
			m_CurrentFrameInBlock++;
		}

		return framesWritten;
	}
};

class CVbStreamDecoder : public CGcStreamDecoder
{
	FILE *m_pFile;
	bool m_bOpened;

	uint32 m_SampleRate;
	uint32 m_ChannelCount;
	uint32 m_BlockCount;
	uint32 m_CurrentBlock;
	uint32 m_CurrentLine;
	uint32 m_CurrentSample;
	bool m_bBlockRead;

	uint8 *m_pBlockData[2];
	CVagDecoder m_Decoders[2];

	int16 m_PendingSamples[VAG_SAMPLES_IN_LINE * 2];
	uint32 m_PendingCount;
	uint32 m_PendingPos;

	bool ReadBlock(uint32 blockIndex)
	{
		if (blockIndex >= m_BlockCount)
			return false;
		if (fseek(m_pFile, long(blockIndex * m_ChannelCount * VB_BLOCK_SIZE), SEEK_SET) != 0)
			return false;
		for (uint32 ch = 0; ch < m_ChannelCount && ch < 2; ch++) {
			if (fread(m_pBlockData[ch], 1, VB_BLOCK_SIZE, m_pFile) != VB_BLOCK_SIZE)
				return false;
		}
		m_bBlockRead = true;
		return true;
	}

	void ResetPending()
	{
		m_PendingCount = 0;
		m_PendingPos = 0;
	}

public:
	CVbStreamDecoder(const char *path, uint32 sampleRate)
		: m_pFile(NULL),
		  m_bOpened(false),
		  m_SampleRate(sampleRate),
		  m_ChannelCount(2),
		  m_BlockCount(0),
		  m_CurrentBlock(0),
		  m_CurrentLine(0),
		  m_CurrentSample(0),
		  m_bBlockRead(false),
		  m_PendingCount(0),
		  m_PendingPos(0)
	{
		m_pBlockData[0] = NULL;
		m_pBlockData[1] = NULL;
		memset(m_PendingSamples, 0, sizeof(m_PendingSamples));

		m_pFile = fopen(path, "rb");
		if (m_pFile == NULL)
			return;

		fseek(m_pFile, 0, SEEK_END);
		long fileSize = ftell(m_pFile);
		fseek(m_pFile, 0, SEEK_SET);
		if (fileSize <= 0)
			return;

		m_BlockCount = uint32(fileSize) / (m_ChannelCount * VB_BLOCK_SIZE);
		if (m_BlockCount == 0)
			return;

		for (uint32 ch = 0; ch < m_ChannelCount; ch++)
			m_pBlockData[ch] = new uint8[VB_BLOCK_SIZE];

		m_bOpened = true;
		SeekMS(0);
	}

	~CVbStreamDecoder()
	{
		if (m_pFile)
			fclose(m_pFile);
		delete[] m_pBlockData[0];
		delete[] m_pBlockData[1];
	}

	bool IsOpened() const
	{
		return m_bOpened;
	}

	uint32 GetLengthMS() const
	{
		return m_SampleRate == 0 ? 0 : SamplesToMs(m_SampleRate, uint64(m_BlockCount) * NUM_VAG_SAMPLES_IN_BLOCK);
	}

	uint32 GetSampleRate() const
	{
		return m_SampleRate;
	}

	void SeekMS(uint32 milliseconds)
	{
		if (!IsOpened())
			return;

		uint32 sample = MsToSamples(m_SampleRate, milliseconds);
		uint32 block = sample / NUM_VAG_SAMPLES_IN_BLOCK;
		if (block >= m_BlockCount) {
			block = 0;
			sample = 0;
		}

		uint32 remaining = sample - block * NUM_VAG_SAMPLES_IN_BLOCK;
		uint32 line = remaining / VAG_SAMPLES_IN_LINE;

		m_CurrentBlock = block;
		m_CurrentLine = line;
		m_CurrentSample = block * NUM_VAG_SAMPLES_IN_BLOCK + line * VAG_SAMPLES_IN_LINE;
		m_bBlockRead = false;
		ResetPending();
		m_Decoders[0].ResetState();
		m_Decoders[1].ResetState();
	}

	uint32 DecodeFrames(int16 *buffer, uint32 maxFrames)
	{
		if (!IsOpened() || buffer == NULL || maxFrames == 0)
			return 0;

		uint32 framesWritten = 0;
		int16 temp[2][VAG_SAMPLES_IN_LINE];

		while (framesWritten < maxFrames && m_CurrentBlock < m_BlockCount) {
			if (m_PendingPos < m_PendingCount) {
				uint32 available = m_PendingCount - m_PendingPos;
				uint32 toCopy = Min(available, maxFrames - framesWritten);
				memcpy(buffer + framesWritten * 2, m_PendingSamples + m_PendingPos * 2, sizeof(int16) * toCopy * 2);
				m_PendingPos += toCopy;
				framesWritten += toCopy;
				m_CurrentSample += toCopy;
				if (m_PendingPos >= m_PendingCount)
					ResetPending();
				continue;
			}

			if (!m_bBlockRead && !ReadBlock(m_CurrentBlock))
				break;

			if (m_CurrentLine >= NUM_VAG_LINES_IN_BLOCK) {
				m_CurrentBlock++;
				m_CurrentLine = 0;
				m_bBlockRead = false;
				continue;
			}

			for (uint32 ch = 0; ch < m_ChannelCount; ch++)
				m_Decoders[ch].DecodeLine(m_pBlockData[ch] + m_CurrentLine * VAG_LINE_SIZE, temp[ch]);

			for (uint32 i = 0; i < VAG_SAMPLES_IN_LINE; i++) {
				m_PendingSamples[i * 2 + 0] = temp[0][i];
				m_PendingSamples[i * 2 + 1] = temp[1][i];
			}
			m_PendingCount = VAG_SAMPLES_IN_LINE;
			m_PendingPos = 0;
			m_CurrentLine++;
		}

		return framesWritten;
	}
};

struct tGcStreamSlot
{
	CGcStreamDecoder *decoder;
	bool8 preloaded;
	bool8 active;
	bool8 paused;
	bool8 loop;
	bool8 effectFlag;
	uint8 requestedVolume;
	uint8 pan;
	uint32 mixVolume;
	uint32 lengthMs;
	uint32 startPosMs;
	uint64 playedOutputFrames;
	uint64 resampleStep;
	uint64 resampleFrac;
	bool8 sourceEnded;
	uint32 sourceFramesValid;
	uint32 sourceFramePos;
	int16 *sourceBuffer;

	void ResetRuntime()
	{
		active = FALSE;
		paused = FALSE;
		sourceEnded = FALSE;
		sourceFramesValid = 0;
		sourceFramePos = 0;
		resampleStep = 0;
		resampleFrac = 0;
		playedOutputFrames = 0;
		startPosMs = 0;
	}
};

static tGcStreamSlot gStreamSlots[MAX_STREAMS];
static uint32 gStreamLength[TOTAL_STREAMED_SOUNDS];
static bool8 gStreamLoopedFlag[MAX_STREAMS];
static volatile uint32 gDmaBufferStates[GC_DMA_BUFFER_COUNT];
static uint32 gDmaBufferSlotFrames[GC_DMA_BUFFER_COUNT][MAX_STREAMS];
static volatile int32 gCurrentDmaBuffer = -1;
static bool8 gDmaActive = FALSE;
static int16 gDmaBuffers[GC_DMA_BUFFER_COUNT][GC_DMA_BUFFER_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int16 gSilenceBuffer[GC_DMA_BUFFER_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int32 gMixBuffer[GC_DMA_BUFFER_FRAMES * 2];

static const char *GetTrackPath(tTrack track)
{
	if (!IsSupportedGcRadioTrack(track))
		return NULL;

#ifndef GTA_PS2
	if (track == STREAMED_SOUND_RADIO_MP3_PLAYER)
		track = STREAMED_SOUND_RADIO_WILD;
#endif

	if (IsDirectPs2RadioWhitelistTrack(track)) {
		const char *directPath = ResolveDirectPs2RadioPath(track);
		if (directPath != NULL)
			return directPath;
	}

#ifdef PS2_AUDIO_PATHS
	if (track < CountOfPS2Table())
		return PS2StreamedNameTable[track];
#endif
	return NULL;
}

static bool8 ProbeTrackLength(tTrack track, uint32 &lengthMs)
{
	if (!IsSupportedGcRadioTrack(track)) {
		lengthMs = 0;
		return FALSE;
	}

	const char *path = GetTrackPath(track);
	if (path == NULL)
		return FALSE;

	if (EndsWithIgnoreCase(path, ".idsp"))
		return ProbeIdspLengthMs(path, lengthMs);
	if (EndsWithIgnoreCase(path, ".vb"))
		return ProbeVbLengthMs(path, IsThisTrackAt16KHz(track) ? 16000 : 32000, lengthMs);

	lengthMs = 0;
	return FALSE;
}

static CGcStreamDecoder *OpenStreamDecoder(const char *path, uint32 overrideSampleRate)
{
	if (path == NULL)
		return NULL;

	CGcStreamDecoder *decoder = NULL;
	if (EndsWithIgnoreCase(path, ".idsp"))
		decoder = new CIdspStreamDecoder(path);
	else if (EndsWithIgnoreCase(path, ".vb"))
		decoder = new CVbStreamDecoder(path, overrideSampleRate);

	if (decoder && !decoder->IsOpened()) {
		delete decoder;
		decoder = NULL;
	}
	return decoder;
}

static CGcStreamDecoder *OpenTrackDecoder(tTrack track)
{
	if (!IsSupportedGcRadioTrack(track))
		return NULL;

	const char *path = GetTrackPath(track);
	if (path == NULL) {
		if (IsDirectPs2RadioWhitelistTrack(track))
			GC_AUDIO_LOG("[GC-AUDIO] whitelist radio %s has no playable direct path\n", GetRadioTrackLabel(track));
		return NULL;
	}

	CGcStreamDecoder *decoder = OpenStreamDecoder(path, IsThisTrackAt16KHz(track) ? 16000 : 32000);
	if (IsDirectPs2RadioWhitelistTrack(track)) {
		if (decoder)
			GC_AUDIO_LOG("[GC-AUDIO] streaming radio %s from %s\n", GetRadioTrackLabel(track), path);
		else
			GC_AUDIO_LOG("[GC-AUDIO] failed to open radio %s from %s\n", GetRadioTrackLabel(track), path);
	}
	return decoder;
}

static bool8 AnyActiveUnpausedStreams()
{
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		if (gStreamSlots[i].active && !gStreamSlots[i].paused && gStreamSlots[i].decoder)
			return TRUE;
	}
	return FALSE;
}

static bool8 AnyActiveSampleChannels()
{
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		if (gSampleChannels[i].active)
			return TRUE;
	}
	return FALSE;
}

static void CloseStreamSlot(uint32 index)
{
	tGcStreamSlot &slot = gStreamSlots[index];
	delete slot.decoder;
	slot.decoder = NULL;
	slot.preloaded = FALSE;
	slot.loop = FALSE;
	slot.effectFlag = FALSE;
	slot.requestedVolume = MAX_VOLUME;
	slot.pan = 63;
	slot.mixVolume = 0;
	slot.lengthMs = 0;
	slot.ResetRuntime();
}

static void ResetSlotForPlayback(tGcStreamSlot &slot, uint32 startPosMs)
{
	slot.startPosMs = startPosMs;
	slot.playedOutputFrames = 0;
	slot.sourceEnded = FALSE;
	slot.sourceFramesValid = 0;
	slot.sourceFramePos = 0;
	slot.resampleFrac = 0;
	slot.resampleStep = slot.decoder ? ((uint64)slot.decoder->GetSampleRate() << 32) / GC_OUTPUT_RATE : 0;
	if (slot.decoder)
		slot.decoder->SeekMS(startPosMs);
}

static void CompactSourceBuffer(tGcStreamSlot &slot)
{
	if (slot.sourceFramePos == 0)
		return;
	if (slot.sourceFramePos >= slot.sourceFramesValid) {
		slot.sourceFramesValid = 0;
		slot.sourceFramePos = 0;
		return;
	}

	uint32 remaining = slot.sourceFramesValid - slot.sourceFramePos;
	memmove(slot.sourceBuffer, slot.sourceBuffer + slot.sourceFramePos * 2, sizeof(int16) * remaining * 2);
	slot.sourceFramesValid = remaining;
	slot.sourceFramePos = 0;
}

static bool RefillSourceBuffer(tGcStreamSlot &slot, uint32 minimumFrames)
{
	if (!slot.decoder)
		return false;

	while (slot.sourceFramesValid - slot.sourceFramePos < minimumFrames) {
		if (slot.sourceFramePos != 0)
			CompactSourceBuffer(slot);

		if (slot.sourceFramesValid >= GC_SOURCE_BUFFER_FRAMES)
			break;

		uint32 capacity = GC_SOURCE_BUFFER_FRAMES - slot.sourceFramesValid;
		uint32 decoded = slot.decoder->DecodeFrames(slot.sourceBuffer + slot.sourceFramesValid * 2, capacity);
		if (decoded == 0) {
			if (slot.loop && slot.lengthMs > 0) {
				slot.decoder->SeekMS(0);
				slot.sourceEnded = FALSE;
				slot.sourceFramesValid = 0;
				slot.sourceFramePos = 0;
				slot.resampleFrac = 0;
				continue;
			}
			slot.sourceEnded = TRUE;
			break;
		}
		slot.sourceFramesValid += decoded;
	}

	return (slot.sourceFramesValid - slot.sourceFramePos) >= minimumFrames || (!slot.sourceEnded && (slot.sourceFramesValid - slot.sourceFramePos) > 0);
}

static bool RenderNextOutputFrame(tGcStreamSlot &slot, int16 &outL, int16 &outR)
{
	if (!slot.decoder)
		return false;

	if (slot.sourceFramesValid - slot.sourceFramePos == 0) {
		if (!RefillSourceBuffer(slot, 2) && slot.sourceFramesValid - slot.sourceFramePos == 0)
			return false;
	}

	uint32 available = slot.sourceFramesValid - slot.sourceFramePos;
	if (available == 0)
		return false;

	if (available == 1 && !slot.sourceEnded)
		RefillSourceBuffer(slot, 2);

	available = slot.sourceFramesValid - slot.sourceFramePos;
	int16 aL = slot.sourceBuffer[slot.sourceFramePos * 2 + 0];
	int16 aR = slot.sourceBuffer[slot.sourceFramePos * 2 + 1];
	int16 bL = aL;
	int16 bR = aR;
	if (available > 1) {
		bL = slot.sourceBuffer[(slot.sourceFramePos + 1) * 2 + 0];
		bR = slot.sourceBuffer[(slot.sourceFramePos + 1) * 2 + 1];
	}

	uint64 frac = slot.resampleFrac;
	int64 diffL = int64(bL) - int64(aL);
	int64 diffR = int64(bR) - int64(aR);
	outL = int16(int64(aL) + ((diffL * int64(frac)) >> 32));
	outR = int16(int64(aR) + ((diffR * int64(frac)) >> 32));

	slot.resampleFrac += slot.resampleStep;
	uint32 advance = uint32(slot.resampleFrac >> 32);
	slot.resampleFrac &= 0xFFFFFFFFULL;
	slot.sourceFramePos += advance;

	if (slot.sourceFramePos > (GC_SOURCE_BUFFER_FRAMES / 2))
		CompactSourceBuffer(slot);

	return true;
}

static uint32 MixSlotIntoBuffer(tGcStreamSlot &slot, int32 *mixBuffer, uint32 outputFrames)
{
	if (!slot.active || slot.paused || slot.decoder == NULL)
		return 0;

	uint32 framesRendered = 0;
	uint32 volume = Min(slot.mixVolume, uint32(MAX_VOLUME));
	uint32 leftGain = volume;
	uint32 rightGain = volume;

	if (slot.pan <= 63)
		rightGain = (volume * slot.pan) / 63;
	else
		leftGain = (volume * (127 - slot.pan)) / 64;

	while (framesRendered < outputFrames) {
		int16 left;
		int16 right;
		if (!RenderNextOutputFrame(slot, left, right)) {
			slot.active = FALSE;
			break;
		}

		if (leftGain != 0 || rightGain != 0) {
			uint32 outIndex = framesRendered * 2;
			mixBuffer[outIndex + 0] += (int32(left) * int32(leftGain)) / MAX_VOLUME;
			mixBuffer[outIndex + 1] += (int32(right) * int32(rightGain)) / MAX_VOLUME;
		}
		framesRendered++;
	}

	return framesRendered;
}

static bool FillDmaBuffer(uint32 bufferIndex)
{
	memset(gMixBuffer, 0, sizeof(gMixBuffer));
	memset(gDmaBufferSlotFrames[bufferIndex], 0, sizeof(gDmaBufferSlotFrames[bufferIndex]));

	bool hasAudio = false;
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		uint32 frames = MixSampleChannelIntoBuffer(gSampleChannels[i], gMixBuffer, GC_DMA_BUFFER_FRAMES);
		if (frames != 0)
			hasAudio = true;
	}

	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		uint32 frames = MixSlotIntoBuffer(gStreamSlots[i], gMixBuffer, GC_DMA_BUFFER_FRAMES);
		gDmaBufferSlotFrames[bufferIndex][i] = frames;
		if (frames != 0)
			hasAudio = true;
	}

	if (!hasAudio)
		return false;

	for (uint32 i = 0; i < GC_DMA_BUFFER_FRAMES * 2; i++)
		gDmaBuffers[bufferIndex][i] = ClampToInt16(gMixBuffer[i]);

	DCFlushRange(gDmaBuffers[bufferIndex], GC_DMA_BUFFER_SIZE);
	return true;
}

static void TryFillReadyBuffers()
{
	if (!AnyActiveUnpausedStreams() && !AnyActiveSampleChannels())
		return;

	for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
		if (gDmaBufferStates[i] != GC_BUFFER_FREE)
			continue;
		if (!FillDmaBuffer(i))
			break;
		gDmaBufferStates[i] = GC_BUFFER_READY;
	}
}

static void AudioDmaCallback()
{
	u32 level;
	_CPU_ISR_Disable(level);

	if (gCurrentDmaBuffer >= 0 && gCurrentDmaBuffer < int32(GC_DMA_BUFFER_COUNT)) {
		for (uint32 i = 0; i < MAX_STREAMS; i++)
			gStreamSlots[i].playedOutputFrames += gDmaBufferSlotFrames[gCurrentDmaBuffer][i];
		gDmaBufferStates[gCurrentDmaBuffer] = GC_BUFFER_FREE;
	}

	int32 nextBuffer = -1;
	for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
		if (gDmaBufferStates[i] == GC_BUFFER_READY) {
			nextBuffer = i;
			break;
		}
	}

	gCurrentDmaBuffer = nextBuffer;
	if (nextBuffer >= 0) {
		gDmaBufferStates[nextBuffer] = GC_BUFFER_PLAYING;
		AUDIO_InitDMA((u32)gDmaBuffers[nextBuffer], GC_DMA_BUFFER_SIZE);
	} else {
		AUDIO_InitDMA((u32)gSilenceBuffer, GC_DMA_BUFFER_SIZE);
	}

	_CPU_ISR_Restore(level);
}

static void StartAudioDmaIfNeeded()
{
	if (gDmaActive)
		return;

	memset(gSilenceBuffer, 0, sizeof(gSilenceBuffer));
	DCFlushRange(gSilenceBuffer, sizeof(gSilenceBuffer));

	for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
		gDmaBufferStates[i] = GC_BUFFER_FREE;
		memset(gDmaBufferSlotFrames[i], 0, sizeof(gDmaBufferSlotFrames[i]));
	}

	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(AudioDmaCallback);
	AUDIO_InitDMA((u32)gSilenceBuffer, GC_DMA_BUFFER_SIZE);
	AUDIO_StartDMA();

	gCurrentDmaBuffer = -1;
	gDmaActive = TRUE;
}

}
#endif

cSampleManager::cSampleManager(void)
{
	;
}

cSampleManager::~cSampleManager(void)
{
}

#ifdef EXTERNAL_3D_SOUND
void cSampleManager::SetSpeakerConfig(int32 nConfig)
{
}

uint32 cSampleManager::GetMaximumSupportedChannels(void)
{
	return MAXCHANNELS;
}

uint32 cSampleManager::GetNum3DProvidersAvailable()
{
	return GC_AUDIO_ENABLED ? 1 : 0;
}

void cSampleManager::SetNum3DProvidersAvailable(uint32 num)
{
}

char *cSampleManager::Get3DProviderName(uint8 id)
{
	(void)id;
	static char enabledName[64] = "GC/Wii Software Audio";
	static char disabledName[64] = "Disabled";
	return GC_AUDIO_ENABLED ? enabledName : disabledName;
}

void cSampleManager::Set3DProviderName(uint8 id, char *name)
{
}

int8 cSampleManager::GetCurrent3DProviderIndex(void)
{
	return GC_AUDIO_ENABLED ? 0 : -1;
}

int8 cSampleManager::SetCurrent3DProvider(uint8 nProvider)
{
	(void)nProvider;
	return GC_AUDIO_ENABLED ? 0 : -1;
}
#endif

bool8
cSampleManager::IsMP3RadioChannelAvailable(void)
{
	return nNumMP3s != 0;
}

void cSampleManager::ReleaseDigitalHandle(void)
{
}

void cSampleManager::ReacquireDigitalHandle(void)
{
}

bool8
cSampleManager::Initialise(void)
{
#ifdef GAMECUBE
	if (_bSampmanInitialised)
		return TRUE;

	ResetSampleBankState();

	for (int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		m_aSamples[i].nOffset = 0;
		m_aSamples[i].nSize = 0;
		m_aSamples[i].nFrequency = 22050;
		m_aSamples[i].nLoopStart = 0;
		m_aSamples[i].nLoopEnd = -1;
	}

	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nMP3BoostVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = FALSE;

	nNumMP3s = 0;
	for (uint32 i = 0; i < TOTAL_STREAMED_SOUNDS; i++)
		gStreamLength[i] = 1;

	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		gStreamLoopedFlag[i] = FALSE;
		gStreamSlots[i].decoder = NULL;
		#ifdef WII
		gStreamSlots[i].sourceBuffer = (int16*)MemoryMgrMallocMem2(sizeof(int16) * GC_SOURCE_BUFFER_FRAMES * 2, 32);
		#else
		gStreamSlots[i].sourceBuffer = (int16*)memalign(32, sizeof(int16) * GC_SOURCE_BUFFER_FRAMES * 2);
		#endif
		if (gStreamSlots[i].sourceBuffer == NULL) {
			for (uint32 j = 0; j < i; j++) {
				#ifdef WII
				MemoryMgrFreeMem2(gStreamSlots[j].sourceBuffer);
				#else
				free(gStreamSlots[j].sourceBuffer);
				#endif
				gStreamSlots[j].sourceBuffer = NULL;
			}
			return FALSE;
		}
		gStreamSlots[i].requestedVolume = MAX_VOLUME;
		gStreamSlots[i].pan = 63;
		gStreamSlots[i].mixVolume = MAX_VOLUME;
		gStreamSlots[i].preloaded = FALSE;
		gStreamSlots[i].loop = FALSE;
		gStreamSlots[i].effectFlag = FALSE;
		gStreamSlots[i].lengthMs = 0;
		gStreamSlots[i].ResetRuntime();
	}

	if (!GC_AUDIO_ENABLED) {
		_bSampmanInitialised = TRUE;
		return TRUE;
	}

	if (!InitialiseSampleBanks()) {
		printf("[GC-AUDIO] WARN: sample bank init failed; continuing with streaming-only audio\n");
	}

	for (uint32 i = 0; i < STREAMED_SOUND_CITY_AMBIENT && i < TOTAL_STREAMED_SOUNDS; i++) {
		uint32 length = 0;
		if (ProbeTrackLength((tTrack)i, length) && length != 0)
			gStreamLength[i] = length;
	}

	StartAudioDmaIfNeeded();
	_bSampmanInitialised = TRUE;
	return TRUE;
#else
	return TRUE;
#endif
}

void
cSampleManager::Terminate(void)
{
#ifdef GAMECUBE
	if (!_bSampmanInitialised)
		return;

	if (gDmaActive) {
		AUDIO_StopDMA();
		AUDIO_RegisterDMACallback(NULL);
		gDmaActive = FALSE;
		gCurrentDmaBuffer = -1;
	}

	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		CloseStreamSlot(i);
		#ifdef WII
		MemoryMgrFreeMem2(gStreamSlots[i].sourceBuffer);
		#else
		free(gStreamSlots[i].sourceBuffer);
		#endif
		gStreamSlots[i].sourceBuffer = NULL;
	}

	FreeSampleBankMemory();
	CloseSampleBankFiles();
	ResetSampleBankState();

	_bSampmanInitialised = FALSE;
#endif
}

bool8 cSampleManager::CheckForAnAudioFileOnCD(void)
{
	return TRUE;
}

char cSampleManager::GetCDAudioDriveLetter(void)
{
	return '\0';
}

void
cSampleManager::UpdateEffectsVolume(void)
{
}

void
cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	m_nEffectsVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (slot.effectFlag) {
			if (i == 1 || i == 2)
				slot.mixVolume = 128 * slot.requestedVolume * m_nEffectsVolume >> 14;
			else
				slot.mixVolume = m_nEffectsFadeVolume * slot.requestedVolume * m_nEffectsVolume >> 14;
		}
	}
#endif
}

void
cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
	m_nMusicVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (!slot.effectFlag) {
			uint32 boost = 0;
#ifdef GTA_PC
			if (MusicManager.GetRadioInCar() == USERTRACK && !MusicManager.CheckForMusicInterruptions())
				boost = m_nMP3BoostVolume / 64;
#endif
			slot.mixVolume = (m_nMusicFadeVolume * slot.requestedVolume * (m_nMusicVolume * boost + m_nMusicVolume)) >> 14;
		}
	}
#endif
}

void
cSampleManager::SetMP3BoostVolume(uint8 nVolume)
{
	m_nMP3BoostVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);
}

void
cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	m_nEffectsFadeVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (slot.effectFlag) {
			if (i == 1 || i == 2)
				slot.mixVolume = 128 * slot.requestedVolume * m_nEffectsVolume >> 14;
			else
				slot.mixVolume = m_nEffectsFadeVolume * slot.requestedVolume * m_nEffectsVolume >> 14;
		}
	}
#endif
}

void
cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
	m_nMusicFadeVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (!slot.effectFlag) {
			uint32 boost = 0;
#ifdef GTA_PC
			if (MusicManager.GetRadioInCar() == USERTRACK && !MusicManager.CheckForMusicInterruptions())
				boost = m_nMP3BoostVolume / 64;
#endif
			slot.mixVolume = (m_nMusicFadeVolume * slot.requestedVolume * (m_nMusicVolume * boost + m_nMusicVolume)) >> 14;
		}
	}
#endif
}

void
cSampleManager::SetMonoMode(bool8 nMode)
{
	m_nMonoMode = nMode;
}

bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return FALSE;
	if (nBank >= MAX_SFX_BANKS || gSampleDataFile == NULL || gSampleBankMemoryStartAddress[nBank] == NULL || gSampleBankSize[nBank] == 0)
		return FALSE;
	if (gSampleBankLoaded[nBank])
		return TRUE;
	if (fseek(gSampleDataFile, long(gSampleBankDiscStartOffset[nBank]), SEEK_SET) != 0)
		return FALSE;
	if (fread(gSampleBankMemoryStartAddress[nBank], 1, gSampleBankSize[nBank], gSampleDataFile) != gSampleBankSize[nBank])
		return FALSE;
	ByteSwapPcm16Buffer(gSampleBankMemoryStartAddress[nBank], gSampleBankSize[nBank]);

	gSampleBankLoaded[nBank] = TRUE;
	return TRUE;
#else
	return FALSE;
#endif
}

void
cSampleManager::UnloadSampleBank(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

#ifdef GAMECUBE
	if (nBank < MAX_SFX_BANKS)
		gSampleBankLoaded[nBank] = FALSE;
#endif
}

int8
cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

#ifdef GAMECUBE
	return gSampleBankLoaded[nBank] ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
#else
	return LOADING_STATUS_NOT_LOADED;
#endif
}

uint8
cSampleManager::IsMissionAudioLoaded(uint8 nSlot, uint32 nSample)
{
	ASSERT(nSlot < MISSION_AUDIO_COUNT);

#ifdef GAMECUBE
	(void)nSlot;
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return LOADING_STATUS_NOT_LOADED;
	return LOADING_STATUS_LOADED;
#else
	return LOADING_STATUS_NOT_LOADED;
#endif
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	ASSERT(nSlot < MISSION_AUDIO_COUNT);

#ifdef GAMECUBE
	(void)nSlot;
	return nSample < TOTAL_AUDIO_SAMPLES;
#else
	return FALSE;
#endif
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	ASSERT(nComment < TOTAL_AUDIO_SAMPLES);

#ifdef GAMECUBE
	return _GetPedCommentSlot(nComment) >= 0 ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
#else
	return LOADING_STATUS_NOT_LOADED;
#endif
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_PEDSFX; i++) {
		if (gPedSlotSfx[i] == int32(nComment))
			return int32(i);
	}
#endif
	return -1;
}

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	ASSERT(nComment < TOTAL_AUDIO_SAMPLES);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED || gSampleDataFile == NULL || nComment >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	if (m_aSamples[nComment].nSize == 0)
		return FALSE;

	uint8 *slotMem = gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS];
	if (slotMem == NULL)
		return FALSE;

	if (fseek(gSampleDataFile, long(m_aSamples[nComment].nOffset), SEEK_SET) != 0)
		return FALSE;

	uint8 *dst = slotMem + PED_BLOCKSIZE * gCurrentPedSlot;
	if (m_aSamples[nComment].nSize > PED_BLOCKSIZE)
		return FALSE;
	if (fread(dst, 1, m_aSamples[nComment].nSize, gSampleDataFile) != m_aSamples[nComment].nSize)
		return FALSE;
	ByteSwapPcm16Buffer(dst, m_aSamples[nComment].nSize);

	gPedSlotSfx[gCurrentPedSlot] = int32(nComment);
	gPedSlotSfxAddr[gCurrentPedSlot] = dst;
	gCurrentPedSlot = (gCurrentPedSlot + 1) % MAX_PEDSFX;
	return TRUE;
#else
	return FALSE;
#endif
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
#ifdef GAMECUBE
	if (offset >= BankStartOffset[SFX_BANK_PED_COMMENTS])
		return SFX_BANK_PED_COMMENTS;
	if (offset >= BankStartOffset[SFX_BANK_0])
		return SFX_BANK_0;
#endif
	return INVALID_SFX_BANK;
}

uint32
cSampleManager::GetSampleBaseFrequency(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return nSample < TOTAL_AUDIO_SAMPLES ? m_aSamples[nSample].nFrequency : 0;
}

uint32
cSampleManager::GetSampleLoopStartOffset(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return nSample < TOTAL_AUDIO_SAMPLES ? m_aSamples[nSample].nLoopStart : 0;
}

int32
cSampleManager::GetSampleLoopEndOffset(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return nSample < TOTAL_AUDIO_SAMPLES ? m_aSamples[nSample].nLoopEnd : 0;
}

uint32
cSampleManager::GetSampleLength(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return nSample < TOTAL_AUDIO_SAMPLES ? m_aSamples[nSample].nSize / sizeof(uint16) : 0;
}

bool8 cSampleManager::UpdateReverb(void)
{
	return FALSE;
}

void
cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].reverbFlag = nReverbFlag != FALSE;
#else
	(void)nReverbFlag;
#endif
}

bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED || nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;

	if (nBank >= MAX_SFX_BANKS)
		nBank = GetBankContainingSound(nSfx);

	if (nBank == INVALID_SFX_BANK)
		return FALSE;

	if (nBank == SFX_BANK_PED_COMMENTS) {
		if (IsPedCommentLoaded(nSfx) != LOADING_STATUS_LOADED && !LoadPedComment(nSfx))
			return FALSE;
	} else if (IsSampleBankLoaded(nBank) != LOADING_STATUS_LOADED && !LoadSampleBank(nBank)) {
		return FALSE;
	}

	const int16 *sampleData = NULL;
	uint32 sampleCount = 0;
	if (!ResolveSampleAddress(m_aSamples, nSfx, nBank, sampleData, sampleCount))
		return FALSE;

	tGcSampleChannel &channel = gSampleChannels[nChannel];
	ResetSampleChannel(channel);
	channel.initialised = TRUE;
	channel.is2D = nChannel >= NUM_CHANNELS_GENERIC;
	channel.effectFlag = !channel.is2D;
	channel.volume = MAX_VOLUME;
	channel.pan = 63;
	channel.baseFrequency = GetSampleBaseFrequency(nSfx);
	channel.frequency = channel.baseFrequency;
	channel.loopStartBytes = GetSampleLoopStartOffset(nSfx);
	channel.loopEndBytes = GetSampleLoopEndOffset(nSfx);
	channel.loopCount = 1;
	channel.sampleData = sampleData;
	channel.sampleCount = sampleCount;
	channel.cursorQ16 = 0;
	channel.stepQ16 = ComputeChannelStepQ16(channel.frequency);
	if (channel.stepQ16 == 0)
		channel.stepQ16 = ComputeChannelStepQ16(channel.baseFrequency != 0 ? channel.baseFrequency : DIGITALRATE);

	return TRUE;
#else
	return FALSE;
#endif
}

#ifdef EXTERNAL_3D_SOUND
void
cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	ASSERT(nChannel < MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].emittingVolume = float(ClampVolume127(nVolume));
	gSampleChannels[nChannel].volume = ClampVolume127(nVolume);
#else
	(void)nVolume;
#endif
}

void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	ASSERT(nChannel < MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].position = CVector(fX, fY, fZ);
#else
	(void)fX;
	(void)fY;
	(void)fZ;
#endif
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	ASSERT(nChannel < MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].maxDistance = fMax;
	(void)fMin;
#else
	(void)fMax;
	(void)fMin;
#endif
}
#endif

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].volume = ClampVolume127(nVolume);
#else
	(void)nVolume;
#endif
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].pan = ClampPan127(nPan);
#else
	(void)nPan;
#endif
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	channel.frequency = nFreq;
	channel.stepQ16 = ComputeChannelStepQ16(nFreq);
	if (nFreq == 0)
		channel.active = FALSE;
#else
	(void)nFreq;
#endif
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].loopStartBytes = nLoopStart;
	gSampleChannels[nChannel].loopEndBytes = nLoopEnd;
#else
	(void)nLoopStart;
	(void)nLoopEnd;
#endif
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	gSampleChannels[nChannel].loopCount = nLoopCount;
#else
	(void)nLoopCount;
#endif
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	return gSampleChannels[nChannel].active;
#else
	return FALSE;
#endif
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	if (!GC_AUDIO_ENABLED || !channel.initialised || channel.sampleData == NULL || channel.sampleCount == 0 || channel.stepQ16 == 0) {
		channel.active = FALSE;
		return;
	}

	channel.active = TRUE;
	channel.paused = FALSE;
	TryFillReadyBuffers();
#endif
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	ResetSampleChannel(gSampleChannels[nChannel]);
#endif
}

void
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return;
	if (nFile >= TOTAL_STREAMED_SOUNDS)
		return;

	CloseStreamSlot(nStream);
	tGcStreamSlot &slot = gStreamSlots[nStream];
	slot.decoder = OpenTrackDecoder(nFile);
	if (slot.decoder == NULL)
		return;

	slot.preloaded = TRUE;
	slot.loop = gStreamLoopedFlag[nStream];
	slot.lengthMs = slot.decoder->GetLengthMS();
	slot.requestedVolume = MAX_VOLUME;
	slot.pan = 63;
	slot.mixVolume = MAX_VOLUME;
	slot.effectFlag = FALSE;
	slot.ResetRuntime();
#endif
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (gStreamSlots[nStream].decoder)
		gStreamSlots[nStream].paused = nPauseFlag != FALSE;
#endif
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return;
	tGcStreamSlot &slot = gStreamSlots[nStream];
	if (!slot.decoder)
		return;

	ResetSlotForPlayback(slot, 0);
	slot.active = TRUE;
	slot.paused = FALSE;
	slot.preloaded = TRUE;
	TryFillReadyBuffers();
#endif
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return FALSE;
	if (nFile >= TOTAL_STREAMED_SOUNDS)
		return FALSE;

	CloseStreamSlot(nStream);
	tGcStreamSlot &slot = gStreamSlots[nStream];
	slot.decoder = OpenTrackDecoder(nFile);
	if (slot.decoder == NULL)
		return FALSE;

	slot.preloaded = TRUE;
	slot.loop = gStreamLoopedFlag[nStream];
	slot.lengthMs = slot.decoder->GetLengthMS();
	slot.requestedVolume = MAX_VOLUME;
	slot.pan = 63;
	slot.mixVolume = MAX_VOLUME;
	slot.effectFlag = FALSE;
	ResetSlotForPlayback(slot, nPos);
	slot.active = TRUE;
	slot.paused = FALSE;
	TryFillReadyBuffers();
	return TRUE;
#else
	return FALSE;
#endif
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	CloseStreamSlot(nStream);
#endif
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	tGcStreamSlot &slot = gStreamSlots[nStream];
	if (!slot.decoder)
		return 0;

	uint32 playedMs = SamplesToMs(GC_OUTPUT_RATE, slot.playedOutputFrames);
	uint32 pos = slot.startPosMs + playedMs;
	if (slot.loop && slot.lengthMs > 0)
		pos %= slot.lengthMs;
	else if (slot.lengthMs > 0 && pos > slot.lengthMs)
		pos = slot.lengthMs;
	return pos;
#else
	return 0;
#endif
}

void
#ifdef GTA_PS2
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nLRPan, uint8 nFRPan, bool8 nEffectFlag, uint8 nStream)
#else
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
#endif
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
#ifdef GTA_PS2
	uint8 pan = nLRPan;
#else
	uint8 pan = nPan;
#endif
	tGcStreamSlot &slot = gStreamSlots[nStream];
	slot.requestedVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);
	slot.pan = Min((uint32)pan, (uint32)MAX_VOLUME);
	slot.effectFlag = nEffectFlag != FALSE;

	if (slot.effectFlag) {
		if (nStream == 1 || nStream == 2)
			slot.mixVolume = 128 * slot.requestedVolume * m_nEffectsVolume >> 14;
		else
			slot.mixVolume = m_nEffectsFadeVolume * slot.requestedVolume * m_nEffectsVolume >> 14;
	} else {
		uint32 boost = 0;
#ifdef GTA_PC
		if (MusicManager.GetRadioInCar() == USERTRACK && !MusicManager.CheckForMusicInterruptions())
			boost = m_nMP3BoostVolume / 64;
#endif
		slot.mixVolume = (m_nMusicFadeVolume * slot.requestedVolume * (m_nMusicVolume * boost + m_nMusicVolume)) >> 14;
	}
#endif
}

int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	ASSERT(nStream < TOTAL_STREAMED_SOUNDS);

#ifdef GAMECUBE
	if (gStreamLength[nStream] <= 1) {
		uint32 length = 0;
		if (ProbeTrackLength((tTrack)nStream, length) && length != 0)
			gStreamLength[nStream] = length;
	}
	return gStreamLength[nStream];
#else
	return 1;
#endif
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	return gStreamSlots[nStream].active;
#else
	return FALSE;
#endif
}

void
cSampleManager::Service(void)
{
#ifdef GAMECUBE
	TryFillReadyBuffers();
#endif
}

bool8
cSampleManager::InitialiseSampleBanks(void)
{
#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return TRUE;

	CloseSampleBankFiles();
	FreeSampleBankMemory();
	ResetSampleBankState();

	if (BankStartOffset[SFX_BANK_0] == 0 || BankStartOffset[SFX_BANK_PED_COMMENTS] == 0) {
		printf("[GC-AUDIO] ERROR: bank start offsets invalid bank0=%u ped=%u\n",
		       BankStartOffset[SFX_BANK_0],
		       BankStartOffset[SFX_BANK_PED_COMMENTS]);
		return FALSE;
	}
	if (!InitialiseSampleBankPaths()) {
		printf("[GC-AUDIO] ERROR: sample bank path init failed\n");
		return FALSE;
	}
	if (!InitialiseSampleTable(m_aSamples)) {
		printf("[GC-AUDIO] ERROR: sample table init failed\n");
		CloseSampleBankFiles();
		return FALSE;
	}
	if (!BuildSampleBankOffsets(m_aSamples)) {
		printf("[GC-AUDIO] ERROR: sample bank offset build failed\n");
		CloseSampleBankFiles();
		return FALSE;
	}

	gSampleBankMemoryStartAddress[SFX_BANK_0] = AllocAlignedAudioBuffer(gSampleBankSize[SFX_BANK_0]);
	if (gSampleBankMemoryStartAddress[SFX_BANK_0] == NULL) {
		printf("[GC-AUDIO] ERROR: alloc failed for SFX bank 0 (%u bytes)\n", gSampleBankSize[SFX_BANK_0]);
		CloseSampleBankFiles();
		return FALSE;
	}

	gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = AllocAlignedAudioBuffer(PED_BLOCKSIZE * MAX_PEDSFX);
	if (gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] == NULL) {
		printf("[GC-AUDIO] ERROR: alloc failed for ped comments bank (%u bytes)\n", PED_BLOCKSIZE * MAX_PEDSFX);
		FreeSampleBankMemory();
		CloseSampleBankFiles();
		return FALSE;
	}
	memset(gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS], 0, PED_BLOCKSIZE * MAX_PEDSFX);

	if (!LoadSampleBank(SFX_BANK_0)) {
		printf("[GC-AUDIO] ERROR: initial SFX bank load failed off=%u size=%u\n",
		       gSampleBankDiscStartOffset[SFX_BANK_0],
		       gSampleBankSize[SFX_BANK_0]);
		FreeSampleBankMemory();
		CloseSampleBankFiles();
		return FALSE;
	}
	return TRUE;
#else
	return TRUE;
#endif
}

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nChannel)
{
#ifdef GAMECUBE
	ASSERT(nChannel < MAX_STREAMS);
	gStreamLoopedFlag[nChannel] = nLoopFlag != FALSE;
	gStreamSlots[nChannel].loop = nLoopFlag != FALSE;
#endif
}

int8 cSampleManager::AutoDetect3DProviders()
{
	return GC_AUDIO_ENABLED ? 0 : -1;
}

#endif
