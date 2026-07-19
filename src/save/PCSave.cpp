#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"

#include "FileMgr.h"
#include "Font.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "GenericGameStorage.h"
#include "Messages.h"
#include "PCSave.h"
#include "Text.h"
#ifdef WII
#include "wii_save.h"
#endif

const char* _psGetUserFilesFolder();

C_PcSave PcSaveHelper;

static bool
EnsureSaveDirectoryInitialized()
{
	if (DefaultPCSaveFileName[0] != '\0')
		return true;

	const char *path = _psGetUserFilesFolder();
	if (path != nil && path[0] != '\0')
		C_PcSave::SetSaveDirectory(path);
	return DefaultPCSaveFileName[0] != '\0';
}

void
C_PcSave::SetSaveDirectory(const char *path)
{
#if defined(ANDROID) || defined(WII)
	sprintf(DefaultPCSaveFileName, "%s/%s", path, "GTAVCsf");
    debug("SetSaveDirectory: %s", DefaultPCSaveFileName);
#else
    sprintf(DefaultPCSaveFileName, "%s\\%s", path, "GTAVCsf");
#endif
}

bool
C_PcSave::DeleteSlot(int32 slot)
{
#ifdef FIX_BUGS
	char FileName[MAX_PATH];
#else
	char FileName[200];
#endif

	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	if (!EnsureSaveDirectoryInitialized())
		return false;
	sprintf(FileName, "%s%i.b", DefaultPCSaveFileName, slot + 1);
#ifdef WII
	bool deleted = WiiSaveDeleteFile(FileName);
#else
	DeleteFile(FileName);
	bool deleted = true;
#endif
	SlotSaveDate[slot][0] = '\0';
	return deleted;
}

int8
C_PcSave::SaveSlot(int32 slot)
{
	if (!EnsureSaveDirectoryInitialized()) {
		PcSaveHelper.nErrorCode = SAVESTATUS_ERR_SAVE_CREATE;
		return 2;
	}
	MakeValidSaveName(slot);
#ifdef WII
#ifdef FIX_BUGS
	char finalSaveName[MAX_PATH];
	char tempSaveName[MAX_PATH];
#else
	char finalSaveName[200];
	char tempSaveName[200];
#endif
	strncpy(finalSaveName, ValidSaveName, sizeof(finalSaveName) - 1);
	finalSaveName[sizeof(finalSaveName) - 1] = '\0';
	if (!WiiSaveBuildTempPath(finalSaveName, tempSaveName, sizeof(tempSaveName))) {
		SYS_Report("[reVC-WII] SaveSlot temp-path build failed: slot=%d final=%s\n", slot, finalSaveName);
		PcSaveHelper.nErrorCode = SAVESTATUS_ERR_SAVE_CREATE;
		return 2;
	}
	WiiSaveDeleteFile(tempSaveName);
	strncpy(ValidSaveName, tempSaveName, sizeof(ValidSaveName) - 1);
	ValidSaveName[sizeof(ValidSaveName) - 1] = '\0';
	SYS_Report("[reVC-WII] SaveSlot begin: slot=%d temp=%s final=%s\n", slot, tempSaveName, finalSaveName);
#endif
	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	_psGetUserFilesFolder();
	int file = CFileMgr::OpenFile(ValidSaveName, "wb");
	if (file != 0) {
#ifdef MISSION_REPLAY
		if (!IsQuickSave)
#endif
			DoGameSpecificStuffBeforeSave();
		if (GenericSave(file)) {
			if (!!CFileMgr::CloseFile(file))
				nErrorCode = SAVESTATUS_ERR_SAVE_CLOSE;
#ifdef WII
			if (nErrorCode == SAVESTATUS_SUCCESSFUL) {
				strncpy(ValidSaveName, finalSaveName, sizeof(ValidSaveName) - 1);
				ValidSaveName[sizeof(ValidSaveName) - 1] = '\0';
				if (!WiiSaveCommitTempFile(tempSaveName, finalSaveName)) {
					SYS_Report("[reVC-WII] SaveSlot commit failed: slot=%d temp=%s final=%s\n", slot, tempSaveName, finalSaveName);
					WiiSaveDeleteFile(tempSaveName);
					nErrorCode = SAVESTATUS_ERR_SAVE_CLOSE;
					return 2;
				}
				SYS_Report("[reVC-WII] SaveSlot success: slot=%d final=%s\n", slot, finalSaveName);
				strncpy(SaveFileNameJustSaved, finalSaveName, sizeof(SaveFileNameJustSaved) - 1);
				SaveFileNameJustSaved[sizeof(SaveFileNameJustSaved) - 1] = '\0';
			} else {
				SYS_Report("[reVC-WII] SaveSlot close failed: slot=%d temp=%s err=%d\n", slot, tempSaveName, nErrorCode);
				WiiSaveDeleteFile(tempSaveName);
				strncpy(ValidSaveName, finalSaveName, sizeof(ValidSaveName) - 1);
				ValidSaveName[sizeof(ValidSaveName) - 1] = '\0';
				return 2;
			}
#endif
			return 0;
		}

#ifdef WII
		SYS_Report("[reVC-WII] SaveSlot GenericSave failed: slot=%d temp=%s err=%d\n", slot, tempSaveName, nErrorCode);
		WiiSaveDeleteFile(tempSaveName);
		strncpy(ValidSaveName, finalSaveName, sizeof(ValidSaveName) - 1);
		ValidSaveName[sizeof(ValidSaveName) - 1] = '\0';
#endif
		CFileMgr::CloseFile(file);
		return 2;
	}
	PcSaveHelper.nErrorCode = SAVESTATUS_ERR_SAVE_CREATE;
#ifdef WII
	SYS_Report("[reVC-WII] SaveSlot open failed: slot=%d temp=%s\n", slot, tempSaveName);
	strncpy(ValidSaveName, finalSaveName, sizeof(ValidSaveName) - 1);
	ValidSaveName[sizeof(ValidSaveName) - 1] = '\0';
#endif
	return 2;
}

