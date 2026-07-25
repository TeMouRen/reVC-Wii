#include "common.h"
#if !defined(AUDIO_OAL) && !defined(AUDIO_MSS)

#include "sampman.h"
#include "Ps2SfxFormat.h"
#include "GcIdspDspTask.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "MemoryMgr.h"

#ifdef GAMECUBE
#include "../skel/gamecube/gcm_vfs.h"
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/mutex.h>
#include <ogc/semaphore.h>
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

static const bool8 GC_IDSP_DSP_ENABLED =
#if GC_IDSP_DSP_DECODE_ENABLE && defined(WII)
	TRUE;
#else
	FALSE;
#endif

#if GC_AUDIO_DEBUG_LOG
#define GC_AUDIO_LOG(...) printf(__VA_ARGS__)
#else
#define GC_AUDIO_LOG(...) ((void)0)
#endif

#if GC_CUTSCENE_AUDIO_DEBUG_LOG
#define CUTSCENE_AUDIO_LOG(...) printf("[CUT-AUDIO] " __VA_ARGS__)
#else
#define CUTSCENE_AUDIO_LOG(...) ((void)0)
#endif

#if GC_MISSION_AUDIO_DEBUG_LOG
#define MISSION_BANK_LOG(...) printf("[MISSION-BANK] " __VA_ARGS__)
#else
#define MISSION_BANK_LOG(...) ((void)0)
#endif

static const uint32 GC_OUTPUT_RATE = 48000;
static const uint32 GC_DMA_BUFFER_SIZE = 8192;
static const uint32 GC_DMA_BUFFER_FRAMES = GC_DMA_BUFFER_SIZE / (sizeof(int16) * 2);
/* Four 42.7 ms buffers add one block of scheduling headroom without the
 * latency of a much deeper ready queue. Stream I/O runs on another thread. */
static const uint32 GC_DMA_BUFFER_COUNT = 4;
static const uint32 GC_SOURCE_BUFFER_FRAMES = 8192;
static const uint32 GC_STREAM_DECODE_CHUNK_FRAMES = 2048;
static const uint32 GC_STREAM_DECODE_RETRY_LIMIT = 8;
static const uint32 GC_SFX_RESAMPLE_FRAC_BITS = 14;
static const uint32 GC_SFX_RESAMPLE_FRAC_MASK = (1U << GC_SFX_RESAMPLE_FRAC_BITS) - 1;
static const uint32 GC_STREAM_RESAMPLE_FRAC_BITS = 14;
static const uint32 GC_STREAM_RESAMPLE_FRAC_MASK = (1U << GC_STREAM_RESAMPLE_FRAC_BITS) - 1;
/* Cap each compressed refill at 32 KiB instead of holding a 64 KiB logical
 * read. A file offset may still make this straddle two physical clusters. */
static const uint32 GC_IDSP_READ_CACHE_SIZE = 32 * 1024;
static const uint32 GC_IDSP_SECTOR_SIZE = 2048;
static const uint32 GC_SFX_EDGE_RAMP_FRAMES = 64;

static const char *GC_PS2_MISSION_SDT_PATH = "AUDIO\\SFX\\Set0\\sfx2.SDT";
static const char *GC_PS2_MISSION_RAW_PATH = "AUDIO\\SFX\\Set0\\sfx2.RAW";

static const uint32 GC_ADPCM_FRAME_SIZE = 8;
static const uint32 GC_ADPCM_SAMPLES_PER_FRAME = 14;

/* The first DSP batch is checked against the existing CPU decoder. A bad
 * accelerator result must never turn an otherwise valid stream into silence. */
static bool8 gIdspDspOutputValidated = FALSE;
static bool8 gIdspDspOutputRejected = FALSE;

static const uint32 VB_BLOCK_SIZE = 0x2000;
static const uint32 VAG_LINE_SIZE = 0x10;
static const uint32 VAG_SAMPLES_IN_LINE = 28;
static const uint32 NUM_VAG_LINES_IN_BLOCK = VB_BLOCK_SIZE / VAG_LINE_SIZE;
static const uint32 NUM_VAG_SAMPLES_IN_BLOCK = NUM_VAG_LINES_IN_BLOCK * VAG_SAMPLES_IN_LINE;
static const uint32 GC_SFX_PATH_CAPACITY = 128;
static const char *GC_SAMPLE_BANK_DESC_PATH = "AUDIO\\SFX.SDT";
static const char *GC_SAMPLE_BANK_DATA_PATH = "AUDIO\\SFX.RAW";

enum eDmaBufferState
{
	GC_BUFFER_FREE = 0,
	GC_BUFFER_READY,
	GC_BUFFER_PLAYING,
	/* Reserved by the producer while it performs decode/file I/O. */
	GC_BUFFER_FILLING,
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
	uint8 requestedVolume;
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
	uint32 cursorFixed;
	uint32 stepFixed;
	uint32 renderedOutputFrames;
	uint32 edgeRampFrames;
};

static FILE *gSampleDescFile = NULL;
static FILE *gSampleDataFile = NULL;
struct tPs2MissionSdtEntry
{
	uint32 rawOffset;
	uint32 allocatedBytes;
	uint32 sampleRate;
};

enum eGcSfxBackend
{
	GC_SFX_BACKEND_UNSELECTED = 0,
	GC_SFX_BACKEND_PC_PCM,
	GC_SFX_BACKEND_PS2_VAG,
};

struct tPs2SfxSdtEntry
{
	uint32 rawOffset;
	uint32 allocatedBytes;
	uint32 sampleRate;
	uint8 bank;
	uint16 localEntry;
};

static tPs2MissionSdtEntry gPs2MissionSdt[PS2_SFX_MISSION_ENTRY_COUNT];
static bool8 gPs2MissionBankAvailable = FALSE;
static eGcSfxBackend gSfxBackend = GC_SFX_BACKEND_UNSELECTED;
static tPs2SfxSdtEntry gPs2SfxSdt[TOTAL_AUDIO_SAMPLES];
static bool8 gPs2SfxSdtValid[TOTAL_AUDIO_SAMPLES];
static uint8 *gPs2SfxDecodedArena = NULL;
static uint32 gPs2SfxDecodedArenaBytes = 0;
static int16 *gPs2SfxDecoded[TOTAL_AUDIO_SAMPLES];
static uint32 gPs2SfxDecodedBytes[TOTAL_AUDIO_SAMPLES];
static uint32 gPedSlotSizeBytes[MAX_PEDSFX];
static uint32 gPedSlotCapacityBytes = PED_BLOCKSIZE;
static bool8 gSampleBankLoaded[MAX_SFX_BANKS];
static uint32 gSampleBankDiscStartOffset[MAX_SFX_BANKS];
static uint32 gSampleBankSize[MAX_SFX_BANKS];
static uint8 *gSampleBankMemoryStartAddress[MAX_SFX_BANKS];
static int32 gPedSlotSfx[MAX_PEDSFX];
static uint8 *gPedSlotSfxAddr[MAX_PEDSFX];
static uint8 gCurrentPedSlot = 0;
static tGcSampleChannel gSampleChannels[MAXCHANNELS + MAX2DCHANNELS];
static uint32 gSampleDataEndOffset = 0;

static uint32 ReadLE32(FILE *file);
static bool8 EndsWithIgnoreCase(const char *path, const char *suffix);
static bool8 BuildPs2SfxPath(uint32 bank, const char *extension, char *path, uint32 pathCapacity);
static bool8 MapSfxToPs2Record(uint32 nSfx, uint8 &bank, uint16 &localEntry);
static bool8 MapPs2RecordToSfx(uint8 bank, uint16 localEntry, uint32 &nSfx);
static bool8 DecodePs2VagData(const uint8 *vagData, uint32 vagBytes, int16 *pcmData, uint32 pcmCapacityBytes,
	uint32 &pcmBytes, uint32 &loopStartBytes, int32 &loopEndBytes);
static bool8 ResolvePs2SampleAddress(uint32 nSfx, const int16 *&sampleData, uint32 &sampleCount, uint32 &sampleBytes);
static bool8 InitialisePs2SampleBanks(tSample *samples);
static bool8 IsPs2PedComment(uint32 nSample);
static inline bool8 ShouldEagerProbePackagedTrack(tTrack track);
static inline uint32 Ps2MissionTrackToOrdinal(tTrack track);
static inline uint32 MsToSamples(uint32 sampleRate, uint32 milliseconds);
static inline uint32 SamplesToMs(uint32 sampleRate, uint64 samples);

static_assert(PS2_SFX_MISSION_ENTRY_COUNT == 1121U, "PS2 mission bank count must match streamed mission tracks");
static_assert(PS2_SFX_MISSION_LAST_SDT_ENTRY >= PS2_SFX_MISSION_FIRST_SDT_ENTRY, "PS2 mission SDT range is invalid");
static_assert(PS2_SFX_BUST_ENTRY_COUNT == 28U, "PS2 busted dialogue count changed");
static_assert(SFX_MISSION_BUST_01 == PS2_SFX_BUST_FIRST_TRACK_ID, "PS2 BUST_01 track mapping changed");
static_assert(SFX_MISSION_BUST_28 == PS2_SFX_BUST_LAST_TRACK_ID, "PS2 BUST_28 track mapping changed");
static_assert(SFX_POLICE_RADIO_VAN - SFX_CRIME_1 + 1U == PS2_SFX_POLICE_ENTRY_COUNT,
              "PC police radio range must match the PS2 sfx2 tail");
static_assert(SFX_RADIO_DIAL_3 - SFX_HELI_1 + 1U == 81U, "PS2 sfx0 middle range changed");
static_assert(SFX_TIMER - SFX_INFO_LEFT + 1U == 15U, "PS2 sfx0 information range changed");
static_assert(SFX_FE_NOISE_BURST_3 - SFX_FE_HIGHLIGHT_LEFT + 1U == 11U, "PS2 frontend range changed");
static_assert(TOTAL_AUDIO_SAMPLES - SAMPLEBANK_PED_START == PS2_SFX_MISSION_FIRST_SDT_ENTRY,
              "PC ped comment count must match the PS2 sfx2 prefix");

class CGcStreamDecoder
{
public:
	virtual ~CGcStreamDecoder() {}

	/* Most decoders validate their container in the constructor. Mission bank
	 * decoders override this to validate the lazy-loaded RAW slice before the
	 * caller marks a mission line as loaded. */
	virtual bool Prepare() { return true; }
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

