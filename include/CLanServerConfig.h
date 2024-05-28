#pragma once

////////////////////////////////////////////////////
// �ۼ��� ���� �⺻ ũ��
////////////////////////////////////////////////////
#define SESSION_SEND_BUFFER_DEFAULT_SIZE	20000
#define SESSION_RECV_BUFFER_DEFAULT_SIZE	20000

////////////////////////////////////////////////////
// �۽� ���� ����ȭ �м�
////////////////////////////////////////////////////
// 1. std::mutex ���
#define SESSION_SENDBUFF_SYNC_TEST
#if defined(SESSION_SENDBUFF_SYNC_TEST)
#include <mutex>
#endif
// 2. (to do) ��-���� ť

////////////////////////////////////////////////////
// �۽� ���� ������ ���� ���
////////////////////////////////////////////////////
// 1. SendPacket, �۽� ���ۿ� ���� ������ copy 
//#define SEND_RECV_RING_BUFF_COPY_MODE

// 2. SendPacket, �۽� ����ȭ ���� ������ ����
#define SEND_RECV_RING_BUFF_SERIALIZATION_MODE
#if defined(SEND_RECV_RING_BUFF_SERIALIZATION_MODE)
#define WSABUF_ARRAY_DEFAULT_SIZE			1000
#endif

////////////////////////////////////////////////////
// �޸� Ǯ
////////////////////////////////////////////////////
#define ALLOC_BY_TLS_MEM_POOL
#if defined(ALLOC_BY_TLS_MEM_POOL)
#define TLS_MEM_POOL_DEFAULT_UNIT_CNT		100
#define TLS_MEM_POOL_DEFAULT_CAPACITY		100
#define TLS_MEM_POOL_DEFAULT_SURPLUS_SIZE	100
#endif


////////////////////////////////////////////////////
// �α�(log)
////////////////////////////////////////////////////
#define SESSION_LOG		// ���� ����(�� ����) ���� �α�
#define SENDBUFF_MONT_LOG		// �۽� ���� ����͸��� �α�


#define dfPACKET_CODE		0x77
#define dfPACKET_KEY		0x32

#define TRACKING_CLIENT_PORT

////////////////////////////////////////////////////
// TPS(Transaction Per Second)
////////////////////////////////////////////////////
#define CALCULATE_TRANSACTION_PER_SECOND
#if defined(CALCULATE_TRANSACTION_PER_SECOND)
#define		ACCEPT_TRANSACTION		0
#define		RECV_TRANSACTION		1
#define		SEND_TRANSACTION		2
#define		SEND_REQ_TRANSACTION	3
#define		NUM_OF_TPS_ITEM		4
#endif