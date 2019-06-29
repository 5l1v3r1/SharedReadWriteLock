#pragma once

#include "Atomic.hpp"
#include "WaitEvent.hpp"
#include "Utility.hpp"
#include "DebugLog.hpp"

//////////////////////////////////////////////////////////////////////////
// ״̬λ˳��
enum SRWBits
{
	// ������. ��ռ��������
	BIT_LOCKED = 0,
	// ������. ���ڵȴ�
	BIT_SPINNING = 1,
	// �����л��Ż��ȴ�����. ֻ�д�������λʱ��������
	BIT_WAKING = 2,
	// ���ع�����
	BIT_MULTI_SHARED = 3,
	// �������λ
	BIT_SHARED = 4,
};

// ״̬λ���
enum SRWFlags
{
	FLAG_LOCKED = 1 << BIT_LOCKED,
	FLAG_SPINNING = 1 << BIT_SPINNING,
	FLAG_WAKING = 1 << BIT_WAKING,
	FLAG_MULTI_SHARED = 1 << BIT_MULTI_SHARED,
	FLAG_SHARED = 1 << BIT_SHARED,
	FLAG_ALL = FLAG_MULTI_SHARED | FLAG_WAKING | FLAG_SPINNING | FLAG_LOCKED
};

// ջ�ڵ�. ������ʱ, �ȴ���ʹ�������������߳�ջ�ϵĽڵ�
struct SRWStackNode : WaitEvent
{
	// ��һ�ڵ�
	SRWStackNode *Back;
	// ֪ͨ�ڵ�
	SRWStackNode *Notify;
	// ��һ�ڵ�
	SRWStackNode *Next;
	// �������
	uint32_t SharedCount;
	// �̱߳��, ֵΪ FLAG_LOCKED, FLAG_SPINNING �� FLAG_WAKING
	uint32_t Flags;
	// ���������ȴ�����һ����
	SRWLock *LastLock;
};

// ��״̬
struct SRWStatus
{
	union
	{
		struct
		{
			size_t Locked : 1;
			size_t Spinning : 1;
			size_t Waking : 1;
			size_t MultiShared : 1;
			size_t SharedCount : sizeof(size_t) * 8 - 4;
		};
		size_t Value;
	};

	SRWStatus() : Value(0)
	{
	}

	SRWStatus(size_t value) : Value(value)
	{
	}

	bool operator ==(size_t value) const
	{
		return Value == value;
	}

	bool operator ==(const SRWStatus &other) const
	{
		return Value == other.Value;
	}

	bool operator !=(size_t value) const
	{
		return !(*this == value);
	}

	bool operator !=(const SRWStatus &other) const
	{
		return !(*this == other);
	}

	SRWStackNode* WaitNode() const
	{
		return reinterpret_cast<SRWStackNode*>(Value & ~FLAG_ALL);
	}

	size_t WithoutMultiSharedLocked() const
	{
		return Value & ~(FLAG_MULTI_SHARED | FLAG_LOCKED);
	}

	uint32_t Counter() const
	{
		return static_cast<uint32_t>(Value & (FLAG_WAKING | FLAG_SPINNING | FLAG_LOCKED));
	}

	bool IsCounterFull() const
	{
		return Counter() == (FLAG_WAKING | FLAG_SPINNING | FLAG_LOCKED);
	}

	size_t WithFullCounter() const
	{
		return Value | (FLAG_WAKING | FLAG_SPINNING | FLAG_LOCKED);
	}

	void ReplaceFlagPart(size_t flagPart)
	{
		Value = (Value & ~FLAG_ALL) | (flagPart & FLAG_ALL);
	}
};

//////////////////////////////////////////////////////////////////////////
static uint32_t g_SRWSpinCount = 1024;

static void Backoff(uint32_t *pCount)
{
	uint32_t count = *pCount;
	if (count)
	{
		if (count < 0x1FFF)
			count *= 2;
	}
	else
	{
		// TODO: ������ֱ�ӷ���
		// ���ó�ʼ����
		count = 64;
	}

	*pCount = count;
	// ����������ô���
	count += (count - 1) & RandomValue();
	//count = count * 10 / _KUSER_SHARED_DATA.CyclesPerYield

#pragma nounroll
	while (count--)
		PLATFORM_YIELD;
}

//////////////////////////////////////////////////////////////////////////
// ����֪ͨ�ڵ�
static SRWStackNode* FindNotifyNode(SRWStackNode *pWaitNode)
{
	SRWStackNode *pNotify = pWaitNode->Notify;
	if (pNotify)
		return pNotify;

	SRWStackNode *pCurr = pWaitNode;
	do
	{
		SRWStackNode *pLast = pCurr;
		pCurr = pCurr->Back;
		pCurr->Next = pLast;
		pNotify = pCurr->Notify;
	} while (!pNotify);

	return pNotify;
}

// ����֪ͨ�ڵ㲢����
static SRWStackNode* UpdateNotifyNode(SRWStackNode *pWaitNode)
{
	SRWStackNode *pNotify = FindNotifyNode(pWaitNode);
	pWaitNode->Notify = pNotify;
	return pNotify;
}