	void DecodeLineWithFlagsMode(const uint8 *inbuf, int16 *outbuf, bool legacyFlag7Silence)
	{
		double samples[VAG_SAMPLES_IN_LINE];

		int predict_nr = *inbuf++;
		int shift_factor = predict_nr & 0xF;
		predict_nr >>= 4;
		int flags = *inbuf++;
		if (legacyFlag7Silence && flags == 7) {
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

	void DecodeLine(const uint8 *inbuf, int16 *outbuf)
	{
		DecodeLineWithFlagsMode(inbuf, outbuf, true);
	}

	void DecodeLineIgnoringFlags(const uint8 *inbuf, int16 *outbuf)
	{
		DecodeLineWithFlagsMode(inbuf, outbuf, false);
	}
};

class CPs2MissionSfxDecoder : public CGcStreamDecoder
{
	FILE *m_pFile;
	uint8 *m_pData;
	bool m_bOpened;
	bool m_bLoaded;
	bool m_bLoadFailed;
	bool m_bTerminalPending;
	bool m_bEnded;

	tTrack m_Track;
	uint32 m_RawOffset;
	uint32 m_AllocatedBytes;
	uint32 m_SampleRate;
	uint32 m_FrameCount;
	uint32 m_CurrentFrame;
	uint32 m_CurrentSample;
	uint32 m_SampleCount;
	uint32 m_SkipSamples;
	uint32 m_LengthMS;

	CVagDecoder m_Decoder;
	int16 m_PendingSamples[PS2_SFX_VAG_SAMPLES_PER_FRAME * 2];
	uint32 m_PendingCount;
	uint32 m_PendingPos;

	void ResetPending()
	{
		m_PendingCount = 0;
		m_PendingPos = 0;
	}

	bool LoadSlice()
	{
		if (m_bLoaded)
			return true;
		if (m_bLoadFailed || !m_bOpened)
			return false;

		m_pFile = fopen(GC_PS2_MISSION_RAW_PATH, "rb");
		if (m_pFile == NULL) {
			MISSION_BANK_LOG("open failed track=%u path=%s\n", uint32(m_Track), GC_PS2_MISSION_RAW_PATH);
			m_bLoadFailed = true;
			return false;
		}
		if (fseek(m_pFile, long(m_RawOffset), SEEK_SET) != 0) {
			MISSION_BANK_LOG("seek failed track=%u offset=%u\n", uint32(m_Track), m_RawOffset);
			fclose(m_pFile);
			m_pFile = NULL;
			m_bLoadFailed = true;
			return false;
		}

		m_pData = new uint8[m_AllocatedBytes];
		if (m_pData == NULL || fread(m_pData, 1, m_AllocatedBytes, m_pFile) != m_AllocatedBytes) {
			MISSION_BANK_LOG("read failed track=%u offset=%u bytes=%u\n",
			                 uint32(m_Track), m_RawOffset, m_AllocatedBytes);
			fclose(m_pFile);
			m_pFile = NULL;
			delete[] m_pData;
			m_pData = NULL;
			m_bLoadFailed = true;
			return false;
		}

		bool terminalFound = false;
		uint32 terminalFrameIndex = 0;
		for (uint32 frameIndex = 0; frameIndex < m_FrameCount; frameIndex++) {
			const uint8 *frame = m_pData + frameIndex * PS2_SFX_VAG_FRAME_SIZE;
			if (!IsSupportedPs2MissionVagFlags(frame[1]) || !IsSupportedPs2VagPredictor(frame[0])) {
				MISSION_BANK_LOG("validate failed track=%u frame=%u flags=%u predictor=%u\n",
				                 uint32(m_Track), frameIndex, uint32(frame[1]), uint32(frame[0] >> 4));
				fclose(m_pFile);
				m_pFile = NULL;
				delete[] m_pData;
				m_pData = NULL;
				m_bLoadFailed = true;
				return false;
			}
			if (terminalFound) {
				if (!IsPs2VagPostEndPadding(frame[1])) {
					MISSION_BANK_LOG("validate failed track=%u frame=%u post-end flags=%u\n",
					                 uint32(m_Track), frameIndex, uint32(frame[1]));
					fclose(m_pFile);
					m_pFile = NULL;
					delete[] m_pData;
					m_pData = NULL;
					m_bLoadFailed = true;
					return false;
				}
			} else if (IsPs2VagEndFrame(frame[1])) {
				terminalFound = true;
				terminalFrameIndex = frameIndex;
			}
		}
		if (!terminalFound) {
			MISSION_BANK_LOG("validate failed track=%u missing terminal frame\n", uint32(m_Track));
			fclose(m_pFile);
			m_pFile = NULL;
			delete[] m_pData;
			m_pData = NULL;
			m_bLoadFailed = true;
			return false;
		}
		m_SampleCount = (terminalFrameIndex + 1U) * PS2_SFX_VAG_SAMPLES_PER_FRAME;
		m_LengthMS = SamplesToMs(m_SampleRate, m_SampleCount);

		fclose(m_pFile);
		m_pFile = NULL;
		m_bLoaded = true;
		MISSION_BANK_LOG("open track=%u ordinal=%u sdt=%u offset=%u bytes=%u rate=%u\n",
		                 uint32(m_Track), Ps2MissionTrackToOrdinal(m_Track),
		                 Ps2SfxMissionOrdinalToSdtEntry(Ps2MissionTrackToOrdinal(m_Track)),
		                 m_RawOffset, m_AllocatedBytes, m_SampleRate);
		return true;
	}

	void FailDecode(const char *reason)
	{
		if (!m_bLoadFailed)
			MISSION_BANK_LOG("decode failed track=%u frame=%u reason=%s\n",
			                 uint32(m_Track), m_CurrentFrame, reason);
		m_bLoadFailed = true;
		m_bEnded = true;
		ResetPending();
	}

public:
	CPs2MissionSfxDecoder(tTrack track, const tPs2MissionSdtEntry &entry)
		: m_pFile(NULL),
		  m_pData(NULL),
		  m_bOpened(entry.allocatedBytes != 0 &&
		            (entry.allocatedBytes % PS2_SFX_VAG_FRAME_SIZE) == 0 &&
		            entry.sampleRate != 0),
		  m_bLoaded(false),
		  m_bLoadFailed(false),
		  m_bTerminalPending(false),
		  m_bEnded(false),
		  m_Track(track),
		  m_RawOffset(entry.rawOffset),
		  m_AllocatedBytes(entry.allocatedBytes),
		  m_SampleRate(entry.sampleRate),
		  m_FrameCount(entry.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE),
		  m_CurrentFrame(0),
		  m_CurrentSample(0),
		  m_SampleCount((entry.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE) * PS2_SFX_VAG_SAMPLES_PER_FRAME),
		  m_SkipSamples(0),
		  m_LengthMS(entry.sampleRate == 0 ? 0 : SamplesToMs(entry.sampleRate,
		              uint64(entry.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE) * PS2_SFX_VAG_SAMPLES_PER_FRAME)),
		  m_PendingCount(0),
		  m_PendingPos(0)
	{
		memset(m_PendingSamples, 0, sizeof(m_PendingSamples));
	}

	~CPs2MissionSfxDecoder()
	{
		if (m_pFile)
			fclose(m_pFile);
		delete[] m_pData;
	}

	bool IsOpened() const
	{
		return m_bOpened && !m_bLoadFailed;
	}

	bool Prepare()
	{
		return IsOpened() && LoadSlice();
	}

	uint32 GetLengthMS() const
	{
		return m_LengthMS;
	}

	uint32 GetSampleRate() const
	{
		return m_SampleRate;
	}

	void SeekMS(uint32 milliseconds)
	{
		if (!m_bOpened)
			return;

		m_Decoder.ResetState();
		m_CurrentFrame = 0;
		m_CurrentSample = 0;
		m_SkipSamples = MsToSamples(m_SampleRate, milliseconds);
		m_bTerminalPending = false;
		m_bEnded = false;
		ResetPending();
	}

	uint32 DecodeFrames(int16 *buffer, uint32 maxFrames)
	{
		if (!IsOpened() || buffer == NULL || maxFrames == 0 || !LoadSlice())
			return 0;

		uint32 framesWritten = 0;
		int16 decoded[PS2_SFX_VAG_SAMPLES_PER_FRAME];
		while (framesWritten < maxFrames && !m_bEnded) {
			if (m_PendingPos < m_PendingCount) {
				uint32 available = m_PendingCount - m_PendingPos;
				if (m_SkipSamples != 0) {
					uint32 skipped = Min(available, m_SkipSamples);
					m_PendingPos += skipped;
					m_CurrentSample += skipped;
					m_SkipSamples -= skipped;
					if (m_PendingPos >= m_PendingCount)
						ResetPending();
					continue;
				}

				uint32 toCopy = Min(available, maxFrames - framesWritten);
				memcpy(buffer + framesWritten * 2,
				       m_PendingSamples + m_PendingPos * 2,
				       sizeof(int16) * toCopy * 2);
				m_PendingPos += toCopy;
				m_CurrentSample += toCopy;
				framesWritten += toCopy;
				if (m_PendingPos >= m_PendingCount) {
					ResetPending();
					if (m_bTerminalPending)
						m_bEnded = true;
				}
				continue;
			}

			if (m_bTerminalPending) {
				m_bEnded = true;
				break;
			}
			if (m_CurrentFrame >= m_FrameCount) {
				FailDecode("missing end frame");
				break;
			}

			const uint8 *frame = m_pData + m_CurrentFrame * PS2_SFX_VAG_FRAME_SIZE;
			uint8 flags = frame[1];
			if (!IsSupportedPs2MissionVagFlags(flags)) {
				FailDecode("unsupported flags");
				break;
			}
			if (!IsSupportedPs2VagPredictor(frame[0])) {
				FailDecode("unsupported predictor");
				break;
			}

			m_Decoder.DecodeLineIgnoringFlags(frame, decoded);
			for (uint32 i = 0; i < PS2_SFX_VAG_SAMPLES_PER_FRAME; i++) {
				m_PendingSamples[i * 2 + 0] = decoded[i];
				m_PendingSamples[i * 2 + 1] = decoded[i];
			}
			m_PendingCount = PS2_SFX_VAG_SAMPLES_PER_FRAME;
			m_PendingPos = 0;
			m_CurrentFrame++;
			if (IsPs2VagEndFrame(flags)) {
				m_bTerminalPending = true;
				m_SampleCount = m_CurrentFrame * PS2_SFX_VAG_SAMPLES_PER_FRAME;
				m_LengthMS = SamplesToMs(m_SampleRate, m_SampleCount);
				MISSION_BANK_LOG("decoded track=%u samples=%u lengthMs=%u\n",
				                 uint32(m_Track), m_SampleCount, m_LengthMS);
			}
		}

		return framesWritten;
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

static inline bool8 IsPs2MissionBankTrack(tTrack track)
{
	return uint32(track) >= PS2_SFX_MISSION_FIRST_TRACK_ID &&
	       uint32(track) <= PS2_SFX_MISSION_LAST_TRACK_ID;
}

static inline uint32 Ps2MissionTrackToOrdinal(tTrack track)
{
	return uint32(track) - PS2_SFX_MISSION_FIRST_TRACK_ID;
}

struct tPs2SfxSpan
{
	uint32 firstSfx;
	uint32 lastSfx;
	uint8 firstBank;
	uint8 entriesPerBank;
};

/* These spans are the PS2 bank layout. The PC enum contains the 68 police
 * radio fragments and nine extra radio dials; the mapping functions below
 * account for those two layout differences explicitly. */
static const tPs2SfxSpan gPs2SfxSpans[] = {
	{ SFX_CAR_ACCEL_1, SFX_CAR_FINGER_OFF_ACCEL_16, 4, 3 },
	{ SFX_CAR_ACCEL_17, SFX_CAR_WIND_20, 20, 4 },
	{ SFX_CAR_ACCEL_21, SFX_CAR_AFTER_ACCEL_22, 24, 1 },
	{ SFX_HELI_APACHE_1, SFX_HELI_APACHE_4, 30, 4 },
	{ SFX_SEAPLANE_PRO1, SFX_SEAPLANE_LOW, 35, 5 },
	{ SFX_PLANE_UNUSED_1, SFX_PLANE_UNUSED_4, 36, 1 },
	{ SFX_BUILDINGS_BANK_ALARM, SFX_BUILDING_CHURCH, 40, 1 },
	{ SFX_BUILDING_FAN_1, SFX_BUILDING_FAN_1, 53, 1 },
	{ SFX_BUILDING_FAN_2, SFX_BUILDING_FAN_4, 54, 3 },
	{ SFX_BUILDING_INSECTS_1, SFX_BUILDING_INSECTS_1, 55, 1 },
	{ SFX_BUILDING_INSECTS_2, SFX_BUILDING_INSECTS_5, 56, 4 },
	{ SFX_CLUB_1, SFX_CLUB_4, 57, 1 },
	{ SFX_FOOTSTEP_GRASS_1, SFX_FOOTSTEP_GRASS_5, 61, 5 },
	{ SFX_FOOTSTEP_GRAVEL_1, SFX_FOOTSTEP_GRAVEL_5, 62, 5 },
	{ SFX_FOOTSTEP_WOOD_1, SFX_FOOTSTEP_WOOD_5, 63, 5 },
	{ SFX_FOOTSTEP_METAL_1, SFX_FOOTSTEP_METAL_5, 64, 5 },
	{ SFX_FOOTSTEP_WATER_1, SFX_FOOTSTEP_WATER_4, 65, 4 },
	{ SFX_FOOTSTEP_SAND_1, SFX_FOOTSTEP_SAND_4, 66, 4 },
};

static bool8 MapSfxToPs2Record(uint32 nSfx, uint8 &bank, uint16 &localEntry)
{
	if (nSfx <= 197U) {
		bank = 0;
		localEntry = uint16(nSfx);
		return TRUE;
	}
	if (nSfx >= SFX_CRIME_1 && nSfx <= SFX_POLICE_RADIO_VAN) {
		bank = 2;
		localEntry = uint16(PS2_SFX_POLICE_FIRST_SDT_ENTRY + (nSfx - SFX_CRIME_1));
		return TRUE;
	}
	if (nSfx >= SFX_HELI_1 && nSfx <= SFX_RADIO_DIAL_3) {
		bank = 0;
		localEntry = uint16(198U + (nSfx - SFX_HELI_1));
		return TRUE;
	}
	if (nSfx >= SFX_INFO_LEFT && nSfx <= SFX_TIMER) {
		bank = 0;
		localEntry = uint16(279U + (nSfx - SFX_INFO_LEFT));
		return TRUE;
	}
	if (nSfx >= SFX_FE_HIGHLIGHT_LEFT && nSfx <= SFX_FE_NOISE_BURST_3) {
		bank = 3;
		localEntry = uint16(nSfx - SFX_FE_HIGHLIGHT_LEFT);
		return TRUE;
	}
	if (nSfx >= SAMPLEBANK_PED_START && nSfx < TOTAL_AUDIO_SAMPLES) {
		bank = 2;
		localEntry = uint16(nSfx - SAMPLEBANK_PED_START);
		return TRUE;
	}

	for (uint32 i = 0; i < sizeof(gPs2SfxSpans) / sizeof(gPs2SfxSpans[0]); i++) {
		const tPs2SfxSpan &span = gPs2SfxSpans[i];
		if (nSfx < span.firstSfx || nSfx > span.lastSfx)
			continue;
		uint32 relative = nSfx - span.firstSfx;
		bank = uint8(span.firstBank + relative / span.entriesPerBank);
		localEntry = uint16(relative % span.entriesPerBank);
		return TRUE;
	}
	return FALSE;
}

static bool8 MapPs2RecordToSfx(uint8 bank, uint16 localEntry, uint32 &nSfx)
{
	if (bank == 0) {
		if (localEntry <= 197U) {
			nSfx = localEntry;
			return TRUE;
		}
		if (localEntry >= 198U && localEntry <= 278U) {
			nSfx = SFX_HELI_1 + (localEntry - 198U);
			return TRUE;
		}
		if (localEntry >= 279U && localEntry <= 293U) {
			nSfx = SFX_INFO_LEFT + (localEntry - 279U);
			return TRUE;
		}
		return FALSE;
	}
	if (bank == 2) {
		if (localEntry < PS2_SFX_MISSION_FIRST_SDT_ENTRY) {
			nSfx = SAMPLEBANK_PED_START + localEntry;
			return nSfx < TOTAL_AUDIO_SAMPLES;
		}
		if (localEntry >= PS2_SFX_POLICE_FIRST_SDT_ENTRY && localEntry <= PS2_SFX_POLICE_LAST_SDT_ENTRY) {
			nSfx = SFX_CRIME_1 + (localEntry - PS2_SFX_POLICE_FIRST_SDT_ENTRY);
			return TRUE;
		}
		return FALSE;
	}
	if (bank == 3 && localEntry < 11U) {
		nSfx = SFX_FE_HIGHLIGHT_LEFT + localEntry;
		return TRUE;
	}

	for (uint32 i = 0; i < sizeof(gPs2SfxSpans) / sizeof(gPs2SfxSpans[0]); i++) {
		const tPs2SfxSpan &span = gPs2SfxSpans[i];
		uint32 spanCount = span.lastSfx - span.firstSfx + 1U;
		uint32 bankCount = (spanCount + span.entriesPerBank - 1U) / span.entriesPerBank;
		if (bank < span.firstBank || bank >= span.firstBank + bankCount)
			continue;
		uint32 relative = uint32(bank - span.firstBank) * span.entriesPerBank + localEntry;
		if (relative >= spanCount)
			return FALSE;
		nSfx = span.firstSfx + relative;
		return TRUE;
	}
	return FALSE;
}

static bool8 BuildPs2SfxPath(uint32 bank, const char *extension, char *path, uint32 pathCapacity)
{
	if (extension == NULL || path == NULL || pathCapacity == 0 || bank >= PS2_SFX_BANK_COUNT)
		return FALSE;
	int written = snprintf(path, pathCapacity, "AUDIO\\SFX\\Set%u\\sfx%u.%s", bank / 10U, bank, extension);
	return written > 0 && uint32(written) < pathCapacity;
}

static bool8 GetPs2MissionSdtEntry(tTrack track, tPs2MissionSdtEntry &entry)
{
	if (gSfxBackend != GC_SFX_BACKEND_PS2_VAG ||
	    !gPs2MissionBankAvailable || !IsPs2MissionBankTrack(track))
		return FALSE;

	uint32 ordinal = Ps2MissionTrackToOrdinal(track);
	if (ordinal >= PS2_SFX_MISSION_ENTRY_COUNT)
		return FALSE;

	entry = gPs2MissionSdt[ordinal];
	return TRUE;
}

static void ResetPs2MissionBank()
{
	gPs2MissionBankAvailable = FALSE;
	memset(gPs2MissionSdt, 0, sizeof(gPs2MissionSdt));
}

static bool8 InitialisePs2MissionBank()
{
	ResetPs2MissionBank();
	if (gSfxBackend != GC_SFX_BACKEND_PS2_VAG)
		return FALSE;

#if !GC_PS2_MISSION_BANK_ENABLE
	MISSION_BANK_LOG("disabled\n");
	return FALSE;
#else
	FILE *sdtFile = fopen(GC_PS2_MISSION_SDT_PATH, "rb");
	if (sdtFile == NULL) {
		MISSION_BANK_LOG("missing index path=%s\n", GC_PS2_MISSION_SDT_PATH);
		return FALSE;
	}

	FILE *rawFile = fopen(GC_PS2_MISSION_RAW_PATH, "rb");
	if (rawFile == NULL) {
		MISSION_BANK_LOG("missing data path=%s\n", GC_PS2_MISSION_RAW_PATH);
		fclose(sdtFile);
		return FALSE;
	}

	if (fseek(sdtFile, 0, SEEK_END) != 0 || fseek(rawFile, 0, SEEK_END) != 0) {
		MISSION_BANK_LOG("failed to seek bank metadata\n");
		fclose(rawFile);
		fclose(sdtFile);
		return FALSE;
	}

	long sdtSize = ftell(sdtFile);
	long rawSize = ftell(rawFile);
	uint64 requiredSdtSize = uint64(PS2_SFX_MISSION_LAST_SDT_ENTRY + 1U) * PS2_SFX_SDT_RECORD_SIZE;
	if (sdtSize < 0 || rawSize <= 0 || uint64(sdtSize) < requiredSdtSize) {
		MISSION_BANK_LOG("invalid sizes sdt=%ld raw=%ld requiredSdt=%u\n",
		                 sdtSize, rawSize, uint32(requiredSdtSize));
		fclose(rawFile);
		fclose(sdtFile);
		return FALSE;
	}

	uint8 record[PS2_SFX_SDT_RECORD_SIZE];
	for (uint32 ordinal = 0; ordinal < PS2_SFX_MISSION_ENTRY_COUNT; ordinal++) {
		uint32 sdtEntry = Ps2SfxMissionOrdinalToSdtEntry(ordinal);
		if (sdtEntry == PS2_SFX_INVALID_SDT_ENTRY) {
			MISSION_BANK_LOG("missing SDT mapping ordinal=%u\n", ordinal);
			fclose(rawFile);
			fclose(sdtFile);
			ResetPs2MissionBank();
			return FALSE;
		}
		long recordOffset = long(uint64(sdtEntry) * PS2_SFX_SDT_RECORD_SIZE);
		if (fseek(sdtFile, recordOffset, SEEK_SET) != 0 ||
		    fread(record, 1, sizeof(record), sdtFile) != sizeof(record)) {
			MISSION_BANK_LOG("short index read entry=%u\n", sdtEntry);
			fclose(rawFile);
			fclose(sdtFile);
			ResetPs2MissionBank();
			return FALSE;
		}

		tPs2MissionSdtEntry &entry = gPs2MissionSdt[ordinal];
		entry.rawOffset = ReadPs2SfxLe32(record + 0);
		entry.allocatedBytes = ReadPs2SfxLe32(record + 4);
		entry.sampleRate = ReadPs2SfxLe32(record + 8);
		uint64 sliceEnd = uint64(entry.rawOffset) + entry.allocatedBytes;
		if (entry.allocatedBytes == 0 ||
		    (entry.rawOffset % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
		    (entry.allocatedBytes % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
		    entry.sampleRate == 0 ||
		    sliceEnd > uint64(rawSize)) {
			MISSION_BANK_LOG("invalid entry=%u offset=%u size=%u rate=%u raw=%ld\n",
			                 sdtEntry, entry.rawOffset, entry.allocatedBytes, entry.sampleRate, rawSize);
			fclose(rawFile);
			fclose(sdtFile);
			ResetPs2MissionBank();
			return FALSE;
		}
	}

	fclose(rawFile);
	fclose(sdtFile);
	gPs2MissionBankAvailable = TRUE;
	MISSION_BANK_LOG("ready entries=%u sdt=%u..%u rawBytes=%ld\n",
	                 PS2_SFX_MISSION_ENTRY_COUNT,
	                 PS2_SFX_MISSION_FIRST_SDT_ENTRY,
	                 PS2_SFX_MISSION_LAST_SDT_ENTRY,
	                 rawSize);
	return TRUE;
#endif
}

static inline bool8 IsThisTrackAt16KHz(uint32 track)
{
	return track == STREAMED_SOUND_RADIO_KCHAT || track == STREAMED_SOUND_RADIO_VCPR || track == STREAMED_SOUND_RADIO_POLICE;
}

static inline const char *GetPackagedTrackPath(tTrack track)
{
#ifdef PS2_AUDIO_PATHS
	return track < CountOfPS2Table() ? PS2StreamedNameTable[track] : NULL;
#else
	return NULL;
#endif
}

static inline bool8 IsCompressedStreamPath(const char *path)
{
	return path != NULL && EndsWithIgnoreCase(path, ".idsp");
}

static inline bool8 IsSupportedGcStreamTrack(tTrack track)
{
	if (IsPs2MissionBankTrack(track))
		return gSfxBackend == GC_SFX_BACKEND_PS2_VAG && gPs2MissionBankAvailable;

	const char *packagedPath = GetPackagedTrackPath(track);
	return IsCompressedStreamPath(packagedPath)
#ifndef GTA_PS2
		|| track == STREAMED_SOUND_RADIO_MP3_PLAYER
#endif
		;
}

static inline bool8 IsCutsceneStreamTrack(tTrack track)
{
	return track >= STREAMED_SOUND_CUTSCENE_ASS_1 &&
	       track <= STREAMED_SOUND_CUTSCENE_ELBURRO2_PH2;
}

static inline bool8 ShouldEagerProbePackagedTrack(tTrack track)
{
	return !IsPs2MissionBankTrack(track) && IsSupportedGcStreamTrack(track);
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
	channel.requestedVolume = MAX_VOLUME;
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
	channel.cursorFixed = 0;
	channel.stepFixed = 0;
	channel.renderedOutputFrames = 0;
	channel.edgeRampFrames = 0;
}

static void ResetSampleBankState()
{
	gPedSlotCapacityBytes = PED_BLOCKSIZE;
	for (uint32 i = 0; i < MAX_SFX_BANKS; i++) {
		gSampleBankLoaded[i] = FALSE;
		gSampleBankDiscStartOffset[i] = 0;
		gSampleBankSize[i] = 0;
		gSampleBankMemoryStartAddress[i] = NULL;
	}

	for (uint32 i = 0; i < MAX_PEDSFX; i++) {
		gPedSlotSfx[i] = NO_SAMPLE;
		gPedSlotSfxAddr[i] = NULL;
		gPedSlotSizeBytes[i] = 0;
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
	if (gPs2SfxDecodedArena != NULL) {
#ifdef WII
		MemoryMgrFreeMem2(gPs2SfxDecodedArena);
#else
		free(gPs2SfxDecodedArena);
#endif
		gPs2SfxDecodedArena = NULL;
		gPs2SfxDecodedArenaBytes = 0;
	}
	for (uint32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		gPs2SfxDecoded[i] = NULL;
		gPs2SfxDecodedBytes[i] = 0;
	}

	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++)
		ResetSampleChannel(gSampleChannels[i]);
	for (uint32 i = 0; i < MAX_PEDSFX; i++) {
		gPedSlotSfxAddr[i] = NULL;
		gPedSlotSizeBytes[i] = 0;
	}
}

static uint8 *AllocAlignedAudioBuffer(uint32 sizeBytes)
{
#ifdef WII
	return (uint8*)MemoryMgrMallocMem2(sizeBytes, 32);
#else
	return (uint8*)memalign(32, sizeBytes);
#endif
}

static uint8 *AllocAlignedAudioBufferStrict(uint32 sizeBytes)
{
#ifdef WII
	return (uint8*)MemoryMgrMallocMem2Strict(sizeBytes, 32);
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
#if defined(RW_BIG_ENDIAN) && !defined(GTA_PS2)
	for (uint32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		samples[i].nOffset = ReadLE32(gSampleDescFile);
		samples[i].nSize = ReadLE32(gSampleDescFile);
		samples[i].nFrequency = ReadLE32(gSampleDescFile);
		samples[i].nLoopStart = ReadLE32(gSampleDescFile);
		samples[i].nLoopEnd = int32(ReadLE32(gSampleDescFile));
		if (ferror(gSampleDescFile) || feof(gSampleDescFile))
			return FALSE;
	}
#else
	size_t got = fread(samples, 1, expected, gSampleDescFile);
	if (got != expected) {
		printf("[GC-AUDIO] ERROR: sample table short read (%u/%u bytes)\n", (uint32)got, (uint32)expected);
		return FALSE;
	}
#endif

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

static uint32 ComputeChannelStepFixed(uint32 frequency)
{
	if (frequency == 0)
		return 0;
	uint64 step = ((uint64(frequency) << GC_SFX_RESAMPLE_FRAC_BITS) + GC_OUTPUT_RATE / 2) / GC_OUTPUT_RATE;
	if (step == 0)
		step = 1;
	return uint32(step);
}

static uint32 ComputeSampleEdgeRampFrames(uint32 sampleCount, uint32 stepFixed)
{
	if (sampleCount == 0 || stepFixed == 0)
		return 0;

	uint64 durationFixed = uint64(sampleCount) << GC_SFX_RESAMPLE_FRAC_BITS;
	uint64 outputFrames = (durationFixed + stepFixed - 1) / stepFixed;
	return uint32(Min(uint64(GC_SFX_EDGE_RAMP_FRAMES), outputFrames / 2));
}

static uint32 AlignUp32(uint32 value)
{
	return (value + 31U) & ~31U;
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

static bool8 IsPs2PedComment(uint32 nSample)
{
	return nSample >= SAMPLEBANK_PED_START && nSample < TOTAL_AUDIO_SAMPLES;
}

static bool8 DecodePs2VagData(const uint8 *vagData, uint32 vagBytes, int16 *pcmData, uint32 pcmCapacityBytes,
	uint32 &pcmBytes, uint32 &loopStartBytes, int32 &loopEndBytes)
{
	pcmBytes = 0;
	loopStartBytes = 0;
	loopEndBytes = -1;
	if (vagData == NULL || pcmData == NULL || vagBytes == 0 || (vagBytes % PS2_SFX_VAG_FRAME_SIZE) != 0)
		return FALSE;

	CVagDecoder decoder;
	int16 decoded[PS2_SFX_VAG_SAMPLES_PER_FRAME];
	uint32 frameCount = vagBytes / PS2_SFX_VAG_FRAME_SIZE;
	uint32 framesDecoded = 0;
	bool8 terminalFound = FALSE;
	bool8 loopStartFound = FALSE;
	for (uint32 frameIndex = 0; frameIndex < frameCount; frameIndex++) {
		const uint8 *frame = vagData + frameIndex * PS2_SFX_VAG_FRAME_SIZE;
		if (!IsSupportedPs2MissionVagFlags(frame[1]) || !IsSupportedPs2VagPredictor(frame[0]))
			return FALSE;
		uint32 nextBytes = (framesDecoded + 1U) * PS2_SFX_VAG_SAMPLES_PER_FRAME * sizeof(int16);
		if (nextBytes > pcmCapacityBytes)
			return FALSE;

		if ((frame[1] & 0x04U) != 0) {
			loopStartFound = TRUE;
			loopStartBytes = framesDecoded * PS2_SFX_VAG_SAMPLES_PER_FRAME * sizeof(int16);
		}
		decoder.DecodeLineIgnoringFlags(frame, decoded);
		memcpy(pcmData + framesDecoded * PS2_SFX_VAG_SAMPLES_PER_FRAME,
		       decoded, sizeof(decoded));
		framesDecoded++;
		if (IsPs2VagEndFrame(frame[1])) {
			terminalFound = TRUE;
			if (loopStartFound && (frame[1] & 0x02U) != 0)
				loopEndBytes = int32(nextBytes);
			else
				loopStartBytes = 0;
			break;
		}
	}
	if (!terminalFound)
		return FALSE;
	pcmBytes = framesDecoded * PS2_SFX_VAG_SAMPLES_PER_FRAME * sizeof(int16);
	return pcmBytes != 0;
}

static bool8 ResolvePs2SampleAddress(uint32 nSfx, const int16 *&sampleData, uint32 &sampleCount, uint32 &sampleBytes)
{
	sampleData = NULL;
	sampleCount = 0;
	sampleBytes = 0;
	if (nSfx >= TOTAL_AUDIO_SAMPLES || !gPs2SfxSdtValid[nSfx])
		return FALSE;

	if (IsPs2PedComment(nSfx)) {
		int32 pedSlot = SampleManager._GetPedCommentSlot(nSfx);
		if (pedSlot < 0 || pedSlot >= MAX_PEDSFX || gPedSlotSfxAddr[pedSlot] == NULL || gPedSlotSizeBytes[pedSlot] == 0)
			return FALSE;
		sampleData = (const int16*)gPedSlotSfxAddr[pedSlot];
		sampleBytes = gPedSlotSizeBytes[pedSlot];
	} else {
		if (gPs2SfxDecoded[nSfx] == NULL || gPs2SfxDecodedBytes[nSfx] == 0)
			return FALSE;
		sampleData = gPs2SfxDecoded[nSfx];
		sampleBytes = gPs2SfxDecodedBytes[nSfx];
	}
	sampleCount = sampleBytes / sizeof(int16);
	return sampleCount != 0;
}

static bool8 InitialisePs2SampleBanks(tSample *samples)
{
	if (samples == NULL)
		return FALSE;
	memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
	memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
	memset(gPs2SfxDecoded, 0, sizeof(gPs2SfxDecoded));
	memset(gPs2SfxDecodedBytes, 0, sizeof(gPs2SfxDecodedBytes));
	gPs2SfxDecodedArena = NULL;
	gPs2SfxDecodedArenaBytes = 0;

	for (uint32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		samples[i].nOffset = 0;
		samples[i].nSize = 0;
		samples[i].nFrequency = 0;
		samples[i].nLoopStart = 0;
		samples[i].nLoopEnd = -1;
	}

	uint32 requiredPedSlotBytes = 0;
	uint32 predecodedCount = 0;
	uint64 predecodedArenaBytes = 0;
	uint32 maxPredecodeVagBytes = 0;
	bool8 bankHasPredecodedSamples[PS2_SFX_BANK_COUNT];
	memset(bankHasPredecodedSamples, 0, sizeof(bankHasPredecodedSamples));
	for (uint32 bank = 0; bank < PS2_SFX_BANK_COUNT; bank++) {
		char sdtPath[PS2_SFX_BANK_PATH_CAPACITY];
		if (!BuildPs2SfxPath(bank, "SDT", sdtPath, sizeof(sdtPath)))
			return FALSE;

		FILE *sdtFile = fopen(sdtPath, "rb");
		if (sdtFile == NULL) {
			printf("[GC-AUDIO] ERROR: incomplete PS2 SFX bank %u (%s)\n", bank, sdtPath);
			return FALSE;
		}
		if (fseek(sdtFile, 0, SEEK_END) != 0) {
			fclose(sdtFile);
			return FALSE;
		}
		long sdtBytes = ftell(sdtFile);
		if (sdtBytes <= 0 || (sdtBytes % PS2_SFX_SDT_RECORD_SIZE) != 0) {
			printf("[GC-AUDIO] ERROR: invalid PS2 SFX bank %u size sdt=%ld\n", bank, sdtBytes);
			fclose(sdtFile);
			return FALSE;
		}
		rewind(sdtFile);
		uint32 entryCount = uint32(sdtBytes) / PS2_SFX_SDT_RECORD_SIZE;
		uint8 record[PS2_SFX_SDT_RECORD_SIZE];
		for (uint32 local = 0; local < entryCount; local++) {
			if (fread(record, 1, sizeof(record), sdtFile) != sizeof(record)) {
				fclose(sdtFile);
				return FALSE;
			}
			uint32 nSfx = 0;
			if (!MapPs2RecordToSfx(uint8(bank), uint16(local), nSfx))
				continue;

			tPs2SfxSdtEntry entry;
			entry.rawOffset = ReadPs2SfxLe32(record + 0);
			entry.allocatedBytes = ReadPs2SfxLe32(record + 4);
			entry.sampleRate = ReadPs2SfxLe32(record + 8);
			entry.bank = uint8(bank);
			entry.localEntry = uint16(local);
			if (entry.allocatedBytes == 0 ||
			    (entry.rawOffset % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
			    (entry.allocatedBytes % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
			    entry.sampleRate == 0 ||
			    nSfx >= TOTAL_AUDIO_SAMPLES || gPs2SfxSdtValid[nSfx]) {
				printf("[GC-AUDIO] ERROR: invalid PS2 SFX mapping sample=%u bank=%u entry=%u\n",
				       nSfx, bank, local);
				fclose(sdtFile);
				return FALSE;
			}

			gPs2SfxSdt[nSfx] = entry;
			gPs2SfxSdtValid[nSfx] = TRUE;
			samples[nSfx].nOffset = entry.rawOffset;
			samples[nSfx].nSize = (entry.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE) *
			                      PS2_SFX_VAG_SAMPLES_PER_FRAME * sizeof(int16);
			samples[nSfx].nFrequency = entry.sampleRate;
			if (IsPs2PedComment(nSfx)) {
				if (samples[nSfx].nSize > requiredPedSlotBytes)
					requiredPedSlotBytes = samples[nSfx].nSize;
			} else {
				predecodedCount++;
				predecodedArenaBytes += AlignUp32(samples[nSfx].nSize);
				if (predecodedArenaBytes > 0xFFFFFFFFULL) {
					printf("[GC-AUDIO] ERROR: PS2 SFX arena size overflow\n");
					fclose(sdtFile);
					return FALSE;
				}
				if (entry.allocatedBytes > maxPredecodeVagBytes)
					maxPredecodeVagBytes = entry.allocatedBytes;
				bankHasPredecodedSamples[bank] = TRUE;
			}
		}
		fclose(sdtFile);
	}

	for (uint32 nSfx = 0; nSfx < TOTAL_AUDIO_SAMPLES; nSfx++) {
		uint8 bank = 0;
		uint16 localEntry = 0;
		if (MapSfxToPs2Record(nSfx, bank, localEntry) && !gPs2SfxSdtValid[nSfx]) {
			printf("[GC-AUDIO] ERROR: missing PS2 SFX mapping sample=%u bank=%u entry=%u\n",
			       nSfx, uint32(bank), uint32(localEntry));
			return FALSE;
		}
	}

	if (requiredPedSlotBytes == 0 || predecodedArenaBytes == 0 || maxPredecodeVagBytes == 0) {
		FreeSampleBankMemory();
		memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
		memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
		return FALSE;
	}

	uint32 decodedArenaBytes = uint32(predecodedArenaBytes);
	gPs2SfxDecodedArena = AllocAlignedAudioBufferStrict(decodedArenaBytes);
	if (gPs2SfxDecodedArena == NULL) {
		printf("[GC-AUDIO] ERROR: strict MEM2 alloc failed for PS2 SFX arena (%u bytes)\n", decodedArenaBytes);
		FreeSampleBankMemory();
		memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
		memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
		return FALSE;
	}
	gPs2SfxDecodedArenaBytes = decodedArenaBytes;
	memset(gPs2SfxDecodedArena, 0, gPs2SfxDecodedArenaBytes);

	uint32 arenaOffset = 0;
	for (uint32 nSfx = 0; nSfx < TOTAL_AUDIO_SAMPLES; nSfx++) {
		if (!gPs2SfxSdtValid[nSfx] || IsPs2PedComment(nSfx))
			continue;
		uint32 sampleArenaBytes = AlignUp32(samples[nSfx].nSize);
		if (sampleArenaBytes > gPs2SfxDecodedArenaBytes ||
		    arenaOffset > gPs2SfxDecodedArenaBytes - sampleArenaBytes) {
			printf("[GC-AUDIO] ERROR: PS2 SFX arena layout overflow sample=%u offset=%u size=%u arena=%u\n",
			       nSfx, arenaOffset, sampleArenaBytes, gPs2SfxDecodedArenaBytes);
			FreeSampleBankMemory();
			memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
			memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
			return FALSE;
		}
		gPs2SfxDecoded[nSfx] = (int16*)(gPs2SfxDecodedArena + arenaOffset);
		arenaOffset += sampleArenaBytes;
	}
	if (arenaOffset != gPs2SfxDecodedArenaBytes) {
		printf("[GC-AUDIO] ERROR: PS2 SFX arena layout mismatch used=%u arena=%u\n",
		       arenaOffset, gPs2SfxDecodedArenaBytes);
		FreeSampleBankMemory();
		memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
		memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
		return FALSE;
	}

	uint8 *vagData = new uint8[maxPredecodeVagBytes];
	if (vagData == NULL) {
		FreeSampleBankMemory();
		memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
		memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
		return FALSE;
	}

	for (uint32 bank = 0; bank < PS2_SFX_BANK_COUNT; bank++) {
		if (!bankHasPredecodedSamples[bank])
			continue;

		char rawPath[PS2_SFX_BANK_PATH_CAPACITY];
		if (!BuildPs2SfxPath(bank, "RAW", rawPath, sizeof(rawPath))) {
			delete[] vagData;
			FreeSampleBankMemory();
			memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
			memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
			return FALSE;
		}

		FILE *rawFile = fopen(rawPath, "rb");
		if (rawFile == NULL) {
			printf("[GC-AUDIO] ERROR: incomplete PS2 SFX RAW bank %u (%s)\n", bank, rawPath);
			delete[] vagData;
			FreeSampleBankMemory();
			memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
			memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
			return FALSE;
		}
		if (fseek(rawFile, 0, SEEK_END) != 0) {
			fclose(rawFile);
			delete[] vagData;
			FreeSampleBankMemory();
			memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
			memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
			return FALSE;
		}
		long rawBytes = ftell(rawFile);
		if (rawBytes <= 0) {
			printf("[GC-AUDIO] ERROR: invalid PS2 SFX RAW bank %u size=%ld\n", bank, rawBytes);
			fclose(rawFile);
			delete[] vagData;
			FreeSampleBankMemory();
			memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
			memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
			return FALSE;
		}

		for (uint32 nSfx = 0; nSfx < TOTAL_AUDIO_SAMPLES; nSfx++) {
			if (!gPs2SfxSdtValid[nSfx] || IsPs2PedComment(nSfx))
				continue;

			const tPs2SfxSdtEntry &entry = gPs2SfxSdt[nSfx];
			if (entry.bank != bank)
				continue;

			uint64 sliceEnd = uint64(entry.rawOffset) + entry.allocatedBytes;
			if (sliceEnd > uint64(rawBytes) ||
			    fseek(rawFile, long(entry.rawOffset), SEEK_SET) != 0 ||
			    fread(vagData, 1, entry.allocatedBytes, rawFile) != entry.allocatedBytes) {
				printf("[GC-AUDIO] ERROR: short PS2 SFX read sample=%u bank=%u entry=%u\n",
				       nSfx, bank, uint32(entry.localEntry));
				fclose(rawFile);
				delete[] vagData;
				FreeSampleBankMemory();
				memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
				memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
				return FALSE;
			}

			uint32 decodedBytes = 0;
			uint32 loopStartBytes = 0;
			int32 loopEndBytes = -1;
			uint32 pcmCapacityBytes = samples[nSfx].nSize;
			if (gPs2SfxDecoded[nSfx] == NULL ||
			    !DecodePs2VagData(vagData, entry.allocatedBytes, gPs2SfxDecoded[nSfx], pcmCapacityBytes,
			                      decodedBytes, loopStartBytes, loopEndBytes)) {
				printf("[GC-AUDIO] ERROR: invalid PS2 VAG sample=%u bank=%u entry=%u\n",
				       nSfx, bank, uint32(entry.localEntry));
				fclose(rawFile);
				delete[] vagData;
				FreeSampleBankMemory();
				memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
				memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
				return FALSE;
			}

			gPs2SfxDecodedBytes[nSfx] = decodedBytes;
			samples[nSfx].nSize = decodedBytes;
			samples[nSfx].nLoopStart = loopStartBytes;
			samples[nSfx].nLoopEnd = loopEndBytes;
		}
		fclose(rawFile);
	}

	delete[] vagData;

	gPedSlotCapacityBytes = requiredPedSlotBytes;
	gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = AllocAlignedAudioBuffer(gPedSlotCapacityBytes * MAX_PEDSFX);
	if (gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] == NULL) {
		FreeSampleBankMemory();
		memset(gPs2SfxSdt, 0, sizeof(gPs2SfxSdt));
		memset(gPs2SfxSdtValid, 0, sizeof(gPs2SfxSdtValid));
		return FALSE;
	}
	memset(gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS], 0, gPedSlotCapacityBytes * MAX_PEDSFX);
	gSampleBankLoaded[SFX_BANK_0] = TRUE;
	gSampleBankLoaded[SFX_BANK_PED_COMMENTS] = TRUE;
	printf("[GC-AUDIO] SFX backend: PS2 VAG (%u predecoded SFX, arena=%u bytes, ped slot=%u bytes, no PC fallback)\n",
	       predecodedCount, gPs2SfxDecodedArenaBytes, gPedSlotCapacityBytes);
	return TRUE;
}

static inline uint8 ClampVolume127(uint32 value)
{
	return uint8(Min(value, (uint32)MAX_VOLUME));
}

static inline uint8 ClampPan127(uint32 value)
{
	return uint8(Min(value, (uint32)127));
}

static void UpdateSampleChannelVolume(tGcSampleChannel &channel, uint8 effectsVolume, uint8 effectsFadeVolume)
{
	uint32 volume = uint32(effectsFadeVolume) * channel.requestedVolume * effectsVolume >> 14;
	channel.volume = ClampVolume127(volume);
	channel.emittingVolume = float(channel.volume);
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
	if (!channel.active || channel.paused || channel.sampleData == NULL || channel.sampleCount == 0 || channel.stepFixed == 0)
		return false;

	const bool looping = channel.loopCount == 0 || channel.loopCount > 1;
	uint32 loopStart = BytesToSampleIndex(channel.loopStartBytes);
	if (loopStart >= channel.sampleCount)
		loopStart = 0;

	uint32 loopEnd = channel.sampleCount;
	int32 loopEndSample = BytesToLoopEndSampleIndex(channel.loopEndBytes);
	if (loopEndSample > int32(loopStart) && uint32(loopEndSample) <= channel.sampleCount)
		loopEnd = uint32(loopEndSample);

	uint32 sampleIndex = channel.cursorFixed >> GC_SFX_RESAMPLE_FRAC_BITS;
	if (sampleIndex >= channel.sampleCount || (looping && sampleIndex >= loopEnd)) {
		if (!looping) {
			channel.active = FALSE;
			return false;
		}
		if (channel.loopCount > 1)
			channel.loopCount--;
		channel.cursorFixed = loopStart << GC_SFX_RESAMPLE_FRAC_BITS;
		sampleIndex = loopStart;
	}

	uint32 fractional = channel.cursorFixed & GC_SFX_RESAMPLE_FRAC_MASK;
	uint32 nextIndex = sampleIndex + 1;
	if (looping && nextIndex >= loopEnd)
		nextIndex = loopStart;
	if (nextIndex >= channel.sampleCount)
		nextIndex = sampleIndex;

	int32 currentSample = channel.sampleData[sampleIndex];
	int32 nextSample = channel.sampleData[nextIndex];
	int32 interpolated = currentSample + ((nextSample - currentSample) * int32(fractional) >> GC_SFX_RESAMPLE_FRAC_BITS);
	int16 mono = int16(interpolated);
	outL = mono;
	outR = mono;

	channel.cursorFixed += channel.stepFixed;

	if (looping && (channel.cursorFixed >> GC_SFX_RESAMPLE_FRAC_BITS) >= loopEnd) {
		if (channel.loopCount > 1)
			channel.loopCount--;
		channel.cursorFixed = loopStart << GC_SFX_RESAMPLE_FRAC_BITS;
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

		uint32 edgeGainQ16 = 1U << 16;
		if (channel.edgeRampFrames != 0 && channel.renderedOutputFrames < channel.edgeRampFrames)
			edgeGainQ16 = ((channel.renderedOutputFrames + 1) << 16) / channel.edgeRampFrames;

		if (channel.edgeRampFrames != 0 && channel.loopCount == 1) {
			uint64 sampleEndFixed = uint64(channel.sampleCount) << GC_SFX_RESAMPLE_FRAC_BITS;
			uint64 remainingFixed = channel.cursorFixed < sampleEndFixed ? sampleEndFixed - channel.cursorFixed : 0;
			/* Most of a one-shot sample is far from its end ramp. Avoid the
			 * 64-bit division until the remaining fixed-point distance is within
			 * the ramp window. */
			if (remainingFixed <= uint64(channel.edgeRampFrames) * channel.stepFixed) {
				uint64 remainingFrames = (remainingFixed + channel.stepFixed - 1) / channel.stepFixed;
				uint64 currentAndRemainingFrames = remainingFrames + 1;
				if (currentAndRemainingFrames <= channel.edgeRampFrames) {
					uint32 endGainQ16 = uint32((currentAndRemainingFrames << 16) / channel.edgeRampFrames);
					edgeGainQ16 = Min(edgeGainQ16, endGainQ16);
				}
			}
		}

		uint32 outIndex = framesRendered * 2;
		int32 rampedLeft = edgeGainQ16 == (1U << 16) ? int32(left) : int32((int64(left) * edgeGainQ16) >> 16);
		int32 rampedRight = edgeGainQ16 == (1U << 16) ? int32(right) : int32((int64(right) * edgeGainQ16) >> 16);
		mixBuffer[outIndex + 0] += (rampedLeft * int32(leftGain)) / MAX_VOLUME;
		mixBuffer[outIndex + 1] += (rampedRight * int32(rightGain)) / MAX_VOLUME;
		channel.renderedOutputFrames++;
		framesRendered++;
	}

	return framesRendered;
}

static const char *ResolveDirectPs2RadioPath(tTrack track)
{
	static bool8 s_scanned[STREAMED_SOUND_RADIO_WAVE + 1];
	static const char *s_cachedPath[STREAMED_SOUND_RADIO_WAVE + 1];

	static const char *const s_wildCandidates[] = {
		"AUDIO\\MUSIC\\wild.idsp",
	};
	static const char *const s_flashCandidates[] = {
		"AUDIO\\MUSIC\\Flash.idsp",
	};
	static const char *const s_kchatCandidates[] = {
		"AUDIO\\MUSIC\\KCHAT.idsp",
		"AUDIO\\MUSIC\\Kchat.idsp",
	};
	static const char *const s_feverCandidates[] = {
		"AUDIO\\MUSIC\\fever.idsp",
	};
	static const char *const s_vrockCandidates[] = {
		"AUDIO\\MUSIC\\vrock.idsp",
	};
	static const char *const s_vcprCandidates[] = {
		"AUDIO\\MUSIC\\VCPR.idsp",
		"AUDIO\\MUSIC\\vcpr.idsp",
	};
	static const char *const s_espantCandidates[] = {
		"AUDIO\\MUSIC\\espant.idsp",
	};
	static const char *const s_emotionCandidates[] = {
		"AUDIO\\MUSIC\\emotion.idsp",
	};
	static const char *const s_waveCandidates[] = {
		"AUDIO\\MUSIC\\wave.idsp",
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
		GC_AUDIO_LOG("[GC-AUDIO] radio %s -> %s\n", GetRadioTrackLabel(track), candidates[i]);
		break;
	}

	if (s_cachedPath[track] == NULL)
		GC_AUDIO_LOG("[GC-AUDIO] radio %s missing direct IDSP candidates\n", GetRadioTrackLabel(track));

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

static uint32 ReadLE32(FILE *file)
{
	uint8 bytes[4];
	if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
		return 0;
	return (uint32(bytes[3]) << 24) | (uint32(bytes[2]) << 16) | (uint32(bytes[1]) << 8) | uint32(bytes[0]);
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
	uint32 m_CombinedBlockSize;

	int16 m_Coefs[2][16];
	tAdpcmHist m_InitialHist[2];
	tAdpcmHist m_CurrentHist[2];
	uint8 *m_pBlockData[2];
	uint8 *m_pReadCache;
	uint32 m_ReadCacheFirstBlock;
	uint32 m_ReadCacheBlockCount;
	uint32 m_ReadCacheCapacityBlocks;

	uint32 m_CurrentSample;
	uint32 m_CurrentBlock;
	uint32 m_CurrentFrameInBlock;
	bool m_bBlockRead;

	int16 m_PendingSamples[GC_ADPCM_SAMPLES_PER_FRAME * 2];
	uint32 m_PendingCount;
	uint32 m_PendingPos;

	bool m_UseDsp;
	uint8 *m_pDspFrames[2];
	int16 *m_pDspPendingSamples;
	uint32 m_DspPendingCount;
	uint32 m_DspPendingPos;
	bool m_bSeeking;

	bool FillReadCache(uint32 blockIndex)
	{
		if (m_pReadCache == NULL || m_ReadCacheCapacityBlocks == 0 || blockIndex >= m_BlockCount)
			return false;

		/* IDSP audio begins after a 0x100-byte header.  Align steady-state
		 * cache windows to DVD sectors so the Wii VFS can use direct DMA instead
		 * of allocating a bounce buffer for every radio refill. */
		uint32 cacheFirstBlock = blockIndex;
		if (m_CombinedBlockSize != 0 && GC_IDSP_SECTOR_SIZE % m_CombinedBlockSize == 0) {
			uint32 blocksPerSector = GC_IDSP_SECTOR_SIZE / m_CombinedBlockSize;
			uint32 firstAlignedBlock = (GC_IDSP_SECTOR_SIZE - (m_AudioDataOffset % GC_IDSP_SECTOR_SIZE)) % GC_IDSP_SECTOR_SIZE;
			if (firstAlignedBlock % m_CombinedBlockSize == 0) {
				firstAlignedBlock /= m_CombinedBlockSize;
				if (blockIndex >= firstAlignedBlock)
					cacheFirstBlock = firstAlignedBlock + ((blockIndex - firstAlignedBlock) / blocksPerSector) * blocksPerSector;
			}
		}

		uint32 blocksToRead = Min(m_ReadCacheCapacityBlocks, m_BlockCount - cacheFirstBlock);
		if (blockIndex < cacheFirstBlock)
			blocksToRead = Min(blocksToRead, cacheFirstBlock - blockIndex);
		uint32 bytesToRead = blocksToRead * m_CombinedBlockSize;
		uint64 offset = uint64(m_AudioDataOffset) + uint64(cacheFirstBlock) * m_CombinedBlockSize;
		uint64 dataEnd = uint64(m_AudioDataOffset) + uint64(m_AudioDataSize) * m_ChannelCount;
		if (offset + bytesToRead > dataEnd || fseek(m_pFile, long(offset), SEEK_SET) != 0)
			return false;
		if (fread(m_pReadCache, 1, bytesToRead, m_pFile) != bytesToRead)
			return false;

		m_ReadCacheFirstBlock = cacheFirstBlock;
		m_ReadCacheBlockCount = blocksToRead;
		return true;
	}

	bool ReadBlock(uint32 blockIndex)
	{
		if (m_ChannelCount == 0 || blockIndex >= m_BlockCount)
			return false;

		bool cached = blockIndex >= m_ReadCacheFirstBlock &&
		              blockIndex - m_ReadCacheFirstBlock < m_ReadCacheBlockCount;
		if (!cached && !FillReadCache(blockIndex))
			return false;

		uint32 cacheOffset = (blockIndex - m_ReadCacheFirstBlock) * m_CombinedBlockSize;
		for (uint32 ch = 0; ch < m_ChannelCount && ch < 2; ch++)
			memcpy(m_pBlockData[ch], m_pReadCache + cacheOffset + ch * m_InterleaveSize, m_InterleaveSize);

		m_bBlockRead = true;
		return true;
	}

	void ResetPending()
	{
		m_PendingCount = 0;
		m_PendingPos = 0;
	}

	void ResetDspPending()
	{
		m_DspPendingCount = 0;
		m_DspPendingPos = 0;
	}

	bool CanStartDsp() const
	{
		return !gIdspDspOutputRejected && !m_bSeeking && m_pDspFrames[0] != NULL && m_pDspFrames[1] != NULL &&
		       m_pDspPendingSamples != NULL && m_PendingCount == 0 && m_CurrentFrameInBlock == 0 &&
		       !m_bBlockRead && m_CurrentSample < m_SampleCount && CGcIdspDspTask::IsAvailable();
	}

	void LogDspMismatchDetails(const GcIdspBlockDecodeRequest &request,
	                           uint32 badSample, uint32 badChannel,
	                           int16 expected, int16 actual) const
	{
		uint32 previewSamples = Min(request.sampleCount, uint32(4));
		int16 cpuPreview[2][4];
		int16 dspPreview[2][4];
		int16 swappedPreview[2][2];
		memset(cpuPreview, 0, sizeof(cpuPreview));
		memset(dspPreview, 0, sizeof(dspPreview));
		memset(swappedPreview, 0, sizeof(swappedPreview));

		tAdpcmHist cpuHistory[2] = {
			{ request.initialHistory[0].hist1, request.initialHistory[0].hist2 },
			{ request.initialHistory[1].hist1, request.initialHistory[1].hist2 }
		};
		uint32 produced = 0;
		uint32 previewProduced = 0;
		int16 cpuSamples[2][GC_ADPCM_SAMPLES_PER_FRAME];
		while (produced < request.sampleCount && previewProduced < previewSamples) {
			uint32 frame = produced / GC_ADPCM_SAMPLES_PER_FRAME;
			uint32 samplesThisFrame = Min(GC_ADPCM_SAMPLES_PER_FRAME, request.sampleCount - produced);
			for (uint32 ch = 0; ch < request.channelCount; ch++) {
				DecodeGcAdpcmFrame(request.channelFrames[ch] + frame * GC_ADPCM_FRAME_SIZE,
				                   request.coefficients[ch], cpuHistory[ch], cpuSamples[ch], samplesThisFrame);
			}
			uint32 copyCount = Min(samplesThisFrame, previewSamples - previewProduced);
			for (uint32 sample = 0; sample < copyCount; sample++) {
				cpuPreview[0][previewProduced + sample] = cpuSamples[0][sample];
				cpuPreview[1][previewProduced + sample] =
				    cpuSamples[request.channelCount > 1 ? 1 : 0][sample];
				dspPreview[0][previewProduced + sample] = request.output[(previewProduced + sample) * 2 + 0];
				dspPreview[1][previewProduced + sample] = request.output[(previewProduced + sample) * 2 + 1];
			}
			produced += samplesThisFrame;
			previewProduced += copyCount;
		}

		if (request.sampleCount != 0) {
			uint32 samplesThisFrame = Min(GC_ADPCM_SAMPLES_PER_FRAME, request.sampleCount);
			tAdpcmHist swappedHistory[2] = {
				{ request.initialHistory[0].hist2, request.initialHistory[0].hist1 },
				{ request.initialHistory[1].hist2, request.initialHistory[1].hist1 }
			};
			int16 swappedSamples[2][GC_ADPCM_SAMPLES_PER_FRAME];
			for (uint32 ch = 0; ch < request.channelCount; ch++) {
				DecodeGcAdpcmFrame(request.channelFrames[ch], request.coefficients[ch],
				                   swappedHistory[ch], swappedSamples[ch], samplesThisFrame);
			}
			swappedPreview[0][0] = swappedSamples[0][0];
			swappedPreview[1][0] = swappedSamples[request.channelCount > 1 ? 1 : 0][0];
			if (samplesThisFrame > 1) {
				swappedPreview[0][1] = swappedSamples[0][1];
				swappedPreview[1][1] = swappedSamples[request.channelCount > 1 ? 1 : 0][1];
			}
		}

		int shiftSample0L = request.sampleCount > 1 ? request.output[2] : 0;
		int shiftSample0R = request.sampleCount > 1 ? request.output[3] : 0;
		printf("[GC-AUDIO] IDSP mismatch detail bad=%u ch=%u exp=%d got=%d initL=%d,%d initR=%d,%d "
		       "coefL=%d,%d,%d,%d "
		       "curL=%d,%d curR=%d,%d frame0=%02x%02x%02x%02x "
		       "cpu=%d,%d|%d,%d|%d,%d|%d,%d dsp=%d,%d|%d,%d|%d,%d|%d,%d shift1=%d,%d swap=%d,%d|%d,%d\n",
		       badSample, badChannel, expected, actual,
		       request.initialHistory[0].hist1, request.initialHistory[0].hist2,
		       request.initialHistory[1].hist1, request.initialHistory[1].hist2,
		       request.coefficients[0][0], request.coefficients[0][1],
		       request.coefficients[0][2], request.coefficients[0][3],
		       request.currentHistory[0].hist1, request.currentHistory[0].hist2,
		       request.currentHistory[1].hist1, request.currentHistory[1].hist2,
		       request.channelFrames[0][0], request.channelFrames[0][1],
		       request.channelFrames[0][2], request.channelFrames[0][3],
		       cpuPreview[0][0], cpuPreview[1][0], cpuPreview[0][1], cpuPreview[1][1],
		       cpuPreview[0][2], cpuPreview[1][2], cpuPreview[0][3], cpuPreview[1][3],
		       dspPreview[0][0], dspPreview[1][0], dspPreview[0][1], dspPreview[1][1],
		       dspPreview[0][2], dspPreview[1][2], dspPreview[0][3], dspPreview[1][3],
		       shiftSample0L, shiftSample0R,
		       swappedPreview[0][0], swappedPreview[1][0], swappedPreview[0][1], swappedPreview[1][1]);
	}

	bool VerifyDspOutput(const GcIdspBlockDecodeRequest &request,
	                     uint32 &badSample, uint32 &badChannel,
	                     int16 &expected, int16 &actual) const
	{
		tAdpcmHist cpuHistory[2] = {
			{ request.initialHistory[0].hist1, request.initialHistory[0].hist2 },
			{ request.initialHistory[1].hist1, request.initialHistory[1].hist2 }
		};
		uint32 produced = 0;
		int16 cpuSamples[2][GC_ADPCM_SAMPLES_PER_FRAME];
		for (uint32 frame = 0; frame < request.frameCount && produced < request.sampleCount; frame++) {
			uint32 samplesThisFrame = Min(GC_ADPCM_SAMPLES_PER_FRAME, request.sampleCount - produced);
			for (uint32 ch = 0; ch < request.channelCount; ch++) {
				DecodeGcAdpcmFrame(request.channelFrames[ch] + frame * GC_ADPCM_FRAME_SIZE,
				                   request.coefficients[ch], cpuHistory[ch], cpuSamples[ch], samplesThisFrame);
			}
			for (uint32 sample = 0; sample < samplesThisFrame; sample++) {
				for (uint32 ch = 0; ch < 2; ch++) {
					uint32 sourceChannel = request.channelCount > 1 ? ch : 0;
					int16 cpuValue = cpuSamples[sourceChannel][sample];
					int16 dspValue = request.output[(produced + sample) * 2 + ch];
					if (cpuValue != dspValue) {
						badSample = produced + sample;
						badChannel = ch;
						expected = cpuValue;
						actual = dspValue;
						return false;
					}
				}
			}
			produced += samplesThisFrame;
		}

		for (uint32 ch = 0; ch < request.channelCount; ch++) {
			if (request.currentHistory[ch].hist1 != cpuHistory[ch].hist1) {
				badSample = UINT32_MAX;
				badChannel = ch;
				expected = cpuHistory[ch].hist1;
				actual = request.currentHistory[ch].hist1;
				return false;
			}
			if (request.currentHistory[ch].hist2 != cpuHistory[ch].hist2) {
				badSample = UINT32_MAX;
				badChannel = ch;
				expected = cpuHistory[ch].hist2;
				actual = request.currentHistory[ch].hist2;
				return false;
			}
		}
		return true;
	}

	uint32 DecodeFramesDsp(int16 *buffer, uint32 maxFrames)
	{
		if (!m_UseDsp || m_pDspPendingSamples == NULL)
			return 0;

		uint32 framesWritten = 0;
		while (framesWritten < maxFrames && m_CurrentSample < m_SampleCount) {
			if (m_DspPendingPos < m_DspPendingCount) {
				uint32 available = m_DspPendingCount - m_DspPendingPos;
				uint32 toCopy = Min(available, maxFrames - framesWritten);
				memcpy(buffer + framesWritten * 2,
				       m_pDspPendingSamples + m_DspPendingPos * 2,
				       sizeof(int16) * toCopy * 2);
				m_DspPendingPos += toCopy;
				framesWritten += toCopy;
				m_CurrentSample += toCopy;
				if (m_DspPendingPos >= m_DspPendingCount)
					ResetDspPending();
				continue;
			}

			/* The DSP path submits complete IDSP blocks so its accelerator history
			 * can be returned once per batch.  Any non-boundary state is decoded by
			 * the existing CPU path, which is also used for seek warm-up. */
			uint32 framesPerBlock = m_InterleaveSize / GC_ADPCM_FRAME_SIZE;
			if (m_CurrentFrameInBlock != 0 || m_bBlockRead || framesPerBlock == 0 || m_CurrentBlock >= m_BlockCount) {
				m_UseDsp = false;
				break;
			}

			uint32 remainingBlocks = m_BlockCount - m_CurrentBlock;
			uint32 batchFrames = Min(uint32(CGcIdspDspTask::MAX_FRAME_COUNT), remainingBlocks * framesPerBlock);
			batchFrames -= batchFrames % framesPerBlock;
			if (batchFrames == 0) {
				m_UseDsp = false;
				break;
			}

			uint32 batchBlocks = batchFrames / framesPerBlock;
			for (uint32 block = 0; block < batchBlocks; block++) {
				if (!ReadBlock(m_CurrentBlock + block)) {
					m_UseDsp = false;
					m_bBlockRead = false;
					ResetDspPending();
					break;
				}
				memcpy(m_pDspFrames[0] + block * m_InterleaveSize, m_pBlockData[0], m_InterleaveSize);
				if (m_ChannelCount > 1)
					memcpy(m_pDspFrames[1] + block * m_InterleaveSize, m_pBlockData[1], m_InterleaveSize);
			}
			if (!m_UseDsp)
				break;

			GcIdspBlockDecodeRequest request;
			memset(&request, 0, sizeof(request));
			request.channelFrames[0] = m_pDspFrames[0];
			request.channelFrames[1] = m_ChannelCount > 1 ? m_pDspFrames[1] : m_pDspFrames[0];
			request.coefficients[0] = m_Coefs[0];
			request.coefficients[1] = m_ChannelCount > 1 ? m_Coefs[1] : m_Coefs[0];
			request.initialHistory[0].hist1 = m_CurrentHist[0].hist1;
			request.initialHistory[0].hist2 = m_CurrentHist[0].hist2;
			request.initialHistory[1].hist1 = m_ChannelCount > 1 ? m_CurrentHist[1].hist1 : m_CurrentHist[0].hist1;
			request.initialHistory[1].hist2 = m_ChannelCount > 1 ? m_CurrentHist[1].hist2 : m_CurrentHist[0].hist2;
			request.channelCount = m_ChannelCount;
			request.frameCount = batchFrames;
			request.sampleCount = Min(batchFrames * GC_ADPCM_SAMPLES_PER_FRAME, m_SampleCount - m_CurrentSample);
			request.output = m_pDspPendingSamples;
			request.outputCapacityFrames = CGcIdspDspTask::MAX_SAMPLE_COUNT;

			if (!CGcIdspDspTask::DecodeBlock(request)) {
				printf("[GC-AUDIO] WARN: IDSP DSP decode failed; stream reverting to CPU\n");
				m_UseDsp = false;
				m_bBlockRead = false;
				ResetDspPending();
				break;
			}
			if (!gIdspDspOutputValidated) {
				uint32 badSample = 0;
				uint32 badChannel = 0;
				int16 expected = 0;
				int16 actual = 0;
				if (!VerifyDspOutput(request, badSample, badChannel, expected, actual)) {
					LogDspMismatchDetails(request, badSample, badChannel, expected, actual);
					printf("[GC-AUDIO] WARN: IDSP DSP output mismatch sample=%u channel=%u expected=%d got=%d; using CPU\n",
					       badSample, badChannel, expected, actual);
					gIdspDspOutputRejected = TRUE;
					m_UseDsp = false;
					m_bBlockRead = false;
					ResetDspPending();
					break;
				}
				gIdspDspOutputValidated = TRUE;
				printf("[GC-AUDIO] IDSP DSP output verified frames=%u\n", request.outputFramesWritten);
			}

			m_CurrentHist[0].hist1 = request.currentHistory[0].hist1;
			m_CurrentHist[0].hist2 = request.currentHistory[0].hist2;
			if (m_ChannelCount > 1) {
				m_CurrentHist[1].hist1 = request.currentHistory[1].hist1;
				m_CurrentHist[1].hist2 = request.currentHistory[1].hist2;
			}
			m_CurrentBlock += batchBlocks;
			m_CurrentFrameInBlock = 0;
			m_bBlockRead = false;
			m_DspPendingCount = request.outputFramesWritten;
			m_DspPendingPos = 0;
		}

		return framesWritten;
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
		  m_CombinedBlockSize(0),
		  m_pReadCache(NULL),
		  m_ReadCacheFirstBlock(0),
		  m_ReadCacheBlockCount(0),
		  m_ReadCacheCapacityBlocks(0),
		  m_CurrentSample(0),
		  m_CurrentBlock(0),
		  m_CurrentFrameInBlock(0),
		  m_bBlockRead(false),
		  m_PendingCount(0),
		  m_PendingPos(0),
		  m_UseDsp(false),
		  m_pDspPendingSamples(NULL),
		  m_DspPendingCount(0),
		  m_DspPendingPos(0),
		  m_bSeeking(false)
	{
		m_pBlockData[0] = NULL;
		m_pBlockData[1] = NULL;
		m_pDspFrames[0] = NULL;
		m_pDspFrames[1] = NULL;
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

		if (m_ChannelCount == 0 || m_ChannelCount > 2 || m_SampleRate == 0 || m_SampleCount == 0 || m_InterleaveSize == 0 ||
		    m_InterleaveSize > UINT32_MAX / m_ChannelCount)
			return;

		m_AudioDataOffset = m_HeaderSize;
		m_BlockCount = m_AudioDataSize / m_InterleaveSize;
		m_BlockSamples = GcByteCountToSampleCount(m_InterleaveSize);
		m_CombinedBlockSize = m_InterleaveSize * m_ChannelCount;
		if (m_BlockCount == 0 || m_BlockSamples == 0)
			return;

		uint32 cacheSize = Max(GC_IDSP_READ_CACHE_SIZE, m_CombinedBlockSize);
		m_ReadCacheCapacityBlocks = cacheSize / m_CombinedBlockSize;
		cacheSize = m_ReadCacheCapacityBlocks * m_CombinedBlockSize;
		m_pReadCache = (uint8*)memalign(32, cacheSize);
		if (m_pReadCache == NULL)
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

		if (GC_IDSP_DSP_ENABLED && CGcIdspDspTask::IsAvailable()) {
			uint32 dspFrameBytes = CGcIdspDspTask::MAX_FRAME_COUNT * GC_ADPCM_FRAME_SIZE;
			m_pDspFrames[0] = (uint8*)memalign(32, dspFrameBytes);
			m_pDspFrames[1] = (uint8*)memalign(32, dspFrameBytes);
			m_pDspPendingSamples = (int16*)memalign(32, CGcIdspDspTask::MAX_SAMPLE_COUNT * 2 * sizeof(int16));
			if (m_pDspFrames[0] == NULL || m_pDspFrames[1] == NULL || m_pDspPendingSamples == NULL) {
				free(m_pDspFrames[0]);
				free(m_pDspFrames[1]);
				free(m_pDspPendingSamples);
				m_pDspFrames[0] = NULL;
				m_pDspFrames[1] = NULL;
				m_pDspPendingSamples = NULL;
			} else {
				m_UseDsp = true;
			}
		}
		m_bOpened = true;
		SeekMS(0);
	}

	~CIdspStreamDecoder()
	{
		if (m_pFile)
			fclose(m_pFile);
		free(m_pReadCache);
		for (uint32 ch = 0; ch < 2; ch++)
			delete[] m_pBlockData[ch];
		free(m_pDspFrames[0]);
		free(m_pDspFrames[1]);
		free(m_pDspPendingSamples);
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
		m_bSeeking = true;

		uint32 targetSample = MsToSamples(m_SampleRate, milliseconds);
		if (targetSample >= m_SampleCount)
			targetSample = 0;

		uint32 warmupBlocks = (m_SampleRate + m_BlockSamples - 1) / m_BlockSamples;
		uint32 targetBlock = targetSample / m_BlockSamples;
		uint32 decodeStartBlock = targetBlock > warmupBlocks ? targetBlock - warmupBlocks : 0;

		m_CurrentBlock = decodeStartBlock;
		m_CurrentFrameInBlock = 0;
		m_CurrentSample = decodeStartBlock * m_BlockSamples;
		m_bBlockRead = false;
		ResetPending();
		ResetDspPending();
		m_UseDsp = targetSample == 0 && !gIdspDspOutputRejected && m_pDspFrames[0] != NULL && m_pDspPendingSamples != NULL &&
			CGcIdspDspTask::IsAvailable();

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
		m_bSeeking = false;
	}

	uint32 DecodeFrames(int16 *buffer, uint32 maxFrames)
	{
		if (!IsOpened() || buffer == NULL || maxFrames == 0)
			return 0;

		uint32 framesWritten = 0;
		uint32 framesPerBlock = m_InterleaveSize / GC_ADPCM_FRAME_SIZE;
		int16 temp[2][GC_ADPCM_SAMPLES_PER_FRAME];

		while (framesWritten < maxFrames && m_CurrentSample < m_SampleCount) {
			if (!m_UseDsp && CanStartDsp())
				m_UseDsp = true;
			if (m_UseDsp) {
				uint32 dspFrames = DecodeFramesDsp(buffer + framesWritten * 2, maxFrames - framesWritten);
				framesWritten += dspFrames;
				if (dspFrames != 0 || m_UseDsp || m_CurrentSample >= m_SampleCount)
					return framesWritten;
			}

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
	bool8 openPending;
	bool8 active;
	bool8 paused;
	bool8 loop;
	bool8 effectFlag;
	uint8 requestedVolume;
	uint8 pan;
	uint32 mixVolume;
	uint32 lengthMs;
	tTrack currentTrack;
	tTrack pendingTrack;
	uint32 pendingStartPosMs;
	uint32 startPosMs;
	uint32 resampleStep;
	uint32 resampleFrac;
	bool8 sourceEnded;
	uint32 sourceFramesValid;
	uint32 sourceFramePos;
	uint8 decodeRetryCount;
	bool8 cutsceneMixLogged;
	int16 *sourceBuffer;

	void ResetRuntime()
	{
		openPending = FALSE;
		active = FALSE;
		paused = FALSE;
		sourceEnded = FALSE;
		sourceFramesValid = 0;
		sourceFramePos = 0;
		decodeRetryCount = 0;
		resampleStep = 0;
		resampleFrac = 0;
		currentTrack = NO_TRACK;
		pendingTrack = NO_TRACK;
		pendingStartPosMs = 0;
		startPosMs = 0;
		cutsceneMixLogged = FALSE;
	}
};

static tGcStreamSlot gStreamSlots[MAX_STREAMS];
static uint32 gStreamLength[TOTAL_STREAMED_SOUNDS];
static bool8 gStreamLoopedFlag[MAX_STREAMS];
static volatile uint32 gStreamPlaybackGeneration[MAX_STREAMS];
static volatile uint64 gStreamPlayedOutputFrames[MAX_STREAMS];
static volatile uint32 gSamplePlaybackGeneration[MAXCHANNELS + MAX2DCHANNELS];
static volatile uint32 gSamplePendingOutputFrames[MAXCHANNELS + MAX2DCHANNELS];
static volatile uint32 gDmaBufferStates[GC_DMA_BUFFER_COUNT];
static uint32 gDmaBufferSlotFrames[GC_DMA_BUFFER_COUNT][MAX_STREAMS];
static uint32 gDmaBufferSlotGeneration[GC_DMA_BUFFER_COUNT][MAX_STREAMS];
static uint32 gDmaBufferSampleFrames[GC_DMA_BUFFER_COUNT][MAXCHANNELS + MAX2DCHANNELS];
static uint32 gDmaBufferSampleGeneration[GC_DMA_BUFFER_COUNT][MAXCHANNELS + MAX2DCHANNELS];
static volatile int32 gPlayingDmaBuffer = -1;
static volatile int32 gSubmittedDmaBuffer = -1;
static bool8 gDmaActive = FALSE;
static volatile uint8 gDmaReadyQueue[GC_DMA_BUFFER_COUNT];
static volatile uint8 gDmaReadyHead = 0;
static volatile uint8 gDmaReadyTail = 0;
static volatile uint8 gDmaReadyCount = 0;
static volatile bool8 gDmaExpectingAudio = FALSE;
static volatile uint32 gDmaUnderrunCount = 0;
static volatile uint32 gDmaSilenceBufferCount = 0;
static volatile uint32 gDmaOutputBufferCount = 0;
static volatile uint32 gDmaMinimumReadyDepth = GC_DMA_BUFFER_COUNT;
static volatile uint32 gDmaBuffersFilled = 0;
static volatile uint32 gDmaMaximumFillTimeUs = 0;
static mutex_t gAudioStateMutex = LWP_MUTEX_NULL;
static mutex_t gAudioFillMutex = LWP_MUTEX_NULL;
static mutex_t gStreamDecodeMutex = LWP_MUTEX_NULL;
static sem_t gAudioProducerSemaphore = LWP_SEM_NULL;
static lwp_t gAudioProducerThread = LWP_THREAD_NULL;
static volatile bool8 gAudioProducerStop = FALSE;
static volatile bool8 gAudioProducerRunning = FALSE;
static volatile bool8 gAudioProducerWakeRequested = FALSE;
static volatile bool8 gAudioProducerFilling = FALSE;
static uint8 gAudioProducerStack[32 * 1024] ATTRIBUTE_ALIGN(32);
static sem_t gStreamDecoderSemaphore = LWP_SEM_NULL;
static lwp_t gStreamDecoderThread = LWP_THREAD_NULL;
static volatile bool8 gStreamDecoderStop = FALSE;
static volatile bool8 gStreamDecoderRunning = FALSE;
static uint8 gStreamDecoderStack[32 * 1024] ATTRIBUTE_ALIGN(32);
static int16 gStreamDecodeBuffer[GC_STREAM_DECODE_CHUNK_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static sem_t gPedCommentLoaderSemaphore = LWP_SEM_NULL;
static lwp_t gPedCommentLoaderThread = LWP_THREAD_NULL;
static volatile bool8 gPedCommentLoaderStop = FALSE;
static volatile bool8 gPedCommentLoaderRunning = FALSE;
static volatile uint32 gPedCommentPending = NO_SAMPLE;
static volatile uint32 gPedCommentLoading = NO_SAMPLE;
static uint32 gPedCommentPendingOffset = 0;
static uint32 gPedCommentPendingSize = 0;
static uint8 gPedSlotLoading[MAX_PEDSFX];
static uint8 gPedSlotClaimFrames[MAX_PEDSFX];
static uint8 *gPedCommentLoadBuffer = NULL;
static uint8 *gPedCommentVagBuffer = NULL;
static FILE *gPedCommentDataFile = NULL;
static uint8 gPedCommentLoaderStack[16 * 1024] ATTRIBUTE_ALIGN(32);
static int16 gDmaBuffers[GC_DMA_BUFFER_COUNT][GC_DMA_BUFFER_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int16 gDmaSampleOnlyBuffers[GC_DMA_BUFFER_COUNT][GC_DMA_BUFFER_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int16 gSilenceBuffer[GC_DMA_BUFFER_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int32 gMixBuffer[GC_DMA_BUFFER_FRAMES * 2];

static CGcStreamDecoder *gDeferredStreamDecoders[MAX_STREAMS * 4];
static uint32 gDeferredStreamDecoderCount = 0;

static void SignalAudioProducer();
static void SignalStreamDecoder();
static void SignalPedCommentLoader();

class cAudioStateLock
{
	bool m_locked;

public:
	cAudioStateLock() : m_locked(false)
	{
		if (gAudioStateMutex != LWP_MUTEX_NULL) {
			m_locked = LWP_MutexLock(gAudioStateMutex) == 0;
		}
	}

	~cAudioStateLock()
	{
		if (m_locked)
			LWP_MutexUnlock(gAudioStateMutex);
	}
};

class cAudioFillLock
{
	bool m_locked;

public:
	cAudioFillLock() : m_locked(false)
	{
		if (gAudioFillMutex != LWP_MUTEX_NULL)
			m_locked = LWP_MutexLock(gAudioFillMutex) == 0;
	}

	~cAudioFillLock()
	{
		if (m_locked)
			LWP_MutexUnlock(gAudioFillMutex);
	}
};

class cStreamDecodeLock
{
	bool m_locked;

public:
	cStreamDecodeLock() : m_locked(false)
	{
		if (gStreamDecodeMutex != LWP_MUTEX_NULL)
			m_locked = LWP_MutexLock(gStreamDecodeMutex) == 0;
	}

	~cStreamDecodeLock()
	{
		if (m_locked)
			LWP_MutexUnlock(gStreamDecodeMutex);
	}
};

static void ResetStreamPlaybackClock(uint32 streamIndex)
{
	u32 level;
	_CPU_ISR_Disable(level);
	gStreamPlaybackGeneration[streamIndex]++;
	gStreamPlayedOutputFrames[streamIndex] = 0;
	_CPU_ISR_Restore(level);
}

static void ResetSamplePlaybackClock(uint32 channelIndex)
{
	u32 level;
	_CPU_ISR_Disable(level);
	gSamplePlaybackGeneration[channelIndex]++;
	gSamplePendingOutputFrames[channelIndex] = 0;
	_CPU_ISR_Restore(level);
}

static void AccountQueuedSampleBuffer(uint32 bufferIndex)
{
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		if (gDmaBufferSampleGeneration[bufferIndex][i] == gSamplePlaybackGeneration[i])
			gSamplePendingOutputFrames[i] += gDmaBufferSampleFrames[bufferIndex][i];
	}
}

static void ReleaseQueuedSampleBuffer(uint32 bufferIndex)
{
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		if (gDmaBufferSampleGeneration[bufferIndex][i] != gSamplePlaybackGeneration[i])
			continue;
		uint32 frames = gDmaBufferSampleFrames[bufferIndex][i];
		if (frames >= gSamplePendingOutputFrames[i])
			gSamplePendingOutputFrames[i] = 0;
		else
			gSamplePendingOutputFrames[i] -= frames;
	}
}

static void CompleteDmaBufferPlayback(uint32 bufferIndex)
{
	ReleaseQueuedSampleBuffer(bufferIndex);
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		if (gDmaBufferSlotGeneration[bufferIndex][i] == gStreamPlaybackGeneration[i])
			gStreamPlayedOutputFrames[i] += gDmaBufferSlotFrames[bufferIndex][i];
	}
	gDmaBufferStates[bufferIndex] = GC_BUFFER_FREE;
}

static bool DmaBufferHasSampleAudio(uint32 bufferIndex)
{
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		if (gDmaBufferSampleFrames[bufferIndex][i] != 0)
			return true;
	}
	return false;
}

static bool StreamHasQueuedReadyAudio(uint32 streamIndex)
{
	u32 level;
	bool queued = false;
	_CPU_ISR_Disable(level);
	for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
		if (gDmaBufferStates[i] == GC_BUFFER_READY && gDmaBufferSlotFrames[i][streamIndex] != 0) {
			queued = true;
			break;
		}
	}
	_CPU_ISR_Restore(level);
	return queued;
}

static void ResetDmaReadyQueue()
{
	gDmaReadyHead = 0;
	gDmaReadyTail = 0;
	gDmaReadyCount = 0;
}

static void InvalidateReadyDmaBuffers()
{
	u32 level;
	_CPU_ISR_Disable(level);
	uint8 keptBuffers[GC_DMA_BUFFER_COUNT];
	uint32 keptCount = 0;
	uint32 queuedCount = gDmaReadyCount;

	for (uint32 n = 0; n < queuedCount; n++) {
		uint32 i = gDmaReadyQueue[(gDmaReadyHead + n) % GC_DMA_BUFFER_COUNT];
		if (i >= GC_DMA_BUFFER_COUNT || gDmaBufferStates[i] != GC_BUFFER_READY)
			continue;
		if (DmaBufferHasSampleAudio(i)) {
			memcpy(gDmaBuffers[i], gDmaSampleOnlyBuffers[i], GC_DMA_BUFFER_SIZE);
			DCFlushRange(gDmaBuffers[i], GC_DMA_BUFFER_SIZE);
			memset(gDmaBufferSlotFrames[i], 0, sizeof(gDmaBufferSlotFrames[i]));
			memset(gDmaBufferSlotGeneration[i], 0, sizeof(gDmaBufferSlotGeneration[i]));
			keptBuffers[keptCount++] = uint8(i);
			continue;
		}
		ReleaseQueuedSampleBuffer(i);
		gDmaBufferStates[i] = GC_BUFFER_FREE;
		memset(gDmaBufferSlotFrames[i], 0, sizeof(gDmaBufferSlotFrames[i]));
		memset(gDmaBufferSlotGeneration[i], 0, sizeof(gDmaBufferSlotGeneration[i]));
		memset(gDmaBufferSampleFrames[i], 0, sizeof(gDmaBufferSampleFrames[i]));
		memset(gDmaBufferSampleGeneration[i], 0, sizeof(gDmaBufferSampleGeneration[i]));
	}

	for (uint32 i = 0; i < keptCount; i++)
		gDmaReadyQueue[i] = keptBuffers[i];
	gDmaReadyHead = 0;
	gDmaReadyTail = keptCount % GC_DMA_BUFFER_COUNT;
	gDmaReadyCount = uint8(keptCount);
	_CPU_ISR_Restore(level);
}

/* Call with audio interrupts disabled. */
static bool PushDmaReadyBuffer(uint32 bufferIndex)
{
	if (bufferIndex >= GC_DMA_BUFFER_COUNT || gDmaReadyCount >= GC_DMA_BUFFER_COUNT)
		return false;

	gDmaReadyQueue[gDmaReadyTail] = uint8(bufferIndex);
	gDmaReadyTail = (gDmaReadyTail + 1) % GC_DMA_BUFFER_COUNT;
	gDmaReadyCount++;
	return true;
}

/* Call with audio interrupts disabled. */
static int32 PopDmaReadyBuffer()
{
	while (gDmaReadyCount != 0) {
		uint32 bufferIndex = gDmaReadyQueue[gDmaReadyHead];
		gDmaReadyHead = (gDmaReadyHead + 1) % GC_DMA_BUFFER_COUNT;
		gDmaReadyCount--;
		if (bufferIndex < GC_DMA_BUFFER_COUNT && gDmaBufferStates[bufferIndex] == GC_BUFFER_READY)
			return int32(bufferIndex);
	}
	return -1;
}

static const char *GetTrackPath(tTrack track)
{
	if (!IsSupportedGcStreamTrack(track))
		return NULL;

#ifndef GTA_PS2
	if (track == STREAMED_SOUND_RADIO_MP3_PLAYER)
		track = STREAMED_SOUND_RADIO_WILD;
	#endif

	const char *packagedPath = GetPackagedTrackPath(track);
	if (CanOpenTrackPath(packagedPath))
		return packagedPath;

	if (IsDirectPs2RadioWhitelistTrack(track)) {
		const char *directPath = ResolveDirectPs2RadioPath(track);
		if (CanOpenTrackPath(directPath))
			return directPath;
	}

	return NULL;
}

static bool8 ProbeTrackLength(tTrack track, uint32 &lengthMs)
{
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG && IsPs2MissionBankTrack(track)) {
		tPs2MissionSdtEntry entry;
		if (!GetPs2MissionSdtEntry(track, entry)) {
			lengthMs = 0;
			return FALSE;
		}
		/* The SDT allocation is padded. The decoder replaces this upper bound
		 * with the exact terminal-frame length after it reads the slice. */
		lengthMs = SamplesToMs(entry.sampleRate,
		                       uint64(entry.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE) * PS2_SFX_VAG_SAMPLES_PER_FRAME);
		return lengthMs != 0;
	}

	if (!IsSupportedGcStreamTrack(track)) {
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
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG && IsPs2MissionBankTrack(track)) {
		tPs2MissionSdtEntry entry;
		if (!GetPs2MissionSdtEntry(track, entry)) {
			MISSION_BANK_LOG("no bank entry for track=%u\n", uint32(track));
			return NULL;
		}
		CGcStreamDecoder *decoder = new CPs2MissionSfxDecoder(track, entry);
		if (decoder != NULL && (!decoder->IsOpened() || !decoder->Prepare())) {
			delete decoder;
			decoder = NULL;
		}
		return decoder;
	}

	if (!IsSupportedGcStreamTrack(track))
		return NULL;

	tTrack pathTrack = track;
#ifndef GTA_PS2
	if (pathTrack == STREAMED_SOUND_RADIO_MP3_PLAYER)
		pathTrack = STREAMED_SOUND_RADIO_WILD;
#endif
	uint32 sampleRate = IsThisTrackAt16KHz(pathTrack) ? 16000 : 32000;

	const char *path = GetTrackPath(pathTrack);
	if (path == NULL) {
		GC_AUDIO_LOG("[GC-AUDIO] no playable stream path for track %u\n", uint32(pathTrack));
		return NULL;
	}

	CGcStreamDecoder *decoder = OpenStreamDecoder(path, sampleRate);
	if (decoder) {
		if (IsCutsceneStreamTrack(pathTrack))
			CUTSCENE_AUDIO_LOG("open track=%u path=%s rate=%u lengthMs=%u\n",
			                   uint32(pathTrack), path, decoder->GetSampleRate(), decoder->GetLengthMS());
		if (IsDirectPs2RadioWhitelistTrack(pathTrack) || pathTrack == STREAMED_SOUND_RADIO_POLICE)
			GC_AUDIO_LOG("[GC-AUDIO] streaming radio %s from %s\n", GetRadioTrackLabel(pathTrack), path);
		else
			GC_AUDIO_LOG("[GC-AUDIO] streaming track %u from %s\n", uint32(pathTrack), path);
	} else {
		GC_AUDIO_LOG("[GC-AUDIO] failed to open track %u from %s\n", uint32(pathTrack), path);
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

static void RetireStreamDecoderLocked(CGcStreamDecoder *decoder)
{
	if (decoder == NULL)
		return;
	if (!gAudioProducerFilling) {
		delete decoder;
		return;
	}

	if (gDeferredStreamDecoderCount < ARRAY_SIZE(gDeferredStreamDecoders))
		gDeferredStreamDecoders[gDeferredStreamDecoderCount++] = decoder;
	/* A full retire list is intentionally leaked rather than freed while in use. */
}

static void CollectRetiredStreamDecodersLocked()
{
	if (gAudioProducerFilling)
		return;
	for (uint32 i = 0; i < gDeferredStreamDecoderCount; i++)
		delete gDeferredStreamDecoders[i];
	memset(gDeferredStreamDecoders, 0, sizeof(gDeferredStreamDecoders));
	gDeferredStreamDecoderCount = 0;
}

static void CloseStreamSlot(uint32 index)
{
	tGcStreamSlot &slot = gStreamSlots[index];
	ResetStreamPlaybackClock(index);
	RetireStreamDecoderLocked(slot.decoder);
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

static void ResetSlotForPlayback(uint32 streamIndex, uint32 startPosMs)
{
	tGcStreamSlot &slot = gStreamSlots[streamIndex];
	ResetStreamPlaybackClock(streamIndex);
	slot.startPosMs = startPosMs;
	slot.sourceEnded = FALSE;
	slot.sourceFramesValid = 0;
	slot.sourceFramePos = 0;
	slot.decodeRetryCount = 0;
	slot.resampleFrac = 0;
	slot.resampleStep = slot.decoder ? uint32((uint64(slot.decoder->GetSampleRate()) << GC_STREAM_RESAMPLE_FRAC_BITS) + GC_OUTPUT_RATE / 2) / GC_OUTPUT_RATE : 0;
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

	bool rewound = false;
	while (slot.sourceFramesValid - slot.sourceFramePos < minimumFrames) {
		if (slot.sourceFramePos != 0)
			CompactSourceBuffer(slot);

		if (slot.sourceFramesValid >= GC_SOURCE_BUFFER_FRAMES)
			break;

		uint32 capacity = GC_SOURCE_BUFFER_FRAMES - slot.sourceFramesValid;
		uint32 decoded = slot.decoder->DecodeFrames(slot.sourceBuffer + slot.sourceFramesValid * 2, capacity);
		uint32 decodedLengthMs = slot.decoder->GetLengthMS();
		if (decodedLengthMs != 0)
			slot.lengthMs = decodedLengthMs;
		if (decoded == 0) {
			if (slot.loop && slot.lengthMs > 0 && !rewound) {
				slot.decoder->SeekMS(0);
				slot.sourceEnded = FALSE;
				slot.sourceFramesValid = 0;
				slot.sourceFramePos = 0;
				slot.resampleFrac = 0;
				rewound = true;
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
		/* The stream thread owns normal decoding. Only use the old synchronous
		 * path if that thread could not be started. */
		if (gStreamDecoderRunning ||
		    (!RefillSourceBuffer(slot, 2) && slot.sourceFramesValid - slot.sourceFramePos == 0))
			return false;
	}

	uint32 available = slot.sourceFramesValid - slot.sourceFramePos;
	if (available == 0)
		return false;

	if (available == 1 && !slot.sourceEnded && !gStreamDecoderRunning)
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

	uint32 frac = slot.resampleFrac;
	int32 diffL = int32(bL) - int32(aL);
	int32 diffR = int32(bR) - int32(aR);
	outL = int16(int32(aL) + ((diffL * int32(frac)) >> GC_STREAM_RESAMPLE_FRAC_BITS));
	outR = int16(int32(aR) + ((diffR * int32(frac)) >> GC_STREAM_RESAMPLE_FRAC_BITS));

	slot.resampleFrac += slot.resampleStep;
	uint32 advance = slot.resampleFrac >> GC_STREAM_RESAMPLE_FRAC_BITS;
	slot.resampleFrac &= GC_STREAM_RESAMPLE_FRAC_MASK;
	slot.sourceFramePos += advance;

	if (slot.sourceFramePos > (GC_SOURCE_BUFFER_FRAMES / 2))
		CompactSourceBuffer(slot);

	return true;
}

static uint32 MixSlotIntoBuffer(uint32 streamIndex, tGcStreamSlot &slot, int32 *mixBuffer, uint32 outputFrames)
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

	int32 cutsceneInputPeak = 0;
	int32 cutsceneMixedPeak = 0;

	while (framesRendered < outputFrames) {
		int16 left;
		int16 right;
		if (!RenderNextOutputFrame(slot, left, right)) {
			/* A temporarily empty predecode ring only silences this stream. It
			 * must not stop the slot or block memory-resident SFX. */
			if (slot.sourceEnded) {
				slot.active = FALSE;
			}
			break;
		}

		if (IsCutsceneStreamTrack(slot.currentTrack) && !slot.cutsceneMixLogged)
			cutsceneInputPeak = Max(cutsceneInputPeak, Max(Abs(int32(left)), Abs(int32(right))));

		if (leftGain != 0 || rightGain != 0) {
			uint32 outIndex = framesRendered * 2;
			int32 mixedLeft = (int32(left) * int32(leftGain)) / MAX_VOLUME;
			int32 mixedRight = (int32(right) * int32(rightGain)) / MAX_VOLUME;
			mixBuffer[outIndex + 0] += mixedLeft;
			mixBuffer[outIndex + 1] += mixedRight;
			if (IsCutsceneStreamTrack(slot.currentTrack) && !slot.cutsceneMixLogged) {
				cutsceneMixedPeak = Max(cutsceneMixedPeak, Max(Abs(mixedLeft), Abs(mixedRight)));
			}
		}
		framesRendered++;
	}

	if (IsCutsceneStreamTrack(slot.currentTrack) && !slot.cutsceneMixLogged && framesRendered != 0) {
		slot.cutsceneMixLogged = TRUE;
		CUTSCENE_AUDIO_LOG("mix first buffer stream=%u track=%u requestedVolume=%u mixVolume=%u pan=%u leftGain=%u rightGain=%u inputPeak=%d mixedPeak=%d renderedFrames=%u\n",
		                   streamIndex, uint32(slot.currentTrack), uint32(slot.requestedVolume), slot.mixVolume,
		                   uint32(slot.pan), leftGain, rightGain, cutsceneInputPeak, cutsceneMixedPeak, framesRendered);
	}

	return framesRendered;
}

static void ConvertMixBufferToPcm(const int32 *mixBuffer, int16 *outputBuffer)
{
	for (uint32 i = 0; i < GC_DMA_BUFFER_FRAMES * 2; i++) {
		int64 magnitude = mixBuffer[i] < 0 ? -int64(mixBuffer[i]) : int64(mixBuffer[i]);
		int64 softened = magnitude;
		if (magnitude > 16384) {
			if (magnitude >= 49152) {
				softened = 32767;
			} else {
				int64 overKnee = magnitude - 16384;
				softened = 16384 + overKnee - ((overKnee * overKnee) >> 16);
			}
		}
		if (softened > 32767)
			softened = 32767;
		outputBuffer[i] = int16(mixBuffer[i] < 0 ? -softened : softened);
	}
}

struct tGcAudioFillSnapshot
{
	tGcSampleChannel sampleChannels[MAXCHANNELS + MAX2DCHANNELS];
	tGcStreamSlot streamSlots[MAX_STREAMS];
	uint32 sampleGeneration[MAXCHANNELS + MAX2DCHANNELS];
	uint32 streamGeneration[MAX_STREAMS];
};

static bool FillDmaBuffer(uint32 bufferIndex, tGcAudioFillSnapshot &snapshot)
{
	memset(gMixBuffer, 0, sizeof(gMixBuffer));
	memset(gDmaBufferSlotFrames[bufferIndex], 0, sizeof(gDmaBufferSlotFrames[bufferIndex]));
	memset(gDmaBufferSlotGeneration[bufferIndex], 0, sizeof(gDmaBufferSlotGeneration[bufferIndex]));
	memset(gDmaBufferSampleFrames[bufferIndex], 0, sizeof(gDmaBufferSampleFrames[bufferIndex]));
	memset(gDmaBufferSampleGeneration[bufferIndex], 0, sizeof(gDmaBufferSampleGeneration[bufferIndex]));

	bool hasSampleAudio = false;
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		uint32 frames = MixSampleChannelIntoBuffer(snapshot.sampleChannels[i], gMixBuffer, GC_DMA_BUFFER_FRAMES);
		gDmaBufferSampleFrames[bufferIndex][i] = frames;
		gDmaBufferSampleGeneration[bufferIndex][i] = snapshot.sampleGeneration[i];
		if (frames != 0)
			hasSampleAudio = true;
	}
	if (hasSampleAudio)
		ConvertMixBufferToPcm(gMixBuffer, gDmaSampleOnlyBuffers[bufferIndex]);
	else
		memset(gDmaSampleOnlyBuffers[bufferIndex], 0, GC_DMA_BUFFER_SIZE);

	bool hasAudio = hasSampleAudio;
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		uint32 frames = MixSlotIntoBuffer(i, snapshot.streamSlots[i], gMixBuffer, GC_DMA_BUFFER_FRAMES);
		gDmaBufferSlotFrames[bufferIndex][i] = frames;
		gDmaBufferSlotGeneration[bufferIndex][i] = snapshot.streamGeneration[i];
		if (frames != 0)
			hasAudio = true;
	}

	if (!hasAudio)
		return false;

	ConvertMixBufferToPcm(gMixBuffer, gDmaBuffers[bufferIndex]);

	DCFlushRange(gDmaBuffers[bufferIndex], GC_DMA_BUFFER_SIZE);
	return true;
}

static bool TryFillReadyBuffers()
{
	cAudioFillLock fillLock;
	tGcAudioFillSnapshot snapshot;
	int32 bufferIndex = -1;
	{
		cAudioStateLock lock;
		gDmaExpectingAudio = AnyActiveUnpausedStreams() || AnyActiveSampleChannels();
		if (!gDmaExpectingAudio || gAudioProducerFilling)
			return false;

		u32 level;
		_CPU_ISR_Disable(level);
		for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
			if (gDmaBufferStates[i] == GC_BUFFER_FREE) {
				gDmaBufferStates[i] = GC_BUFFER_FILLING;
				bufferIndex = int32(i);
				break;
			}
		}
		_CPU_ISR_Restore(level);
		if (bufferIndex < 0)
			return false;

		for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
			snapshot.sampleChannels[i] = gSampleChannels[i];
			snapshot.sampleGeneration[i] = gSamplePlaybackGeneration[i];
		}
		for (uint32 i = 0; i < MAX_STREAMS; i++) {
			snapshot.streamSlots[i] = gStreamSlots[i];
			snapshot.streamGeneration[i] = gStreamPlaybackGeneration[i];
		}
		gAudioProducerFilling = TRUE;
	}

	u64 fillStart = gettime();
	bool filled = FillDmaBuffer(uint32(bufferIndex), snapshot);
	uint32 fillTimeUs = uint32(ticks_to_microsecs(gettime() - fillStart));

	bool queued = false;
	{
		cAudioStateLock lock;
		gAudioProducerFilling = FALSE;
		CollectRetiredStreamDecodersLocked();
		if (fillTimeUs > gDmaMaximumFillTimeUs)
			gDmaMaximumFillTimeUs = fillTimeUs;

		for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
			if (snapshot.sampleGeneration[i] != gSamplePlaybackGeneration[i])
				continue;
			bool channelFinished = gSampleChannels[i].active && !snapshot.sampleChannels[i].active;
			gSampleChannels[i].cursorFixed = snapshot.sampleChannels[i].cursorFixed;
			gSampleChannels[i].active = snapshot.sampleChannels[i].active;
			gSampleChannels[i].renderedOutputFrames = snapshot.sampleChannels[i].renderedOutputFrames;
			if (channelFinished) {
				gSampleChannels[i].initialised = FALSE;
				gSampleChannels[i].sampleData = NULL;
				gSampleChannels[i].sampleCount = 0;
			}
		}
		for (uint32 i = 0; i < MAX_STREAMS; i++) {
			if (snapshot.streamGeneration[i] != gStreamPlaybackGeneration[i])
				continue;
			gStreamSlots[i].active = snapshot.streamSlots[i].active;
			gStreamSlots[i].sourceEnded = snapshot.streamSlots[i].sourceEnded;
			gStreamSlots[i].sourceFramesValid = snapshot.streamSlots[i].sourceFramesValid;
			gStreamSlots[i].sourceFramePos = snapshot.streamSlots[i].sourceFramePos;
			gStreamSlots[i].resampleFrac = snapshot.streamSlots[i].resampleFrac;
			gStreamSlots[i].lengthMs = snapshot.streamSlots[i].lengthMs;
			gStreamSlots[i].cutsceneMixLogged = snapshot.streamSlots[i].cutsceneMixLogged;
		}

		u32 level;
		_CPU_ISR_Disable(level);
		if (filled)
			queued = gDmaBufferStates[bufferIndex] == GC_BUFFER_FILLING && PushDmaReadyBuffer(uint32(bufferIndex));
		if (queued) {
			gDmaBufferStates[bufferIndex] = GC_BUFFER_READY;
			AccountQueuedSampleBuffer(uint32(bufferIndex));
		} else {
			gDmaBufferStates[bufferIndex] = GC_BUFFER_FREE;
		}
		_CPU_ISR_Restore(level);
		if (queued)
			gDmaBuffersFilled++;
		if (queued)
			SignalStreamDecoder();

		gDmaExpectingAudio = AnyActiveUnpausedStreams() || AnyActiveSampleChannels();
		if (!gDmaExpectingAudio)
			return false;
		if (!filled)
			return false;
		for (uint32 i = 0; i < GC_DMA_BUFFER_COUNT; i++) {
			if (gDmaBufferStates[i] == GC_BUFFER_FREE)
				return true;
		}
	}
	return false;
}

static void SignalAudioProducer()
{
	u32 level;
	_CPU_ISR_Disable(level);
	gAudioProducerWakeRequested = TRUE;
	_CPU_ISR_Restore(level);
	if (gAudioProducerRunning && gAudioProducerSemaphore != LWP_SEM_NULL)
		LWP_SemPost(gAudioProducerSemaphore);
}

static bool ConsumeAudioProducerWakeRequest()
{
	u32 level;
	_CPU_ISR_Disable(level);
	bool requested = gAudioProducerWakeRequested != FALSE;
	gAudioProducerWakeRequested = FALSE;
	_CPU_ISR_Restore(level);
	return requested;
}

static bool OpenPendingStream(uint32 streamIndex)
{
	tTrack track = NO_TRACK;
	uint32 startPosMs = 0;
	uint32 generation = 0;
	{
		cAudioFillLock fillLock;
		cAudioStateLock stateLock;
		tGcStreamSlot &slot = gStreamSlots[streamIndex];
		if (!slot.active || !slot.openPending)
			return false;
		track = slot.pendingTrack;
		startPosMs = slot.pendingStartPosMs;
		generation = gStreamPlaybackGeneration[streamIndex];
	}

	CGcStreamDecoder *decoder = OpenTrackDecoder(track);
	if (decoder != NULL)
		decoder->SeekMS(startPosMs);

	bool accepted = false;
	{
		cAudioFillLock fillLock;
		cAudioStateLock stateLock;
		tGcStreamSlot &slot = gStreamSlots[streamIndex];
		if (!slot.active || !slot.openPending || slot.pendingTrack != track ||
		    slot.pendingStartPosMs != startPosMs ||
		    gStreamPlaybackGeneration[streamIndex] != generation) {
			accepted = false;
		} else {
			slot.openPending = FALSE;
			slot.pendingTrack = NO_TRACK;
			slot.pendingStartPosMs = 0;
			if (decoder == NULL) {
				slot.active = FALSE;
				slot.preloaded = FALSE;
			} else {
				slot.decoder = decoder;
				slot.preloaded = TRUE;
				slot.currentTrack = track;
				slot.lengthMs = decoder->GetLengthMS();
				slot.resampleStep = uint32((uint64(decoder->GetSampleRate()) << GC_STREAM_RESAMPLE_FRAC_BITS) +
				                           GC_OUTPUT_RATE / 2) / GC_OUTPUT_RATE;
				slot.sourceEnded = FALSE;
				slot.sourceFramesValid = 0;
				slot.sourceFramePos = 0;
				slot.decodeRetryCount = 0;
				slot.resampleFrac = 0;
				accepted = true;
			}
		}
	}
	if (!accepted)
		delete decoder;

	SignalAudioProducer();
	return true;
}

static bool DecodeStreamChunk(uint32 streamIndex)
{
	cStreamDecodeLock decodeLock;
	CGcStreamDecoder *decoder = NULL;
	uint32 requestFrames = 0;
	bool8 canLoop = FALSE;
	uint32 generation = 0;
	bool retryLater = false;

	{
		cAudioFillLock fillLock;
		cAudioStateLock stateLock;
		tGcStreamSlot &slot = gStreamSlots[streamIndex];
		if (!slot.active || slot.paused || slot.decoder == NULL)
			return false;
		if (slot.sourceEnded && !(slot.loop && slot.lengthMs > 0))
			return false;

		CompactSourceBuffer(slot);
		uint32 freeFrames = GC_SOURCE_BUFFER_FRAMES - slot.sourceFramesValid;
		if (freeFrames == 0)
			return false;

		decoder = slot.decoder;
		canLoop = slot.loop && slot.lengthMs > 0;
		generation = gStreamPlaybackGeneration[streamIndex];
		requestFrames = Min(freeFrames, GC_STREAM_DECODE_CHUNK_FRAMES);
	}

	uint32 decoded = decoder->DecodeFrames(gStreamDecodeBuffer, requestFrames);
	if (decoded == 0 && canLoop) {
		decoder->SeekMS(0);
		decoded = decoder->DecodeFrames(gStreamDecodeBuffer, requestFrames);
	}
	uint32 decodedLengthMs = decoder->GetLengthMS();

	{
		cAudioFillLock fillLock;
		cAudioStateLock stateLock;
		tGcStreamSlot &slot = gStreamSlots[streamIndex];
		if (slot.decoder != decoder || gStreamPlaybackGeneration[streamIndex] != generation)
			return false;

		CompactSourceBuffer(slot);
		uint32 freeFrames = GC_SOURCE_BUFFER_FRAMES - slot.sourceFramesValid;
		if (decoded > freeFrames)
			decoded = freeFrames;
		if (decodedLengthMs != 0)
			slot.lengthMs = decodedLengthMs;
		if (decoded != 0) {
			memcpy(slot.sourceBuffer + slot.sourceFramesValid * 2,
			       gStreamDecodeBuffer, sizeof(int16) * decoded * 2);
			slot.sourceFramesValid += decoded;
			slot.sourceEnded = FALSE;
			slot.decodeRetryCount = 0;
		} else {
			if (canLoop && slot.decodeRetryCount < GC_STREAM_DECODE_RETRY_LIMIT) {
				slot.decodeRetryCount++;
				slot.sourceEnded = FALSE;
				retryLater = true;
			} else {
				slot.sourceEnded = TRUE;
				slot.resampleFrac = 0;
			}
		}
	}

	SignalAudioProducer();
	if (retryLater)
		SignalStreamDecoder();
	return decoded != 0;
}

static void *StreamDecoderMain(void *)
{
	while (!gStreamDecoderStop) {
		LWP_SemWait(gStreamDecoderSemaphore);
		if (gStreamDecoderStop)
			break;

		bool progress;
		do {
			progress = false;
			for (uint32 i = 0; i < MAX_STREAMS; i++) {
				progress = OpenPendingStream(i) || progress;
				progress = DecodeStreamChunk(i) || progress;
			}
		} while (progress && !gStreamDecoderStop);
	}
	return NULL;
}

static void SignalStreamDecoder()
{
	if (gStreamDecoderRunning && gStreamDecoderSemaphore != LWP_SEM_NULL)
		LWP_SemPost(gStreamDecoderSemaphore);
}

static bool StartStreamDecoder()
{
	if (gStreamDecoderRunning)
		return true;
	if (gStreamDecodeMutex == LWP_MUTEX_NULL && LWP_MutexInit(&gStreamDecodeMutex, true) != 0)
		return false;
	if (gStreamDecoderSemaphore == LWP_SEM_NULL && LWP_SemInit(&gStreamDecoderSemaphore, 0, 1) != 0)
		return false;

	gStreamDecoderStop = FALSE;
	if (LWP_CreateThread(&gStreamDecoderThread, StreamDecoderMain, NULL,
	                     gStreamDecoderStack, sizeof(gStreamDecoderStack),
	                     LWP_PRIO_NORMAL + 12) != 0) {
		gStreamDecoderThread = LWP_THREAD_NULL;
		return false;
	}
	gStreamDecoderRunning = TRUE;
	return true;
}

static void StopStreamDecoder()
{
	if (gStreamDecoderRunning) {
		gStreamDecoderStop = TRUE;
		SignalStreamDecoder();
		LWP_JoinThread(gStreamDecoderThread, NULL);
		gStreamDecoderThread = LWP_THREAD_NULL;
		gStreamDecoderRunning = FALSE;
	}
	if (gStreamDecoderSemaphore != LWP_SEM_NULL) {
		LWP_SemDestroy(gStreamDecoderSemaphore);
		gStreamDecoderSemaphore = LWP_SEM_NULL;
	}
}

static bool PedCommentSlotIsReferencedLocked(uint32 slotIndex)
{
	uint8 *slotMemory = gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS];
	if (slotMemory == NULL || slotIndex >= MAX_PEDSFX)
		return true;

	const int16 *slotData = (const int16 *)(slotMemory + gPedSlotCapacityBytes * slotIndex);
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++) {
		const tGcSampleChannel &channel = gSampleChannels[i];
		if (channel.initialised && channel.sampleData == slotData)
			return true;
	}
	return false;
}

static int32 ReservePedCommentSlotLocked()
{
	for (uint32 offset = 0; offset < MAX_PEDSFX; offset++) {
		uint32 slotIndex = (gCurrentPedSlot + offset) % MAX_PEDSFX;
		if (!gPedSlotLoading[slotIndex] && gPedSlotClaimFrames[slotIndex] == 0 &&
		    !PedCommentSlotIsReferencedLocked(slotIndex))
			return int32(slotIndex);
	}
	return -1;
}

static bool TakePedCommentLoadRequest(uint32 &sample, uint32 &fileOffset, uint32 &sizeBytes, uint32 &slotIndex)
{
	cAudioFillLock fillLock;
	cAudioStateLock stateLock;
	if (gPedCommentLoaderStop || gPedCommentPending == NO_SAMPLE)
		return false;

	int32 freeSlot = ReservePedCommentSlotLocked();
	if (freeSlot < 0)
		return false;

	sample = gPedCommentPending;
	fileOffset = gPedCommentPendingOffset;
	sizeBytes = gPedCommentPendingSize;
	slotIndex = uint32(freeSlot);
	gPedCommentPending = NO_SAMPLE;
	gPedCommentPendingOffset = 0;
	gPedCommentPendingSize = 0;
	gPedCommentLoading = sample;
	gPedSlotLoading[slotIndex] = TRUE;
	gPedSlotClaimFrames[slotIndex] = 0;
	gPedSlotSfx[slotIndex] = NO_SAMPLE;
	gPedSlotSfxAddr[slotIndex] = NULL;
	gPedSlotSizeBytes[slotIndex] = 0;
	gCurrentPedSlot = uint8((slotIndex + 1) % MAX_PEDSFX);
	return true;
}

static bool ReadPedComment(uint32 fileOffset, uint32 sourceBytes, uint32 &decodedBytes)
{
	decodedBytes = 0;
	if (gPedCommentDataFile == NULL || gPedCommentLoadBuffer == NULL || sourceBytes == 0 || sourceBytes > gPedSlotCapacityBytes)
		return false;
	if (fseek(gPedCommentDataFile, long(fileOffset), SEEK_SET) != 0)
		return false;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		if (gPedCommentVagBuffer == NULL ||
		    fread(gPedCommentVagBuffer, 1, sourceBytes, gPedCommentDataFile) != sourceBytes)
			return false;
		uint32 loopStartBytes = 0;
		int32 loopEndBytes = -1;
		return DecodePs2VagData(gPedCommentVagBuffer, sourceBytes, (int16*)gPedCommentLoadBuffer,
		                        gPedSlotCapacityBytes, decodedBytes, loopStartBytes, loopEndBytes) != FALSE;
	}
	if (fread(gPedCommentLoadBuffer, 1, sourceBytes, gPedCommentDataFile) != sourceBytes)
		return false;
	ByteSwapPcm16Buffer(gPedCommentLoadBuffer, sourceBytes);
	decodedBytes = sourceBytes;
	return true;
}

static void CompletePedCommentLoad(uint32 sample, uint32 sizeBytes, uint32 slotIndex, bool loaded)
{
	cAudioFillLock fillLock;
	cAudioStateLock stateLock;
	if (slotIndex >= MAX_PEDSFX || !gPedSlotLoading[slotIndex] || gPedCommentLoading != sample)
		return;

	uint8 *slotMemory = gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS];
	if (loaded && !gPedCommentLoaderStop && slotMemory != NULL) {
		uint8 *destination = slotMemory + gPedSlotCapacityBytes * slotIndex;
		memcpy(destination, gPedCommentLoadBuffer, sizeBytes);
		gPedSlotSfx[slotIndex] = int32(sample);
		gPedSlotSfxAddr[slotIndex] = destination;
		gPedSlotSizeBytes[slotIndex] = sizeBytes;
	}

	gPedSlotLoading[slotIndex] = FALSE;
	gPedCommentLoading = NO_SAMPLE;
}

static void *PedCommentLoaderMain(void *)
{
	const struct timespec retryWait = { 0, 5000000 };
	while (!gPedCommentLoaderStop) {
		LWP_SemTimedWait(gPedCommentLoaderSemaphore, &retryWait);
		if (gPedCommentLoaderStop)
			break;

		uint32 sample = NO_SAMPLE;
		uint32 fileOffset = 0;
		uint32 sizeBytes = 0;
		uint32 slotIndex = 0;
		if (!TakePedCommentLoadRequest(sample, fileOffset, sizeBytes, slotIndex))
			continue;

		uint32 decodedBytes = 0;
		bool loaded = ReadPedComment(fileOffset, sizeBytes, decodedBytes);
		CompletePedCommentLoad(sample, decodedBytes, slotIndex, loaded);
	}
	return NULL;
}

static void SignalPedCommentLoader()
{
	if (gPedCommentLoaderRunning && gPedCommentLoaderSemaphore != LWP_SEM_NULL)
		LWP_SemPost(gPedCommentLoaderSemaphore);
}

static void FreePedCommentScratchBuffers()
{
	if (gPedCommentLoadBuffer != NULL) {
#ifdef WII
		MemoryMgrFreeMem2(gPedCommentLoadBuffer);
#else
		free(gPedCommentLoadBuffer);
#endif
		gPedCommentLoadBuffer = NULL;
	}
	if (gPedCommentVagBuffer != NULL) {
#ifdef WII
		MemoryMgrFreeMem2(gPedCommentVagBuffer);
#else
		free(gPedCommentVagBuffer);
#endif
		gPedCommentVagBuffer = NULL;
	}
}

static bool StartPedCommentLoader()
{
	if (gPedCommentLoaderRunning)
		return true;
	if (gAudioStateMutex == LWP_MUTEX_NULL || gAudioFillMutex == LWP_MUTEX_NULL)
		return false;
	if (gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] == NULL)
		return false;

	char ps2PedPath[PS2_SFX_BANK_PATH_CAPACITY];
	const char *pedPath = GC_SAMPLE_BANK_DATA_PATH;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		if (!BuildPs2SfxPath(2, "RAW", ps2PedPath, sizeof(ps2PedPath)))
			return false;
		pedPath = ps2PedPath;
	}
	gPedCommentDataFile = fopen(pedPath, "rb");
	if (gPedCommentDataFile == NULL)
		return false;
	gPedCommentLoadBuffer = AllocAlignedAudioBuffer(gPedSlotCapacityBytes);
	if (gPedCommentLoadBuffer == NULL) {
		fclose(gPedCommentDataFile);
		gPedCommentDataFile = NULL;
		return false;
	}
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		gPedCommentVagBuffer = AllocAlignedAudioBuffer(gPedSlotCapacityBytes);
		if (gPedCommentVagBuffer == NULL) {
			FreePedCommentScratchBuffers();
			fclose(gPedCommentDataFile);
			gPedCommentDataFile = NULL;
			return false;
		}
	}
	if (LWP_SemInit(&gPedCommentLoaderSemaphore, 0, 1) != 0) {
		FreePedCommentScratchBuffers();
		fclose(gPedCommentDataFile);
		gPedCommentDataFile = NULL;
		return false;
	}

	gPedCommentLoaderStop = FALSE;
	gPedCommentPending = NO_SAMPLE;
	gPedCommentLoading = NO_SAMPLE;
	gPedCommentPendingOffset = 0;
	gPedCommentPendingSize = 0;
	memset(gPedSlotLoading, 0, sizeof(gPedSlotLoading));
	memset(gPedSlotClaimFrames, 0, sizeof(gPedSlotClaimFrames));
	if (LWP_CreateThread(&gPedCommentLoaderThread, PedCommentLoaderMain, NULL,
	                     gPedCommentLoaderStack, sizeof(gPedCommentLoaderStack),
	                     LWP_PRIO_NORMAL + 10) != 0) {
		LWP_SemDestroy(gPedCommentLoaderSemaphore);
		gPedCommentLoaderSemaphore = LWP_SEM_NULL;
		FreePedCommentScratchBuffers();
		fclose(gPedCommentDataFile);
		gPedCommentDataFile = NULL;
		gPedCommentLoaderThread = LWP_THREAD_NULL;
		return false;
	}
	gPedCommentLoaderRunning = TRUE;
	return true;
}

static void StopPedCommentLoader()
{
	if (gPedCommentLoaderRunning) {
		gPedCommentLoaderStop = TRUE;
		SignalPedCommentLoader();
		LWP_JoinThread(gPedCommentLoaderThread, NULL);
		gPedCommentLoaderThread = LWP_THREAD_NULL;
		gPedCommentLoaderRunning = FALSE;
	}
	if (gPedCommentLoaderSemaphore != LWP_SEM_NULL) {
		LWP_SemDestroy(gPedCommentLoaderSemaphore);
		gPedCommentLoaderSemaphore = LWP_SEM_NULL;
	}
	if (gPedCommentDataFile != NULL) {
		fclose(gPedCommentDataFile);
		gPedCommentDataFile = NULL;
	}
	FreePedCommentScratchBuffers();
	gPedCommentPending = NO_SAMPLE;
	gPedCommentLoading = NO_SAMPLE;
	gPedCommentPendingOffset = 0;
	gPedCommentPendingSize = 0;
	memset(gPedSlotLoading, 0, sizeof(gPedSlotLoading));
	memset(gPedSlotClaimFrames, 0, sizeof(gPedSlotClaimFrames));
}

static void *AudioProducerMain(void *)
{
	const struct timespec waitTime = { 0, 5000000 };
	while (!gAudioProducerStop) {
		LWP_SemTimedWait(gAudioProducerSemaphore, &waitTime);
		if (gAudioProducerStop)
			break;
		if (!ConsumeAudioProducerWakeRequest())
			continue;

		bool refillNeeded = TryFillReadyBuffers();
		if (refillNeeded && !gAudioProducerStop)
			SignalAudioProducer();
	}
	return NULL;
}

static bool StartAudioProducer()
{
	if (gAudioProducerRunning)
		return true;
	if (gAudioStateMutex == LWP_MUTEX_NULL && LWP_MutexInit(&gAudioStateMutex, true) != 0)
		return false;
	if (gAudioFillMutex == LWP_MUTEX_NULL && LWP_MutexInit(&gAudioFillMutex, true) != 0)
		return false;
	if (gAudioProducerSemaphore == LWP_SEM_NULL && LWP_SemInit(&gAudioProducerSemaphore, 0, 1) != 0)
		return false;

	gAudioProducerStop = FALSE;
	gAudioProducerWakeRequested = FALSE;
	if (LWP_CreateThread(&gAudioProducerThread, AudioProducerMain, NULL,
	                     gAudioProducerStack, sizeof(gAudioProducerStack),
	                     LWP_PRIO_NORMAL + 4) != 0) {
		gAudioProducerThread = LWP_THREAD_NULL;
		return false;
	}
	gAudioProducerRunning = TRUE;
	return true;
}

static void StopAudioProducer()
{
	if (gAudioProducerRunning) {
		gAudioProducerStop = TRUE;
		SignalAudioProducer();
		LWP_JoinThread(gAudioProducerThread, NULL);
		gAudioProducerThread = LWP_THREAD_NULL;
		gAudioProducerRunning = FALSE;
	}
	if (gAudioProducerSemaphore != LWP_SEM_NULL) {
		LWP_SemDestroy(gAudioProducerSemaphore);
		gAudioProducerSemaphore = LWP_SEM_NULL;
	}
}

static void DestroyAudioStateMutex()
{
	if (gStreamDecodeMutex != LWP_MUTEX_NULL) {
		LWP_MutexDestroy(gStreamDecodeMutex);
		gStreamDecodeMutex = LWP_MUTEX_NULL;
	}
	if (gAudioFillMutex != LWP_MUTEX_NULL) {
		LWP_MutexDestroy(gAudioFillMutex);
		gAudioFillMutex = LWP_MUTEX_NULL;
	}
	if (gAudioStateMutex != LWP_MUTEX_NULL) {
		LWP_MutexDestroy(gAudioStateMutex);
		gAudioStateMutex = LWP_MUTEX_NULL;
	}
}

static void RequestAudioFillLocked()
{
	if (gAudioProducerRunning)
		SignalAudioProducer();
	else {
		/* The synchronous fallback is serviced from cSampleManager::Service(). */
		gAudioProducerWakeRequested = TRUE;
	}
}

static void AudioDmaCallback()
{
	u32 level;
	_CPU_ISR_Disable(level);
	gDmaOutputBufferCount++;

	if (gPlayingDmaBuffer >= 0 && gPlayingDmaBuffer < int32(GC_DMA_BUFFER_COUNT))
		CompleteDmaBufferPlayback((uint32)gPlayingDmaBuffer);

	gPlayingDmaBuffer = gSubmittedDmaBuffer;
	gSubmittedDmaBuffer = -1;

	int32 nextBuffer = PopDmaReadyBuffer();
	if (gDmaExpectingAudio && gDmaReadyCount < gDmaMinimumReadyDepth)
		gDmaMinimumReadyDepth = gDmaReadyCount;

	if (nextBuffer >= 0) {
		gDmaBufferStates[nextBuffer] = GC_BUFFER_PLAYING;
		AUDIO_InitDMA((u32)gDmaBuffers[nextBuffer], GC_DMA_BUFFER_SIZE);
		gSubmittedDmaBuffer = nextBuffer;
	} else {
		gDmaSilenceBufferCount++;
		if (gDmaExpectingAudio)
			gDmaUnderrunCount++;
		AUDIO_InitDMA((u32)gSilenceBuffer, GC_DMA_BUFFER_SIZE);
	}

	gAudioProducerWakeRequested = TRUE;
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
		memset(gDmaSampleOnlyBuffers[i], 0, sizeof(gDmaSampleOnlyBuffers[i]));
		memset(gDmaBufferSlotFrames[i], 0, sizeof(gDmaBufferSlotFrames[i]));
		memset(gDmaBufferSlotGeneration[i], 0, sizeof(gDmaBufferSlotGeneration[i]));
		memset(gDmaBufferSampleFrames[i], 0, sizeof(gDmaBufferSampleFrames[i]));
		memset(gDmaBufferSampleGeneration[i], 0, sizeof(gDmaBufferSampleGeneration[i]));
	}
	memset((void *)gSamplePendingOutputFrames, 0, sizeof(gSamplePendingOutputFrames));
	ResetDmaReadyQueue();
	gDmaExpectingAudio = FALSE;
	gDmaUnderrunCount = 0;
	gDmaSilenceBufferCount = 0;
	gDmaOutputBufferCount = 0;
	gDmaMinimumReadyDepth = GC_DMA_BUFFER_COUNT;
	gDmaBuffersFilled = 0;
	gDmaMaximumFillTimeUs = 0;

	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(AudioDmaCallback);
	AUDIO_InitDMA((u32)gSilenceBuffer, GC_DMA_BUFFER_SIZE);
	AUDIO_StartDMA();

	gPlayingDmaBuffer = -1;
	gSubmittedDmaBuffer = -1;
	gDmaActive = TRUE;
}

#if GC_AUDIO_DEBUG_LOG
static void LogDmaDiagnostics()
{
	static uint32 serviceCallsUntilLog = 240;
	if (--serviceCallsUntilLog != 0)
		return;
	serviceCallsUntilLog = 240;

	u32 level;
	_CPU_ISR_Disable(level);
	uint32 underruns = gDmaUnderrunCount;
	uint32 silenceBuffers = gDmaSilenceBufferCount;
	uint32 outputBuffers = gDmaOutputBufferCount;
	uint32 minimumReadyDepth = gDmaMinimumReadyDepth;
	uint32 readyDepth = gDmaReadyCount;
	uint32 buffersFilled = gDmaBuffersFilled;
	uint32 maximumFillTimeUs = gDmaMaximumFillTimeUs;
	_CPU_ISR_Restore(level);

	GC_AUDIO_LOG("[GC-AUDIO] dma out=%u filled=%u underruns=%u silence=%u ready=%u minReady=%u maxFill=%uus\n",
	             outputBuffers, buffersFilled, underruns, silenceBuffers, readyDepth,
	             minimumReadyDepth, maximumFillTimeUs);
}
#endif

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
	gAudioProducerFilling = FALSE;
	gDeferredStreamDecoderCount = 0;
	memset(gDeferredStreamDecoders, 0, sizeof(gDeferredStreamDecoders));

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
		gStreamPlaybackGeneration[i] = 0;
		gStreamPlayedOutputFrames[i] = 0;
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
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		if (!InitialisePs2MissionBank())
			printf("[MISSION-BANK] WARN: PS2 mission dialogue bank unavailable\n");
	} else {
		ResetPs2MissionBank();
	}
	if (GC_IDSP_DSP_ENABLED) {
		if (CGcIdspDspTask::Initialize())
			printf("[GC-AUDIO] IDSP DSP decoder ready; CPU mixer retained\n");
		else
			printf("[GC-AUDIO] WARN: IDSP DSP task unavailable; using CPU IDSP decoder\n");
	}
	/* IDSP assets now cover radio, ambient Music tracks, and cutscene streams.
	 * Probe every packaged compressed stream up front so MusicManager does not
	 * see the old one-millisecond placeholder for non-radio tracks. Mission
	 * bank entries stay lazy and use their SDT allocation as an upper bound. */
	for (uint32 i = 0; i < TOTAL_STREAMED_SOUNDS; i++) {
		uint32 length = 0;
		if (ShouldEagerProbePackagedTrack((tTrack)i) && ProbeTrackLength((tTrack)i, length) && length != 0)
			gStreamLength[i] = length;
	}

	if (!StartAudioProducer())
		printf("[GC-AUDIO] WARN: producer thread init failed; using synchronous refill\n");
	if (!StartStreamDecoder())
		printf("[GC-AUDIO] WARN: stream decoder thread init failed; using synchronous stream decode\n");
	if (!StartPedCommentLoader())
		printf("[GC-AUDIO] WARN: ped comment loader init failed; ped comments disabled\n");
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
		if (gPlayingDmaBuffer >= 0 && gPlayingDmaBuffer < int32(GC_DMA_BUFFER_COUNT))
			gDmaBufferStates[gPlayingDmaBuffer] = GC_BUFFER_FREE;
		if (gSubmittedDmaBuffer >= 0 && gSubmittedDmaBuffer < int32(GC_DMA_BUFFER_COUNT))
			gDmaBufferStates[gSubmittedDmaBuffer] = GC_BUFFER_FREE;
		gPlayingDmaBuffer = -1;
		gSubmittedDmaBuffer = -1;
	}
	/* Stop the consumer before destroying the decoder wake semaphore; the DMA
	 * producer can still signal the decoder while it drains its last buffer. */
	StopPedCommentLoader();
	StopAudioProducer();
	StopStreamDecoder();
	CGcIdspDspTask::Shutdown();
	ResetDmaReadyQueue();

	{
		cAudioStateLock lock;
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
		ResetPs2MissionBank();
		gSfxBackend = GC_SFX_BACKEND_UNSELECTED;
	}
	DestroyAudioStateMutex();

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
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
	m_nEffectsVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (slot.effectFlag) {
			if (i == 1 || i == 2 || IsCutsceneStreamTrack(slot.currentTrack))
				slot.mixVolume = 128 * slot.requestedVolume * m_nEffectsVolume >> 14;
			else
				slot.mixVolume = m_nEffectsFadeVolume * slot.requestedVolume * m_nEffectsVolume >> 14;
		}
	}
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++)
		UpdateSampleChannelVolume(gSampleChannels[i], m_nEffectsVolume, m_nEffectsFadeVolume);
