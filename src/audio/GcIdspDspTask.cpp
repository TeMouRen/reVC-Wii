#include "common.h"
#include "GcIdspDspTask.h"

#if defined(WII)
#include <ogc/dsp.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <unistd.h>
#include "gc_idsp_decode.h"
#endif

namespace
{
/* The output contains interleaved stereo PCM followed by four history words. */
static const uint32 GC_IDSP_DSP_COMMAND_WORDS = 64;
static const uint32 GC_IDSP_DSP_OUTPUT_WORDS = CGcIdspDspTask::MAX_SAMPLE_COUNT * 2 + 4;
static const uint32 GC_IDSP_DSP_OUTPUT_BYTES = GC_IDSP_DSP_OUTPUT_WORDS * sizeof(uint16);
static const uint32 GC_IDSP_DSP_MAIL_MAGIC = 0x1d5a0001;
static const uint32 GC_IDSP_DSP_MAIL_SHUTDOWN = 0x1d5a0002;

#if defined(WII)
struct GcIdspDspCommand
{
	uint16 words[GC_IDSP_DSP_COMMAND_WORDS];
} ATTRIBUTE_ALIGN(32);

struct GcIdspDspRuntime
{
	dsptask_t task;
	GcIdspDspCommand command;
	uint8 input[2][CGcIdspDspTask::MAX_FRAME_COUNT * 8] ATTRIBUTE_ALIGN(32);
	uint16 output[GC_IDSP_DSP_OUTPUT_WORDS] ATTRIBUTE_ALIGN(32);
	volatile bool initialized;
	volatile bool available;
	volatile bool ready;
	volatile bool requestDone;
	volatile bool taskDone;
	bool diagnosticLogged;
};

static GcIdspDspRuntime gRuntime;

static void SetCommandU32(uint32 word, uint32 value)
{
	gRuntime.command.words[word + 0] = uint16(value >> 16);
	gRuntime.command.words[word + 1] = uint16(value & 0xffff);
}

static void SetCommandS16(uint32 word, int16 value)
{
	gRuntime.command.words[word] = uint16(value);
}

static uint32 PhysicalAddress(const void *address)
{
	return uint32(MEM_VIRTUAL_TO_PHYSICAL(address));
}

static void DspInitCallback(dsptask_t *)
{
	gRuntime.ready = true;
}

static void DspResumeCallback(dsptask_t *)
{
}

static void DspDoneCallback(dsptask_t *)
{
	gRuntime.taskDone = true;
	gRuntime.available = false;
}

static void DspRequestCallback(dsptask_t *)
{
	/* libogc consumes DCD10004 before invoking this callback. */
	gRuntime.requestDone = true;
}

static bool SendMail(u32 mail, uint32 timeoutMs)
{
	u64 start = gettime();
	while (DSP_CheckMailTo()) {
		if (ticks_to_millisecs(gettime() - start) >= timeoutMs)
			return false;
		LWP_YieldThread();
	}
	DSP_SendMailTo(mail);
	start = gettime();
	while (DSP_CheckMailTo()) {
		if (ticks_to_millisecs(gettime() - start) >= timeoutMs)
			return false;
		LWP_YieldThread();
	}
	return true;
}

static bool WaitForFlag(volatile bool &flag, uint32 timeoutMs)
{
	u64 start = gettime();
	while (!flag && ticks_to_millisecs(gettime() - start) < timeoutMs) {
		LWP_YieldThread();
	}
	return flag;
}

static bool WaitForDspReady()
{
	return WaitForFlag(gRuntime.ready, 1000);
}

static void FillCommand(const GcIdspBlockDecodeRequest &request)
{
	memset(&gRuntime.command, 0, sizeof(gRuntime.command));

	/* DSP ADPCM accelerator addresses are nibble addresses. Each compressed
	 * byte therefore advances the address by two, while the DMA output address
	 * below remains a normal byte address. */
	uint32 leftAddress = PhysicalAddress(gRuntime.input[0]) << 1;
	uint32 rightAddress = PhysicalAddress(gRuntime.input[1]) << 1;
	uint32 lastSample = request.sampleCount - 1U;
	uint32 endOffset = 2U + (lastSample / 14U) * 16U + lastSample % 14U;
	SetCommandU32(0, leftAddress);
	SetCommandU32(2, leftAddress + endOffset);
	SetCommandU32(4, leftAddress + 2U);
	SetCommandU32(6, rightAddress);
	SetCommandU32(8, rightAddress + endOffset);
	SetCommandU32(10, rightAddress + 2U);
	SetCommandU32(12, PhysicalAddress(gRuntime.output));
	gRuntime.command.words[14] = uint16(request.sampleCount);
	gRuntime.command.words[15] = uint16(request.frameCount);
	gRuntime.command.words[16] = uint16(request.channelCount);
	gRuntime.command.words[17] = request.channelFrames[0][0];
	gRuntime.command.words[18] = request.channelCount > 1 ? request.channelFrames[1][0] : request.channelFrames[0][0];
	SetCommandS16(19, request.initialHistory[0].hist1);
	SetCommandS16(20, request.initialHistory[0].hist2);
	SetCommandS16(21, request.channelCount > 1 ? request.initialHistory[1].hist1 : request.initialHistory[0].hist1);
	SetCommandS16(22, request.channelCount > 1 ? request.initialHistory[1].hist2 : request.initialHistory[0].hist2);
	for (uint32 i = 0; i < 16; i++) {
		SetCommandS16(23 + i, request.coefficients[0][i]);
		SetCommandS16(39 + i, request.channelCount > 1 ? request.coefficients[1][i] : request.coefficients[0][i]);
	}
}
#endif
}

