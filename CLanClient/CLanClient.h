#pragma once

#include "CLanServer.h"

class CLanClient : public CLanServer
{
private:
	SOCKET		m_CLanClientSock;
	JBuffer		m_RecvBufferFromCLanServer;
	JBuffer		m_SendBufferToCLanServer;
	std::mutex	m_SendBufferMtx;
	UINT		m_SendFlag;

	WSAOVERLAPPED	m_RecvOverlapped;
	WSAOVERLAPPED	m_SendOverlapped;

	HANDLE			m_CLanNetworkThread;

	static const BYTE	m_EventCnt = 3;
	static enum enEvent {
		enThreadExit = 0,
		enCLanRecv,
		enCLanSend
	};
	HANDLE		m_Events[m_EventCnt];

protected:
	bool Connect(const CHAR* clanServerIP, USHORT clanserverPort, DWORD numOfIocpConcurrentThrd);		//	바인딩 IP, 서버IP / 워커스레드 수 / 나글옵션
	bool Disconnect();							
	
	bool SendPacketToCLanServer(JBuffer* sendPacket, bool encoded = false);
	void SendPostToCLanServer();

	virtual void OnEnterJoinServer() = 0;		//< 서버와의 연결 성공 후
	virtual void OnLeaveServer() = 0;			//< 서버와의 연결이 끊어졌을 때

	virtual void OnRecvFromCLanServer(JBuffer& recvBuff) = 0;

private:
	static UINT __stdcall CLanNetworkFunc(void* arg);
};