#endif
}

void
cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
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
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
	m_nMP3BoostVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);
}

void
cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
	m_nEffectsFadeVolume = Min((uint32)nVolume, (uint32)MAX_VOLUME);

#ifdef GAMECUBE
	for (uint32 i = 0; i < MAX_STREAMS; i++) {
		tGcStreamSlot &slot = gStreamSlots[i];
		if (slot.effectFlag) {
			if (i == 1 || i == 2 || IsCutsceneStreamTrack(slot.currentTrack))
				slot.mixVolume = 128 * slot.requestedVolume * m_nEffectsVolume >> 14;
			else
				slot.mixVolume = m_nEffectsFadeVolume * slot.requestedVolume * m_nEffectsVolume >> 14;
		}
	}
	for (uint32 i = 0; i < MAXCHANNELS + MAX2DCHANNELS; i++)
		UpdateSampleChannelVolume(gSampleChannels[i], m_nEffectsVolume, m_nEffectsFadeVolume);
#endif
}

void
cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
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
#ifdef GAMECUBE
	cAudioStateLock lock;
#endif
	m_nMonoMode = nMode;
}

bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

#ifdef GAMECUBE
	cAudioStateLock lock;
	if (!GC_AUDIO_ENABLED)
		return FALSE;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG)
		return nBank == SFX_BANK_0 || nBank == SFX_BANK_PED_COMMENTS;
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
	cAudioStateLock lock;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG)
		return;
	if (nBank < MAX_SFX_BANKS)
		gSampleBankLoaded[nBank] = FALSE;
