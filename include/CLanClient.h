#pragma once

#include "CLanServer.h"

class CLanClient : public CLanServer
{
public:
#if defined(ALLOC_BY_TLS_MEM_POOL)
	CLanClient(const char* serverIP, uint16 serverPort,
		DWORD numOfIocpConcurrentThrd, uint16 numOfWorkerThreads, uint16 maxOfConnections,
		size_t tlsMemPoolDefaultUnitCnt = 0, size_t tlsMemPoolDefaultCapacity = 0, 
		bool tlsMemPoolReferenceFlag = false, bool tlsMemPoolPlacementNewFlag = false,
		UINT serialBufferSize = DEFAULT_SERIAL_BUFFER_SIZE,
#if defined(LOCKFREE_SEND_QUEUE)
		uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#else
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
#endif
		BYTE protocolCode = dfPACKET_CODE, BYTE packetKey = dfPACKET_KEY
	)
		: CLanServer(serverIP, serverPort,numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections,
			tlsMemPoolDefaultUnitCnt, tlsMemPoolDefaultCapacity, tlsMemPoolReferenceFlag, tlsMemPoolPlacementNewFlag,
			serialBufferSize,
#if defined(LOCKFREE_SEND_QUEUE)
			sessionRecvBuffSize,
#else
			sessionSendBuffSize, sessionRecvBuffSize,
#endif
			protocolCode, packetKey
		)
	{}
#else
	CLanClient(const char* serverIP, UINT16 serverPort,
		DWORD numOfIocpConcurrentThrd, UINT16 numOfWorkerThreads, UINT16 maxOfConnections,
		uint32 sessionSendBuffSize = SESSION_SEND_BUFFER_DEFAULT_SIZE, uint32 sessionRecvBuffSize = SESSION_RECV_BUFFER_DEFAULT_SIZE,
		BYTE protocolCode = dfPACKET_CODE, BYTE packetKey = dfPACKET_KEY
	)
		: CLanServer(serverIP, serverPort, numOfIocpConcurrentThrd, numOfWorkerThreads, maxOfConnections,
			sessionSendBuffSize, sessionRecvBuffSize,
			protocolCode, packetKey
		)
	{}
#endif

	bool Start() {
		return CLanServer::Start();
	}
	void Stop() {
		CLanServer::Stop();
	}

private:
	SOCKET		m_CLanClientSock;
	bool		m_ClientSockAlive = false;
	JBuffer		m_RecvBufferFromCLanServer;
	JBuffer		m_SendBufferToCLanServer;
	std::mutex	m_SendBufferMtx;
	UINT		m_SendFlag;

	WSAOVERLAPPED	m_RecvOverlapped;
	WSAOVERLAPPED	m_SendOverlapped;

	HANDLE			m_CLanNetworkThread;
	bool			m_StopNetworkThread;

	static const BYTE	m_EventCnt = 3;
	static enum enEvent {
		enThreadExit = 0,
		enCLanRecv,
		enCLanSend
	};
	HANDLE		m_Events[m_EventCnt] = { NULL, };

	bool InitLanClient(const CHAR* clanServerIP, USHORT clanserverPort);
	void DeleteLanClient();

protected:
	bool ConnectLanServer(const CHAR* clanServerIP, USHORT clanserverPort);		//	바인딩 IP, 서버IP / 워커스레드 수 / 나글옵션
	bool DisconnectLanServer();
	
	bool SendPacketToCLanServer(JBuffer* sendPacket, bool encoded = false);
	void SendPostToCLanServer();

	virtual void OnEnterJoinServer() = 0;		//< 서버와의 연결 성공 후
	virtual void OnLeaveServer() = 0;			//< 서버와의 연결이 끊어졌을 때

	virtual void OnRecvFromCLanServer(JBuffer& recvBuff) = 0;

private:
	static UINT __stdcall CLanNetworkFunc(void* arg);
};

