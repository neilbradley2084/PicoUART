#pragma once

// Single-producer / single-consumer byte ring, safe across cores.
// One slot is always left empty so head == tail means empty.
// Everything is forced inline so core 1's RAM-resident loop never calls into flash.

#include "Shared/Shared.h"
#include "hardware/sync.h"

typedef struct SRing
{
	uint8_t *pu8Buffer;
	uint32_t u32Size;
	volatile uint32_t u32Head;		// Written only by the producer
	volatile uint32_t u32Tail;		// Written only by the consumer
} SRing;

#define RING_INLINE		static inline __attribute__((always_inline))

// Bytes waiting to be consumed
RING_INLINE uint32_t RingUsed(const SRing *psRing)
{
	uint32_t u32Head = psRing->u32Head;
	uint32_t u32Tail = psRing->u32Tail;

	if (u32Head >= u32Tail)
	{
		return(u32Head - u32Tail);
	}

	return(psRing->u32Size - u32Tail + u32Head);
}

// Bytes that can still be produced
RING_INLINE uint32_t RingFree(const SRing *psRing)
{
	return(psRing->u32Size - 1 - RingUsed(psRing));
}

// Producer: add one byte.  Returns false if full.
RING_INLINE bool RingPut(SRing *psRing,
                         uint8_t u8Data)
{
	uint32_t u32Head = psRing->u32Head;
	uint32_t u32Next = u32Head + 1;

	if (u32Next == psRing->u32Size)
	{
		u32Next = 0;
	}

	if (u32Next == psRing->u32Tail)
	{
		return(false);
	}

	psRing->pu8Buffer[u32Head] = u8Data;
	__dmb();
	psRing->u32Head = u32Next;
	return(true);
}

// Consumer: remove one byte.  Returns false if empty.
RING_INLINE bool RingGet(SRing *psRing,
                         uint8_t *pu8Data)
{
	uint32_t u32Tail = psRing->u32Tail;
	uint32_t u32Next;

	if (u32Tail == psRing->u32Head)
	{
		return(false);
	}

	__dmb();
	*pu8Data = psRing->pu8Buffer[u32Tail];
	__dmb();

	u32Next = u32Tail + 1;
	if (u32Next == psRing->u32Size)
	{
		u32Next = 0;
	}

	psRing->u32Tail = u32Next;
	return(true);
}

// Consumer: contiguous readable span starting at the tail
RING_INLINE uint32_t RingReadSpan(SRing *psRing,
                                  uint8_t **ppu8Data)
{
	uint32_t u32Head = psRing->u32Head;
	uint32_t u32Tail = psRing->u32Tail;

	__dmb();
	*ppu8Data = &psRing->pu8Buffer[u32Tail];

	if (u32Head >= u32Tail)
	{
		return(u32Head - u32Tail);
	}

	return(psRing->u32Size - u32Tail);
}

// Consumer: release bytes obtained from RingReadSpan()
RING_INLINE void RingConsume(SRing *psRing,
                             uint32_t u32Count)
{
	uint32_t u32Tail = psRing->u32Tail + u32Count;

	if (u32Tail >= psRing->u32Size)
	{
		u32Tail -= psRing->u32Size;
	}

	__dmb();
	psRing->u32Tail = u32Tail;
}

// Consumer: throw away everything currently queued
RING_INLINE void RingDiscard(SRing *psRing)
{
	__dmb();
	psRing->u32Tail = psRing->u32Head;
}

// Producer: contiguous writable span starting at the head
RING_INLINE uint32_t RingWriteSpan(SRing *psRing,
                                   uint8_t **ppu8Data)
{
	uint32_t u32Head = psRing->u32Head;
	uint32_t u32Tail = psRing->u32Tail;
	uint32_t u32Span;

	*ppu8Data = &psRing->pu8Buffer[u32Head];

	if (u32Head >= u32Tail)
	{
		u32Span = psRing->u32Size - u32Head;
		if (0 == u32Tail)
		{
			u32Span--;
		}
	}
	else
	{
		u32Span = u32Tail - u32Head - 1;
	}

	return(u32Span);
}

// Producer: publish bytes written through RingWriteSpan()
RING_INLINE void RingProduce(SRing *psRing,
                             uint32_t u32Count)
{
	uint32_t u32Head = psRing->u32Head + u32Count;

	if (u32Head >= psRing->u32Size)
	{
		u32Head -= psRing->u32Size;
	}

	__dmb();
	psRing->u32Head = u32Head;
}