// �����������״̬
static bool TryClearWaking(size_t *pLockStatus, SRWStatus &lastStatus)
{
	SRWStatus newStatus = lastStatus.Value - FLAG_WAKING;
	AssertDebug(!newStatus.Waking);
	AssertDebug(newStatus.Locked);

	newStatus = Atomic::CompareExchange<size_t>(pLockStatus, lastStatus.Value, newStatus.Value);
	if (newStatus == lastStatus)
		return true;

	lastStatus = newStatus;
	return false;
}

PLATFORM_NOINLINE static void WakeUpLock(size_t *pLockStatus, SRWStatus lastStatus, bool isForce = false)
{
	SRWStackNode *pNotify;
	for (;;)
	{
		AssertDebug(!lastStatus.MultiShared);

		if (!isForce)
		{
			// ����״̬����������ѱ��
			while (lastStatus.Locked)
			{
				AssertDebug(lastStatus.Spinning);
				if (TryClearWaking(pLockStatus, lastStatus))
					return;
			}
		}

		// Ѱ����Ҫ֪ͨ�Ľڵ�
		SRWStackNode *pWaitNode = lastStatus.WaitNode();
		pNotify = UpdateNotifyNode(pWaitNode);

		if (pNotify->Flags & FLAG_LOCKED)
		{
			if (isForce)
			{
				Atomic::FetchAnd<size_t>(pLockStatus, ~FLAG_WAKING);
				return;
			}

			if (SRWStackNode *pNext = pNotify->Next)
			{
				// ���֪ͨ������һ���ڵ�, ����֪ͨ������״̬, �������һ֪ͨ�ڵ㲢���ѵ�ǰ�ڵ�
				pWaitNode->Notify = pNext;
				pNotify->Next = nullptr;

				AssertDebug(pWaitNode != pNotify);
				AssertDebug(SRWStatus(*pLockStatus).Spinning);

				// ����������ѱ��
				Atomic::FetchAnd<size_t>(pLockStatus, ~FLAG_WAKING);
				break;
			}
		}

		// ����״̬
		SRWStatus currStatus = Atomic::CompareExchange<size_t>(
			pLockStatus,
			lastStatus.Value,
			isForce
				? (FLAG_SHARED | FLAG_LOCKED)
				: 0);
		if (currStatus == lastStatus)
			break;

		lastStatus = currStatus;
	}

	// ������Ҫ֪ͨ�Ľڵ�
	do
	{
		// �������֪ͨ�ڵ�����
		SRWStackNode *pNext = pNotify->Next;

		Atomic::FetchBitSet(&pNotify->Flags, BIT_WAKING);

		// ��������������
		if (!Atomic::FetchBitClear(&pNotify->Flags, BIT_SPINNING))
		{
			// ���֮ǰ������������
			pNotify->WakeUp();
		}

		pNotify = pNext;
	} while (pNotify);
}

static void OptimizeLockList(size_t *pLockStatus, SRWStatus lastStatus)
{
	// ����״̬ʱѭ��
	while (lastStatus.Locked)
	{
		// ����֪ͨ�ڵ�
		SRWStackNode *pWaitNode = lastStatus.WaitNode();
		UpdateNotifyNode(pWaitNode);

		// ����������ѱ��
		if (TryClearWaking(pLockStatus, lastStatus))
			return;
	}
	// �����������״̬����
	WakeUpLock(pLockStatus, lastStatus);
}

template <bool IsExclusive>
static bool QueueStackNode(size_t *pLockStatus, SRWStackNode *pStackNode, SRWStatus lastStatus)
{
	SRWStatus newStatus;
	bool isOptimize = false;
	pStackNode->Next = nullptr;

	if (lastStatus.Spinning)
	{
		// �����߳���������
		pStackNode->SharedCount = -1;
		pStackNode->Notify = nullptr;
		// ��Ϊ��ǰ�̵߳���һ���ڵ�ҽ�
		pStackNode->Back = lastStatus.WaitNode();
		// �̳ж��ع�����, �����û���, �����������ͱ��
		newStatus = reinterpret_cast<size_t>(pStackNode) | (lastStatus.Value & FLAG_MULTI_SHARED) | FLAG_WAKING | FLAG_SPINNING | FLAG_LOCKED;

		// ���������ѱ�ǵ������Ҫ�����Ż�����
		if (!lastStatus.Waking)
			isOptimize = true;
	}
	else
	{
		// �ѵ�ǰ�߳���Ϊ��һ��֪ͨ�ڵ�
		pStackNode->Notify = pStackNode;
		newStatus = reinterpret_cast<size_t>(pStackNode) | FLAG_SPINNING | FLAG_LOCKED;

		if (IsExclusive)
		{
			pStackNode->SharedCount = lastStatus.SharedCount;
			// ����������� 1 �������Ҫ���ö��ع�����
			if (pStackNode->SharedCount > 1)
				newStatus.MultiShared = 1;
			else if (!pStackNode->SharedCount)
				pStackNode->SharedCount = -2;
		}
		else
		{
			pStackNode->SharedCount = -2;
		}
	}

	AssertDebug(newStatus.Spinning);
	AssertDebug(newStatus.Locked);
	AssertDebug(lastStatus.Locked);

	// ���Ը�����״̬
	if (lastStatus == Atomic::CompareExchange<size_t>(pLockStatus, lastStatus.Value, newStatus.Value))
	{
		// ���³ɹ�, �Ż�����
		if (isOptimize)
			OptimizeLockList(pLockStatus, newStatus);
		return true;
	}
	return false;
}