#endif
}

int8
cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

#ifdef GAMECUBE
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG)
		return (nBank == SFX_BANK_0 || nBank == SFX_BANK_PED_COMMENTS) ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
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
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return LOADING_STATUS_NOT_LOADED;
	if (nSlot == MISSION_AUDIO_PLAYER_COMMENT)
		return IsPedCommentLoaded(nSample);
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
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	if (nSlot == MISSION_AUDIO_PLAYER_COMMENT)
		return LoadPedComment(nSample);
	return TRUE;
#else
	return FALSE;
#endif
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	ASSERT(nComment < TOTAL_AUDIO_SAMPLES);

#ifdef GAMECUBE
	cAudioStateLock lock;
	int32 loadedSlot = _GetPedCommentSlot(nComment);
	if (loadedSlot >= 0) {
		/* Keep the cache entry alive while AudioLogic transfers the comment
		 * from its loading queue to a mixer channel. The channel reference
		 * becomes the long-lived pin once InitialiseChannel succeeds. */
		gPedSlotClaimFrames[loadedSlot] = 4;
		return LOADING_STATUS_LOADED;
	}
	if (gPedCommentPending == nComment || gPedCommentLoading == nComment)
		return LOADING_STATUS_LOADING;
	return LOADING_STATUS_NOT_LOADED;
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
	if (!GC_AUDIO_ENABLED || !gPedCommentLoaderRunning || nComment >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		if (!IsPs2PedComment(nComment) || !gPs2SfxSdtValid[nComment] ||
		    m_aSamples[nComment].nSize > gPedSlotCapacityBytes)
			return FALSE;
	} else if (m_aSamples[nComment].nSize == 0 || m_aSamples[nComment].nSize > PED_BLOCKSIZE) {
		return FALSE;
	}

	cAudioStateLock lock;
	if (_GetPedCommentSlot(nComment) >= 0 || gPedCommentPending == nComment || gPedCommentLoading == nComment)
		return TRUE;
	if (gPedCommentPending == NO_SAMPLE) {
		gPedCommentPending = nComment;
		if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
			gPedCommentPendingOffset = gPs2SfxSdt[nComment].rawOffset;
			gPedCommentPendingSize = gPs2SfxSdt[nComment].allocatedBytes;
		} else {
			gPedCommentPendingOffset = m_aSamples[nComment].nOffset;
			gPedCommentPendingSize = m_aSamples[nComment].nSize;
		}
		SignalPedCommentLoader();
	}
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
	cAudioStateLock lock;
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
	cAudioStateLock lock;
	if (!GC_AUDIO_ENABLED || nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	u32 level;
	_CPU_ISR_Disable(level);
	bool8 hasPendingOutput = gSamplePendingOutputFrames[nChannel] != 0;
	_CPU_ISR_Restore(level);
	if (hasPendingOutput)
		return FALSE;

	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG)
		nBank = IsPs2PedComment(nSfx) ? SFX_BANK_PED_COMMENTS : SFX_BANK_0;
	else if (nBank >= MAX_SFX_BANKS)
		nBank = GetBankContainingSound(nSfx);

	if (nBank == INVALID_SFX_BANK)
		return FALSE;

	if (nBank == SFX_BANK_PED_COMMENTS) {
		uint8 loadStatus = IsPedCommentLoaded(nSfx);
		if (loadStatus != LOADING_STATUS_LOADED) {
			if (loadStatus == LOADING_STATUS_NOT_LOADED && !LoadPedComment(nSfx))
				return FALSE;
			if (IsPedCommentLoaded(nSfx) != LOADING_STATUS_LOADED)
				return FALSE;
		}
	} else if (IsSampleBankLoaded(nBank) != LOADING_STATUS_LOADED && !LoadSampleBank(nBank)) {
		return FALSE;
	}

	const int16 *sampleData = NULL;
	uint32 sampleCount = 0;
	if (gSfxBackend == GC_SFX_BACKEND_PS2_VAG) {
		uint32 sampleBytes = 0;
		if (!ResolvePs2SampleAddress(nSfx, sampleData, sampleCount, sampleBytes))
			return FALSE;
		m_aSamples[nSfx].nSize = sampleBytes;
	} else if (!ResolveSampleAddress(m_aSamples, nSfx, nBank, sampleData, sampleCount)) {
		return FALSE;
	}

	tGcSampleChannel &channel = gSampleChannels[nChannel];
	ResetSamplePlaybackClock(nChannel);
	ResetSampleChannel(channel);
	UpdateSampleChannelVolume(channel, m_nEffectsVolume, m_nEffectsFadeVolume);
	channel.initialised = TRUE;
	channel.is2D = nChannel >= NUM_CHANNELS_GENERIC;
	channel.effectFlag = !channel.is2D;
	channel.pan = 63;
	channel.baseFrequency = GetSampleBaseFrequency(nSfx);
	channel.frequency = channel.baseFrequency;
	channel.loopStartBytes = GetSampleLoopStartOffset(nSfx);
	channel.loopEndBytes = GetSampleLoopEndOffset(nSfx);
	channel.loopCount = 1;
	channel.sampleData = sampleData;
	channel.sampleCount = sampleCount;
	channel.cursorFixed = 0;
	channel.stepFixed = ComputeChannelStepFixed(channel.frequency);
	if (channel.stepFixed == 0)
		channel.stepFixed = ComputeChannelStepFixed(channel.baseFrequency != 0 ? channel.baseFrequency : DIGITALRATE);
	channel.edgeRampFrames = ComputeSampleEdgeRampFrames(channel.sampleCount, channel.stepFixed);
	if (nBank == SFX_BANK_PED_COMMENTS) {
		int32 pedSlot = _GetPedCommentSlot(nSfx);
		if (pedSlot >= 0)
			gPedSlotClaimFrames[pedSlot] = 0;
	}

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
	cAudioStateLock lock;
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	channel.requestedVolume = ClampVolume127(nVolume);
	UpdateSampleChannelVolume(channel, m_nEffectsVolume, m_nEffectsFadeVolume);
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
	cAudioStateLock lock;
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
	cAudioStateLock lock;
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
	cAudioStateLock lock;
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	channel.requestedVolume = ClampVolume127(nVolume);
	UpdateSampleChannelVolume(channel, m_nEffectsVolume, m_nEffectsFadeVolume);
