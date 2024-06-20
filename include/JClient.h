#pragma once
#include "WInSocketAPI.h"
#include "JBuffer.h"

#include "CommonProtocol.h"

#include <mutex>

class JClient
{
public:
	JClient() 
	: m_ClientSockAlive(false), m_SendFlag(0), m_StopNetworkThread(false)
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

private:
	bool InitClient(const CHAR* clanServerIP, USHORT clanserverPort);
	void DeleteClient();
	void SendPostToServer();

	static UINT __stdcall ClientNetworkThreadFunc(void* arg);
};

