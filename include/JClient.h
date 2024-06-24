#pragma once
#include "WInSocketAPI.h"
#include "JBuffer.h"
#include "TlsMemPool.h"

#include "CommonProtocol.h"

#include <mutex>

class JClient
{
public:
	JClient(TlsMemPoolManager<JBuffer>* tlsMemPoolMgr, UINT serialBuffSize, BYTE protoCode, BYTE packetKey)
	: m_TlsMemPoolMgr(tlsMemPoolMgr), m_SerialBuffSize(serialBuffSize), m_ProtocolCode(protoCode), m_PacketKey(packetKey),
		m_ClientSockAlive(false), m_SendFlag(0), m_StopNetworkThread(false)
	{
		m_RecvBuffer.ClearBuffer();
		m_SendBuffer.ClearBuffer();
		memset(&m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED));
		memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED));
	}
private:
	SOCKET		m_ClientSock;
	bool		m_ClientSockAlive;
	JBuffer		m_RecvBuffer;
	JBuffer		m_SendBuffer;
	std::mutex	m_SendBufferMtx;
	UINT		m_SendFlag;

	WSAOVERLAPPED	m_RecvOverlapped;
	WSAOVERLAPPED	m_SendOverlapped;

	HANDLE			m_NetworkThread;
	bool			m_StopNetworkThread;

	static const BYTE	m_EventCnt = 3;
	static enum enEvent {
		enThreadExit = 0,
		enCLanRecv,
		enCLanSend
	};
	HANDLE		m_Events[m_EventCnt] = { NULL, };

	TlsMemPoolManager<JBuffer>* m_TlsMemPoolMgr;
	UINT			m_SerialBuffSize;
	BYTE			m_ProtocolCode;
	BYTE			m_PacketKey;

protected:
	bool ConnectToServer(const CHAR* clanServerIP, USHORT clanserverPort);
	bool DisconnectFromServer();
	bool SendPacketToServer(JBuffer* sendPacket);

protected:
	virtual void OnClientNetworkThreadStart() = 0;
	virtual void OnServerConnected() = 0;		//< 서버와의 연결 성공 후
	virtual void OnServerLeaved() = 0;			//< 서버와의 연결이 끊어졌을 때
	virtual void OnRecvFromServer(JBuffer& clientRecvRingBuffer) = 0;
	virtual void OnSerialSendBufferFree(JBuffer* serialBuff) = 0;

protected:
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads);
	void Encode(BYTE randKey, USHORT payloadLen, BYTE& checkSum, BYTE* payloads, BYTE packetKey);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, BYTE* payloads);
	bool Decode(BYTE randKey, USHORT payloadLen, BYTE checkSum, JBuffer& ringPayloads);
	inline BYTE GetRandomKey() {
		return rand() % UINT8_MAX;	// 0b0000'0000 ~ 0b0111'1110 (0b1111'1111은 디코딩이 이루어지지 않은 페이로드 식별 값)
	}

	inline void AllocTlsMemPool() {
		m_TlsMemPoolMgr->AllocTlsMemPool();
	}
	inline JBuffer* AllocSerialBuff() {
		JBuffer* msg = m_TlsMemPoolMgr->GetTlsMemPool().AllocMem(1, m_SerialBuffSize);
		msg->ClearBuffer();
		return msg;
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length, BYTE randKey = -1) {
		JBuffer* msg = m_TlsMemPoolMgr->GetTlsMemPool().AllocMem(1, m_SerialBuffSize);
		msg->ClearBuffer();

		stMSG_HDR* hdr = msg->DirectReserve<stMSG_HDR>();
		hdr->code = m_ProtocolCode;
		hdr->len = length;
		hdr->randKey = randKey;	// Encode 전 송신 직렬화 버퍼 식별

		return msg;
	}
	inline JBuffer* AllocSerialSendBuff(USHORT length, BYTE protocolCode, BYTE randKey = -1) {
		JBuffer* msg = m_TlsMemPoolMgr->GetTlsMemPool().AllocMem(1, m_SerialBuffSize);
		msg->ClearBuffer();

		stMSG_HDR* hdr = msg->DirectReserve<stMSG_HDR>();
		hdr->code = protocolCode;
		hdr->len = length;
		hdr->randKey = randKey;	// Encode 전 송신 직렬화 버퍼 식별

		return msg;
	}
	inline void FreeSerialBuff(JBuffer* buff) {
		m_TlsMemPoolMgr->GetTlsMemPool().FreeMem(buff);
	}
	inline void AddRefSerialBuff(JBuffer* buff) {
		m_TlsMemPoolMgr->GetTlsMemPool().IncrementRefCnt(buff, 1);
	}
	inline size_t GetAllocMemPoolUsageUnitCnt() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_TlsMemPoolMgr->GetAllocatedMemUnitCnt();
	}
	inline size_t GetAllocMemPoolUsageSize() {
		//return (m_SerialBuffPoolMgr.GetTotalAllocMemCnt() - m_SerialBuffPoolMgr.GetTotalFreeMemCnt()) * sizeof(stMemPoolNode<JBuffer>);
		return m_TlsMemPoolMgr->GetAllocatedMemUnitCnt() * (sizeof(stMemPoolNode<JBuffer>) + m_SerialBuffSize);
	}

private:
	bool InitClient(const CHAR* clanServerIP, USHORT clanserverPort);
	void DeleteClient();
	void SendPostToServer();

	static UINT __stdcall ClientNetworkThreadFunc(void* arg);
};