#else
	(void)nVolume;
#endif
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	cAudioStateLock lock;
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
	cAudioStateLock lock;
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	channel.frequency = nFreq;
	channel.stepFixed = ComputeChannelStepFixed(nFreq);
	channel.edgeRampFrames = ComputeSampleEdgeRampFrames(channel.sampleCount, channel.stepFixed);
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
	cAudioStateLock lock;
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
	cAudioStateLock lock;
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
	cAudioStateLock lock;
	u32 level;
	_CPU_ISR_Disable(level);
	bool8 hasPendingOutput = gSamplePendingOutputFrames[nChannel] != 0;
	_CPU_ISR_Restore(level);
	return gSampleChannels[nChannel].active || hasPendingOutput;
#else
	return FALSE;
#endif
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	cAudioStateLock lock;
	tGcSampleChannel &channel = gSampleChannels[nChannel];
	/* Invalidate any producer snapshot taken before this start. */
	ResetSamplePlaybackClock(nChannel);
	if (!GC_AUDIO_ENABLED || !channel.initialised || channel.sampleData == NULL || channel.sampleCount == 0 || channel.stepFixed == 0) {
		channel.active = FALSE;
		return;
	}

	channel.active = TRUE;
	channel.paused = FALSE;
	RequestAudioFillLocked();
