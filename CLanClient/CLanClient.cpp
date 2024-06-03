#include "CLanClient.h"

bool CLanClient::Connect(const CHAR* clanServerIP, USHORT clanserverPort, DWORD numOfIocpConcurrentThrd)
{
	m_CLanClientSock = CreateWindowSocket_IPv4(true);
	SOCKADDR_IN serverAddr = CreateDestinationADDR(clanServerIP, clanserverPort);
	if (ConnectSocket(m_CLanClientSock, serverAddr) == SOCKET_ERROR) {
		DebugBreak();
		return false;
	}

	for (BYTE i = 0; i < m_EventCnt; i++) {
		m_Events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
	}

	memset(&m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED));
	m_RecvOverlapped.hEvent = m_Events[enCLanRecv];
	m_SendOverlapped.hEvent = m_Events[enCLanSend];

	OnEnterJoinServer();

	m_CLanNetworkThread = (HANDLE)_beginthreadex(NULL, 0, CLanClient::CLanNetworkFunc, this, 0, NULL);

	WSABUF wsabuf;
	DWORD recvBytes;
	DWORD flags = 0;
	wsabuf.buf = (CHAR*)m_RecvBufferFromCLanServer.GetEnqueueBufferPtr();
	wsabuf.len = m_RecvBufferFromCLanServer.GetDirectEnqueueSize();
	int retval = WSARecv(m_CLanClientSock, &wsabuf, 1, &recvBytes, &flags, &m_RecvOverlapped, NULL);
	if (retval == SOCKET_ERROR) {
		if (WSAGetLastError() != ERROR_IO_PENDING) {
			DebugBreak();
			return false;
		}
	}

	return true;
}

bool CLanClient::Disconnect()
{
	SetEvent(m_Events[enThreadExit]);

	if (closesocket(m_CLanClientSock) == SOCKET_ERROR) {
		DebugBreak();
		return false;
	}

	OnLeaveServer();
	return true;
}

bool CLanClient::SendPacketToCLanServer(JBuffer* sendPacket, bool encoded)
{
	if (!encoded) {
		// 인코딩 수행
		if (sendPacket->GetUseSize() < sizeof(stMSG_HDR)) {
			DebugBreak();
			return false;
		}
		else {
			UINT offset = 0;
			stMSG_HDR* hdr;// = (stMSG_HDR*)sendDataPtr->GetDequeueBufferPtr();
			while (offset < sendPacket->GetUseSize()) {
				hdr = (stMSG_HDR*)sendPacket->GetBufferPtr(offset);
				offset += sizeof(stMSG_HDR);
				if (hdr->randKey == (BYTE)-1) {
					hdr->randKey = GetRandomKey();
					Encode(hdr->randKey, hdr->len, hdr->checkSum, sendPacket->GetBufferPtr(offset));
				}
				offset += hdr->len;
			}
		}
	}

	{
		std::lock_guard<std::mutex> lockGuard(m_SendBufferMtx);
		if (m_SendBufferToCLanServer.GetFreeSize() < sendPacket->GetUseSize()) {
			return false;
		}
		else {
			UINT enqSize = m_SendBufferToCLanServer.Enqueue(sendPacket->GetDequeueBufferPtr(), sendPacket->GetUseSize());
			if (enqSize < sendPacket->GetUseSize()) {
				DebugBreak();
			}
		}
	}

	return true;
}

