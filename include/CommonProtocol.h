#pragma once

#include <minwindef.h>

////////////////////////////////////////////////////
// 
// �������� ���� ���(���̺귯��)
// 
////////////////////////////////////////////////////
#define dfPACKET_CODE		0x77
#define dfPACKET_KEY		0x32

#pragma pack(push, 1)
struct stMSG_HDR {
	BYTE	code;
	USHORT	len;
	BYTE	randKey;
	BYTE	checkSum;
};
#pragma pack(pop)