#endif
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS + MAX2DCHANNELS);

#ifdef GAMECUBE
	cAudioStateLock lock;
	/* The DMA mixer may still contain this channel after rendering stops. */
	ResetSamplePlaybackClock(nChannel);
	ResetSampleChannel(gSampleChannels[nChannel]);
#endif
}

bool8
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return FALSE;
	if (nFile >= TOTAL_STREAMED_SOUNDS)
		return FALSE;

	CGcStreamDecoder *decoder = OpenTrackDecoder(nFile);
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	if (gStreamSlots[nStream].active || StreamHasQueuedReadyAudio(nStream))
		InvalidateReadyDmaBuffers();
	CloseStreamSlot(nStream);
	tGcStreamSlot &slot = gStreamSlots[nStream];
	slot.decoder = decoder;
	slot.currentTrack = nFile;
	if (slot.decoder == NULL)
		return FALSE;

	slot.preloaded = TRUE;
	slot.loop = gStreamLoopedFlag[nStream];
	slot.lengthMs = slot.decoder->GetLengthMS();
	slot.requestedVolume = MAX_VOLUME;
	slot.pan = 63;
	slot.mixVolume = MAX_VOLUME;
	slot.effectFlag = FALSE;
	slot.ResetRuntime();
	slot.currentTrack = nFile;
	if (IsCutsceneStreamTrack(nFile)) {
		/* PS2 VB cutscene streams are converted to Wii IDSP. Decode a full
		 * source-buffer window while the cutscene is still loading so its first
		 * DMA blocks cannot race the decoder thread. */
		ResetSlotForPlayback(nStream, 0);
		if (!RefillSourceBuffer(slot, GC_STREAM_DECODE_CHUNK_FRAMES * 2)) {
			printf("[CUT-AUDIO] preload failed track=%u lengthMs=%u path=%s pcmFrames=%u\n",
			       uint32(nFile), slot.lengthMs, GetTrackPath(nFile) != NULL ? GetTrackPath(nFile) : "<missing>",
			       slot.sourceFramesValid);
			CloseStreamSlot(nStream);
			return FALSE;
		}
		printf("[CUT-AUDIO] preload track=%u lengthMs=%u path=%s pcmFrames=%u\n",
		       uint32(nFile), slot.lengthMs, GetTrackPath(nFile) != NULL ? GetTrackPath(nFile) : "<missing>",
		       slot.sourceFramesValid);
	}
	return TRUE;
