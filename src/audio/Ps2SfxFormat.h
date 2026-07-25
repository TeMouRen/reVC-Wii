#ifndef __GTA_PS2_SFX_FORMAT_H__
#define __GTA_PS2_SFX_FORMAT_H__

#include <stdint.h>

#define PS2_SFX_SDT_RECORD_SIZE 12U
#define PS2_SFX_VAG_FRAME_SIZE 16U
#define PS2_SFX_VAG_SAMPLES_PER_FRAME 28U
#define PS2_SFX_MISSION_FIRST_TRACK_ID 103U
#define PS2_SFX_MISSION_LAST_TRACK_ID 1223U
#define PS2_SFX_MISSION_ENTRY_COUNT (PS2_SFX_MISSION_LAST_TRACK_ID - PS2_SFX_MISSION_FIRST_TRACK_ID + 1U)
#define PS2_SFX_MISSION_FIRST_SDT_ENTRY 9417U
#define PS2_SFX_MISSION_LAST_SDT_ENTRY 10562U
#define PS2_SFX_BUST_FIRST_TRACK_ID 1196U
#define PS2_SFX_BUST_LAST_TRACK_ID 1223U
#define PS2_SFX_BUST_ENTRY_COUNT (PS2_SFX_BUST_LAST_TRACK_ID - PS2_SFX_BUST_FIRST_TRACK_ID + 1U)
#define PS2_SFX_BUST_FIRST_SDT_ENTRY 10535U
#define PS2_SFX_BUST_LAST_SDT_ENTRY 10562U
#define PS2_SFX_POLICE_FIRST_SDT_ENTRY 10563U
#define PS2_SFX_POLICE_ENTRY_COUNT 68U
#define PS2_SFX_POLICE_LAST_SDT_ENTRY (PS2_SFX_POLICE_FIRST_SDT_ENTRY + PS2_SFX_POLICE_ENTRY_COUNT - 1U)
#define PS2_SFX_INVALID_SDT_ENTRY 0xFFFFFFFFU
#define PS2_SFX_BANK_COUNT 67U
#define PS2_SFX_BANK_PATH_CAPACITY 96U

/* The PS2 mission assets occupy one ordered region in sfx2, but the bank also
 * contains 25 extra records that are not present in the streamed-track enum.
 * These are repeated Hilary/Mercedes voice data, not 25 unique PS2 lines. */
static const uint16_t Ps2SfxMissionSkippedSdtEntries[] = {
	9536U, 9538U,
	9609U, 9610U, 9611U, 9612U,
	10061U, 10062U, 10063U, 10064U, 10065U, 10066U, 10067U,
	10415U, 10416U, 10417U, 10418U, 10419U, 10420U, 10421U, 10422U,
	10424U, 10425U, 10426U, 10427U,
};

static inline uint32_t Ps2SfxMissionOrdinalToSdtEntry(uint32_t ordinal)
{
	if (ordinal >= PS2_SFX_MISSION_ENTRY_COUNT)
		return PS2_SFX_INVALID_SDT_ENTRY;

	uint32_t entry = PS2_SFX_MISSION_FIRST_SDT_ENTRY + ordinal;
	for (uint32_t i = 0; i < sizeof(Ps2SfxMissionSkippedSdtEntries) / sizeof(Ps2SfxMissionSkippedSdtEntries[0]); i++) {
		if (Ps2SfxMissionSkippedSdtEntries[i] <= entry)
			entry++;
	}
	return entry;
}

static inline uint32_t ReadPs2SfxLe32(const uint8_t *bytes)
{
	return uint32_t(bytes[0]) |
	       (uint32_t(bytes[1]) << 8) |
	       (uint32_t(bytes[2]) << 16) |
	       (uint32_t(bytes[3]) << 24);
}

static inline bool IsPs2VagEndFrame(uint8_t flags)
{
	return (flags & 0x01U) != 0;
}

static inline bool IsSupportedPs2MissionVagFlags(uint8_t flags)
{
	return (flags & ~0x07U) == 0;
}

static inline bool IsPs2VagPostEndPadding(uint8_t flags)
{
	return flags == 0 || flags == 7;
}

static inline bool IsSupportedPs2VagPredictor(uint8_t predictorAndShift)
{
	return (predictorAndShift >> 4) < 5;
}

#endif