bool
C_PcSave::PcClassSaveRoutine(int32 file, uint8 *data, uint32 size)
{
	CFileMgr::Write(file, (const char*)&size, sizeof(size));
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
#ifdef WII
		SYS_Report("[reVC-WII] Save write failed on block-size write: path=%s size=%u\n", ValidSaveName, size);
#endif
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	CFileMgr::Write(file, (const char*)data, align4bytes(size));
	CheckSum += (uint8) size;
	CheckSum += (uint8) (size >> 8);
	CheckSum += (uint8) (size >> 16);
	CheckSum += (uint8) (size >> 24);
	for (int i = 0; i < align4bytes(size); i++) {
		CheckSum += *data++;
	}
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
#ifdef WII
		SYS_Report("[reVC-WII] Save write failed on block-data write: path=%s size=%u\n", ValidSaveName, size);
#endif
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	return true;
}

void
C_PcSave::PopulateSlotInfo()
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		Slots[i] = SLOT_EMPTY;
		SlotFileName[i][0] = '\0';
		SlotSaveDate[i][0] = '\0';
	}
	if (!EnsureSaveDirectoryInitialized())
		return;
	for (int i = 0; i < SLOT_COUNT; i++) {
#ifdef FIX_BUGS
		char savename[MAX_PATH];
#else
		char savename[52];
#endif
		struct {
			int size;
			wchar FileName[24];
			SYSTEMTIME SaveDateTime;
		} header;
		sprintf(savename, "%s%i%s", DefaultPCSaveFileName, i + 1, ".b");
		int file = CFileMgr::OpenFile(savename, "rb");
		if (file != 0) {
			CFileMgr::Read(file, (char*)&header, sizeof(header));
			if (strncmp((char*)&header, TopLineEmptyFile, sizeof(TopLineEmptyFile)-1) != 0) {
				Slots[i] = SLOT_OK;
				memcpy(SlotFileName[i], &header.FileName, sizeof(header.FileName));
				
				SlotFileName[i][24] = '\0';
#ifdef WII
				SYS_Report("[reVC-WII] Slot scan header: slot=%d path=%s title=%s\n",
					i, savename, UnicodeToAsciiForSaveLoad(header.FileName));
#endif
			}
			CFileMgr::CloseFile(file);
		}
		if (Slots[i] == SLOT_OK) {
			if (CheckDataNotCorrupt(i, savename)) {
#if defined(FIX_INCOMPATIBLE_SAVES) && !defined(WII)
				if (!FixSave(i, GetSaveType(savename))) {
					CMessages::InsertNumberInString(TheText.Get("FEC_SLC"), i + 1, -1, -1, -1, -1, -1, SlotFileName[i]);
					Slots[i] = SLOT_CORRUPTED;
					continue;
				}
#endif
				SYSTEMTIME st;
				memcpy(&st, &header.SaveDateTime, sizeof(SYSTEMTIME));
				const char *month;
				switch (st.wMonth)
				{
				case 1: month = "JAN"; break;
				case 2: month = "FEB"; break;
				case 3: month = "MAR"; break;
				case 4: month = "APR"; break;
				case 5: month = "MAY"; break;
				case 6: month = "JUN"; break;
				case 7: month = "JUL"; break;
				case 8: month = "AUG"; break;
				case 9: month = "SEP"; break;
				case 10: month = "OCT"; break;
				case 11: month = "NOV"; break;
				case 12: month = "DEC"; break;
				default: assert(0);
				}
				char date[70];
#ifdef MORE_LANGUAGES
				if (CGame::japaneseGame)
					sprintf(date, "%02d %02d %04d %02d:%02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
				else
#endif // MORE_LANGUAGES
					sprintf(date, "%02d %s %04d %02d:%02d:%02d", st.wDay, UnicodeToAsciiForSaveLoad(TheText.Get(month)), st.wYear, st.wHour, st.wMinute, st.wSecond);
				AsciiToUnicode(date, SlotSaveDate[i]);
#ifdef WII
				SYS_Report("[reVC-WII] Slot scan valid: slot=%d title=%s date=%s\n",
					i,
					UnicodeToAsciiForSaveLoad(SlotFileName[i]),
					date);
#endif

			} else {
				CMessages::InsertNumberInString(TheText.Get("FEC_SLC"), i + 1, -1, -1, -1, -1, -1, SlotFileName[i]);
				Slots[i] = SLOT_CORRUPTED;
#ifdef WII
				SYS_Report("[reVC-WII] Slot scan corrupt: slot=%d path=%s title=%s\n",
					i, savename, UnicodeToAsciiForSaveLoad(SlotFileName[i]));
#endif
			}
		}
	}
}