#else
	(void)nFile;
	(void)nStream;
	return FALSE;
#endif
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	if (gStreamSlots[nStream].decoder && gStreamSlots[nStream].paused != (nPauseFlag != FALSE)) {
		if (gStreamSlots[nStream].active || StreamHasQueuedReadyAudio(nStream))
			InvalidateReadyDmaBuffers();
		gStreamSlots[nStream].paused = nPauseFlag != FALSE;
	}
	SignalStreamDecoder();
	RequestAudioFillLocked();
#endif
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	if (!GC_AUDIO_ENABLED)
		return;
	tGcStreamSlot &slot = gStreamSlots[nStream];
	if (!slot.decoder)
		return;

	if (slot.active || StreamHasQueuedReadyAudio(nStream))
		InvalidateReadyDmaBuffers();
	if (!IsCutsceneStreamTrack(slot.currentTrack) || slot.sourceFramesValid == 0 || slot.sourceFramePos != 0)
		ResetSlotForPlayback(nStream, 0);
	else {
		/* PreloadStreamedFile already decoded the first source window. Keep it;
		 * seeking here would discard the only data that made startup deterministic. */
		ResetStreamPlaybackClock(nStream);
		slot.startPosMs = 0;
		slot.resampleFrac = 0;
	}
	slot.active = TRUE;
	slot.paused = FALSE;
	slot.preloaded = TRUE;
	SignalStreamDecoder();
	RequestAudioFillLocked();
