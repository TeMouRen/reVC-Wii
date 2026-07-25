#pragma once

#include "common.h"

struct GcIdspAdpcmHist
{
	int16 hist1;
	int16 hist2;
};

struct GcIdspBlockDecodeRequest
{
	const uint8 *channelFrames[2];
	const int16 *coefficients[2];
	GcIdspAdpcmHist initialHistory[2];
	GcIdspAdpcmHist currentHistory[2];
	uint32 channelCount;
	uint32 frameCount;
	uint32 sampleCount;
	int16 *output;
	uint32 outputCapacityFrames;
	uint32 outputFramesWritten;
};

class CGcIdspDspTask
{
public:
	enum {
		MAX_FRAME_COUNT = 62,
		MAX_SAMPLE_COUNT = MAX_FRAME_COUNT * 14
	};

	static bool Initialize();
	static void Shutdown();
	static bool IsAvailable();
	static bool DecodeBlock(GcIdspBlockDecodeRequest &request);

#ifdef WII
	static const struct _dsp_task *GetTask();
#endif
};
