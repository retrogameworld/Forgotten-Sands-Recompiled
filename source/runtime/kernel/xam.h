#pragma once
#include <xbox.h>

// XAM (from UnleashedRecomp kernel/xam.h, trimmed).

#define MSGID(Area, Number) (uint32_t)((uint16_t)(Area) << 16 | (uint16_t)(Number))
#define MSG_AREA(msgid)     (((msgid) >> 16) & 0xFFFF)
#define MSG_NUMBER(msgid)   ((msgid) & 0xFFFF)

std::string_view XamGetRootPath(const std::string_view& root);
void XamRootCreate(const std::string_view& root, const std::string_view& path);

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam);