void CLanClient::SendPostToCLanServer()
{
	if (InterlockedExchange(&m_SendFlag, 1) == 0) {
		WSABUF wsabuffs[2];
		DWORD bufCnt;
		if (m_SendBufferToCLanServer.GetDirectDequeueSize() < m_SendBufferToCLanServer.GetUseSize()) {
			wsabuffs[0].buf = (char*)m_SendBufferToCLanServer.GetDequeueBufferPtr();
			wsabuffs[0].len = m_SendBufferToCLanServer.GetDirectDequeueSize();
			wsabuffs[1].buf = (char*)m_SendBufferToCLanServer.GetBeginBufferPtr();
			wsabuffs[1].len = m_SendBufferToCLanServer.GetUseSize() - m_SendBufferToCLanServer.GetDirectDequeueSize();
			bufCnt = 2;
		}
		else {
			wsabuffs[0].buf = (char*)m_SendBufferToCLanServer.GetDequeueBufferPtr();
			wsabuffs[0].len = m_SendBufferToCLanServer.GetUseSize();
			bufCnt = 1;
		}

		memset(&m_SendOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
		if (WSASend(m_CLanClientSock, wsabuffs, bufCnt, NULL, 0, &m_SendOverlapped, NULL) == SOCKET_ERROR) {
			int errcode = WSAGetLastError();
			if (errcode != WSA_IO_PENDING) {
				// 연결 종료
				Disconnect();
			}
		}
	}
}

UINT __stdcall CLanClient::CLanNetworkFunc(void* arg)
{
	CLanClient* clanclient = (CLanClient*)arg;
	int retval;
	while (true) {
		retval = WaitForMultipleObjects(m_EventCnt, clanclient->m_Events, FALSE, INFINITE);
		switch (retval) {
		case WAIT_OBJECT_0 + enEvent::enThreadExit:
		{
			return 0;
		}
			break;
		case WAIT_OBJECT_0 + enEvent::enCLanRecv:
		{
			DWORD recvBytes;
			GetOverlappedResult((HANDLE)clanclient->m_CLanClientSock, (LPOVERLAPPED)&clanclient->m_RecvOverlapped, &recvBytes, FALSE);
			if (recvBytes == 0) {
				// 상대측 연결 종료
			}
			else {
				// 수신 완료
				clanclient->m_RecvBufferFromCLanServer.DirectMoveEnqueueOffset(recvBytes);
				while (clanclient->m_RecvBufferFromCLanServer.GetUseSize() >= sizeof(stMSG_HDR)) {
					stMSG_HDR hdr;
					clanclient->m_RecvBufferFromCLanServer.Peek(&hdr);
					if (hdr.code != dfPACKET_CODE) {
						DebugBreak();
						return false;
					}
					if (clanclient->m_RecvBufferFromCLanServer.GetUseSize() < sizeof(stMSG_HDR) + hdr.len) {
						break;
					}
					clanclient->m_RecvBufferFromCLanServer.DirectMoveDequeueOffset(sizeof(stMSG_HDR));

					if (!clanclient->Decode(hdr.randKey, hdr.len, hdr.checkSum, clanclient->m_RecvBufferFromCLanServer)) {
						DebugBreak();
						return false;
					}

					JSerBuffer recvBuff(clanclient->m_RecvBufferFromCLanServer, hdr.len, true);
					clanclient->OnRecvFromCLanServer(recvBuff);
					clanclient->m_RecvBufferFromCLanServer.DirectMoveDequeueOffset(hdr.len);
				}

				// 수신 대기
				memset(&clanclient->m_RecvOverlapped, 0, sizeof(WSAOVERLAPPED) - sizeof(WSAOVERLAPPED::hEvent));
				WSABUF wsabuf;
				DWORD recvBytes;
				DWORD flags = 0;
				wsabuf.buf = (CHAR*)clanclient->m_RecvBufferFromCLanServer.GetEnqueueBufferPtr();
				wsabuf.len = clanclient->m_RecvBufferFromCLanServer.GetDirectEnqueueSize();
				int retval = WSARecv(clanclient->m_CLanClientSock, &wsabuf, 1, &recvBytes, &flags, &clanclient->m_RecvOverlapped, NULL);
				if (retval == SOCKET_ERROR) {
					if (WSAGetLastError() != ERROR_IO_PENDING) {
						DebugBreak();
						return 1;
					}
				}
			}
		}
			break;
		case WAIT_OBJECT_0 + enEvent::enCLanSend:
		{
			DWORD sendBytes;
			GetOverlappedResult((HANDLE)clanclient->m_CLanClientSock, (LPOVERLAPPED)&clanclient->m_SendOverlapped, &sendBytes, FALSE);
			if (sendBytes == 0) {

			}
			else {
				// 송신 완료
				clanclient->m_SendBufferToCLanServer.DirectMoveDequeueOffset(sendBytes);
				if (clanclient->m_SendBufferToCLanServer.GetUseSize() > 0) {
					clanclient->SendPostToCLanServer();
				}
			}
		}
			break;
		default:
			DebugBreak();
			break;
		}
	}

	return 0;
}