bool CGcIdspDspTask::Initialize()
{
#if defined(WII)
	if (gRuntime.initialized)
		return gRuntime.available;

	memset(&gRuntime, 0, sizeof(gRuntime));
	gRuntime.initialized = true;
	if (gc_idsp_decode_size == 0)
		return false;

	DSP_Init();
	DCFlushRange((void *)gc_idsp_decode, gc_idsp_decode_size);
	gRuntime.task.state = DSPTASK_INIT;
	gRuntime.task.prio = 0;
	gRuntime.task.flags = DSPTASK_CLEARALL;
	gRuntime.task.iram_maddr = (void *)MEM_VIRTUAL_TO_PHYSICAL((void *)gc_idsp_decode);
	gRuntime.task.iram_len = gc_idsp_decode_size;
	gRuntime.task.iram_addr = 0x0000;
	gRuntime.task.dram_maddr = NULL;
	gRuntime.task.dram_len = 0;
	gRuntime.task.dram_addr = 0x0000;
	gRuntime.task.init_vec = 0x0010;
	gRuntime.task.resume_vec = 0x0000;
	gRuntime.task.init_cb = DspInitCallback;
	gRuntime.task.res_cb = DspResumeCallback;
	gRuntime.task.done_cb = DspDoneCallback;
	gRuntime.task.req_cb = DspRequestCallback;
	gRuntime.task.next = NULL;
	gRuntime.task.prev = NULL;

	if (DSP_AddTask(&gRuntime.task) == NULL || !WaitForDspReady()) {
		DSP_CancelTask(&gRuntime.task);
		DSP_Reset();
		gRuntime.available = false;
		return false;
	}
	gRuntime.available = true;
	return true;
#else
	return false;
#endif
}

void CGcIdspDspTask::Shutdown()
{
#if defined(WII)
	if (!gRuntime.initialized)
		return;
	if (gRuntime.ready && !gRuntime.taskDone) {
		gRuntime.taskDone = false;
		if (!SendMail(GC_IDSP_DSP_MAIL_SHUTDOWN, 100) || !WaitForFlag(gRuntime.taskDone, 1000)) {
			DSP_CancelTask(&gRuntime.task);
			DSP_Reset();
		}
	}
	memset(&gRuntime, 0, sizeof(gRuntime));
#endif
}

bool CGcIdspDspTask::IsAvailable()
{
#if defined(WII)
	return gRuntime.available && gRuntime.ready;
#else
	return false;
#endif
}