#endif
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	if (!GC_AUDIO_ENABLED)
		return FALSE;
	if (nFile >= TOTAL_STREAMED_SOUNDS || !IsSupportedGcStreamTrack(nFile))
		return FALSE;

	CGcStreamDecoder *decoder = NULL;
	if (!gStreamDecoderRunning) {
		decoder = OpenTrackDecoder(nFile);
		if (decoder == NULL)
			return FALSE;
	}
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	if (gStreamSlots[nStream].active || StreamHasQueuedReadyAudio(nStream))
		InvalidateReadyDmaBuffers();
	CloseStreamSlot(nStream);
	tGcStreamSlot &slot = gStreamSlots[nStream];
	slot.decoder = decoder;
	slot.preloaded = decoder != NULL;
	slot.currentTrack = nFile;
	slot.loop = gStreamLoopedFlag[nStream];
	slot.lengthMs = decoder != NULL ? decoder->GetLengthMS() : 0;
	slot.requestedVolume = MAX_VOLUME;
	slot.pan = 63;
	/* The caller applies the station volume immediately after StartStreamedFile.
	 * Start silent so a higher-priority producer cannot queue one full-volume
	 * block before that setter runs. */
	slot.mixVolume = 0;
	slot.effectFlag = FALSE;
	if (decoder != NULL) {
		ResetSlotForPlayback(nStream, nPos);
	} else {
		slot.ResetRuntime();
		slot.openPending = TRUE;
		slot.pendingTrack = nFile;
		slot.pendingStartPosMs = nPos;
		slot.startPosMs = nPos;
	}
	slot.active = TRUE;
	slot.paused = FALSE;
	SignalStreamDecoder();
	RequestAudioFillLocked();
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
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	if (gStreamSlots[nStream].active || StreamHasQueuedReadyAudio(nStream))
		InvalidateReadyDmaBuffers();
	CloseStreamSlot(nStream);
	RequestAudioFillLocked();
#endif
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	cAudioStateLock lock;
	tGcStreamSlot &slot = gStreamSlots[nStream];
	if (!slot.decoder)
		return 0;

	u32 level;
	_CPU_ISR_Disable(level);
	uint64 playedOutputFrames = gStreamPlayedOutputFrames[nStream];
	_CPU_ISR_Restore(level);
	uint32 playedMs = SamplesToMs(GC_OUTPUT_RATE, playedOutputFrames);
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
	cAudioStateLock lock;
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
		if (nStream == 1 || nStream == 2 || IsCutsceneStreamTrack(slot.currentTrack))
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
cSampleManager::GetStreamedFileLength(tTrack nFile)
{
	ASSERT(nFile < TOTAL_STREAMED_SOUNDS);

#ifdef GAMECUBE
	if (gStreamLength[nFile] <= 1) {
		uint32 length = 0;
		if (ProbeTrackLength(nFile, length) && length != 0)
			gStreamLength[nFile] = length;
	}
	return gStreamLength[nFile];
#else
	return 1;
#endif
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

#ifdef GAMECUBE
	cAudioStateLock lock;
	return gStreamSlots[nStream].active;
#else
	return FALSE;
#endif
}

void
cSampleManager::Service(void)
{
#ifdef GAMECUBE
	if (gAudioProducerRunning) {
		SignalAudioProducer();
	} else {
		TryFillReadyBuffers();
	}
	{
		cAudioStateLock lock;
		for (uint32 i = 0; i < MAX_PEDSFX; i++) {
			if (gPedSlotClaimFrames[i] != 0)
				gPedSlotClaimFrames[i]--;
		}
	}
	SignalPedCommentLoader();
#if GC_AUDIO_DEBUG_LOG
	LogDmaDiagnostics();
#endif
#endif
}

bool8
cSampleManager::IsUsingPs2SfxBanks(void) const
{
#ifdef GAMECUBE
	return gSfxBackend == GC_SFX_BACKEND_PS2_VAG;
#else
	return FALSE;
#endif
}

bool8
cSampleManager::InitialiseSampleBanks(void)
{
#ifdef GAMECUBE
	/* Initialisation runs before the producer thread is started. */
	if (!GC_AUDIO_ENABLED)
		return TRUE;

	CloseSampleBankFiles();
	FreeSampleBankMemory();
	ResetSampleBankState();
	gSfxBackend = GC_SFX_BACKEND_UNSELECTED;

	/* Only a PC bank in the Audio root selects PC PCM. The normal VFS lookup
	 * is recursive for legacy bare filenames, so use an exact FST probe here. */
	bool pcLayoutPresent = GCM_VFS_FileExistsExact("audio/sfx.sdt");
	bool ps2LayoutPresent = GCM_VFS_FileExistsExact("audio/sfx/set0/sfx2.sdt");
	printf("[GC-AUDIO] SFX layout probe: pcRoot=%d ps2Set0=%d\n",
	       pcLayoutPresent ? 1 : 0, ps2LayoutPresent ? 1 : 0);
	if (pcLayoutPresent) {
		gSfxBackend = GC_SFX_BACKEND_PC_PCM;
	} else {
#if GC_PS2_SFX_BANK_ENABLE
		gSfxBackend = GC_SFX_BACKEND_PS2_VAG;
		if (!InitialisePs2SampleBanks(m_aSamples)) {
			printf("[GC-AUDIO] ERROR: PS2 SFX backend initialisation failed\n");
			return FALSE;
		}
		return TRUE;
#else
		printf("[GC-AUDIO] ERROR: AUDIO\\SFX.SDT missing and PS2 SFX backend disabled\n");
		return FALSE;
#endif
	}

	if (BankStartOffset[SFX_BANK_PED_COMMENTS] <= BankStartOffset[SFX_BANK_0]) {
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

	gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = AllocAlignedAudioBuffer(gPedSlotCapacityBytes * MAX_PEDSFX);
	if (gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] == NULL) {
		printf("[GC-AUDIO] ERROR: alloc failed for ped comments bank (%u bytes)\n", gPedSlotCapacityBytes * MAX_PEDSFX);
		FreeSampleBankMemory();
		CloseSampleBankFiles();
		return FALSE;
	}
	memset(gSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS], 0, gPedSlotCapacityBytes * MAX_PEDSFX);

	if (!LoadSampleBank(SFX_BANK_0)) {
		printf("[GC-AUDIO] ERROR: initial SFX bank load failed off=%u size=%u\n",
		       gSampleBankDiscStartOffset[SFX_BANK_0],
		       gSampleBankSize[SFX_BANK_0]);
		FreeSampleBankMemory();
		CloseSampleBankFiles();
		return FALSE;
	}
	printf("[GC-AUDIO] SFX backend: PC PCM (AUDIO\\SFX.SDT/SFX.RAW)\n");
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
	cStreamDecodeLock decodeLock;
	cAudioFillLock fillLock;
	cAudioStateLock lock;
	gStreamLoopedFlag[nChannel] = nLoopFlag != FALSE;
	gStreamSlots[nChannel].loop = nLoopFlag != FALSE;
#endif
}

#ifdef EXTERNAL_3D_SOUND
int8 cSampleManager::AutoDetect3DProviders()
{
	return GC_AUDIO_ENABLED ? 0 : -1;
}
#endif

#endif