bool CGcIdspDspTask::DecodeBlock(GcIdspBlockDecodeRequest &request)
{
#if defined(WII)
	request.outputFramesWritten = 0;
	if (!IsAvailable() || request.output == NULL || request.outputCapacityFrames == 0 ||
	    request.channelCount == 0 || request.channelCount > 2 || request.frameCount == 0 ||
	    request.frameCount > CGcIdspDspTask::MAX_FRAME_COUNT || request.sampleCount == 0 ||
	    request.sampleCount > CGcIdspDspTask::MAX_SAMPLE_COUNT || request.sampleCount > request.frameCount * 14U ||
	    request.outputCapacityFrames < request.sampleCount ||
	    request.channelFrames[0] == NULL || request.coefficients[0] == NULL ||
	    (request.channelCount > 1 && (request.channelFrames[1] == NULL || request.coefficients[1] == NULL)))
		return false;

	uint32 compressedBytes = request.frameCount * 8U;
	memcpy(gRuntime.input[0], request.channelFrames[0], compressedBytes);
	if (request.channelCount > 1)
		memcpy(gRuntime.input[1], request.channelFrames[1], compressedBytes);
	else
		memcpy(gRuntime.input[1], request.channelFrames[0], compressedBytes);
	FillCommand(request);
	DCFlushRange(gRuntime.input[0], compressedBytes);
	DCFlushRange(gRuntime.input[1], compressedBytes);
	DCFlushRange(&gRuntime.command, sizeof(gRuntime.command));
	/* Keep a sentinel in the unused part of the output buffer.  On the first
	 * validation failure this distinguishes a real zero sample from a DMA that
	 * wrote fewer bytes than the requested batch. */
	if (!gRuntime.diagnosticLogged)
		memset(gRuntime.output, 0x5a, GC_IDSP_DSP_OUTPUT_BYTES);
	DCFlushRange(gRuntime.output, GC_IDSP_DSP_OUTPUT_BYTES);

	gRuntime.requestDone = false;
	if (!SendMail(GC_IDSP_DSP_MAIL_MAGIC, 100)) {
		printf("[GC-AUDIO] WARN: IDSP DSP mailbox timeout stage=command-magic\n");
		gRuntime.available = false;
		return false;
	}
	if (!SendMail(PhysicalAddress(&gRuntime.command), 100)) {
		printf("[GC-AUDIO] WARN: IDSP DSP mailbox timeout stage=command-address\n");
		gRuntime.available = false;
		return false;
	}
	if (!WaitForFlag(gRuntime.requestDone, 1000)) {
		printf("[GC-AUDIO] WARN: IDSP DSP mailbox timeout stage=decode-complete\n");
		gRuntime.available = false;
		return false;
	}
	DCInvalidateRange(gRuntime.output, GC_IDSP_DSP_OUTPUT_BYTES);
	if (!gRuntime.diagnosticLogged) {
		uint32 stateOffset = request.sampleCount * 2;
		uint32 probeState = stateOffset + 3 < GC_IDSP_DSP_OUTPUT_WORDS ? stateOffset : 0;
		printf("[GC-AUDIO] IDSP probe input=%02x%02x%02x%02x addrL=%08x endL=%08x curL=%08x "
		       "cmdSamples=%u cmdFrames=%u pred=%02x state=%04x,%04x,%04x,%04x tail=%04x\n",
		       gRuntime.input[0][0], gRuntime.input[0][1], gRuntime.input[0][2], gRuntime.input[0][3],
		       (uint32(gRuntime.command.words[0]) << 16) | gRuntime.command.words[1],
		       (uint32(gRuntime.command.words[2]) << 16) | gRuntime.command.words[3],
		       (uint32(gRuntime.command.words[4]) << 16) | gRuntime.command.words[5],
		       gRuntime.command.words[14], gRuntime.command.words[15], gRuntime.command.words[17],
		       gRuntime.output[probeState + 0], gRuntime.output[probeState + 1],
		       gRuntime.output[probeState + 2], gRuntime.output[probeState + 3],
		       gRuntime.output[GC_IDSP_DSP_OUTPUT_WORDS - 1]);
		gRuntime.diagnosticLogged = true;
	}

	uint32 frames = request.sampleCount;
	memcpy(request.output, gRuntime.output, frames * sizeof(int16) * 2);
	uint32 stateOffset = frames * 2;
	request.currentHistory[0].hist1 = int16(gRuntime.output[stateOffset + 0]);
	request.currentHistory[0].hist2 = int16(gRuntime.output[stateOffset + 1]);
	request.currentHistory[1].hist1 = int16(gRuntime.output[stateOffset + 2]);
	request.currentHistory[1].hist2 = int16(gRuntime.output[stateOffset + 3]);
	request.outputFramesWritten = frames;
	return true;
#else
	(void)request;
	return false;
#endif
}

#if defined(WII)
const dsptask_t *CGcIdspDspTask::GetTask()
{
	return gRuntime.available ? &gRuntime.task : NULL;
}
#endif
