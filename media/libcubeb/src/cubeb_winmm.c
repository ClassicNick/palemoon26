/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#define __MSVCRT_VERSION__ 0x0700
#undef WINVER
#define WINVER 0x0501
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN

#include <malloc.h>
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

/* This is missing from the MinGW headers. Use a safe fallback. */
#if !defined(MEMORY_ALLOCATION_ALIGNMENT)
#define MEMORY_ALLOCATION_ALIGNMENT 16
#endif

/**This is also missing from the MinGW headers. It  also appears to be undocumented by Microsoft.*/
#ifndef WAVE_FORMAT_48S16
#define WAVE_FORMAT_48S16      0x00008000       /* 48     kHz, Stereo, 16-bit */
#endif

/**Taken from winbase.h, also not in MinGW.*/
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION   0x00010000    // Threads only
#endif

#define CUBEB_STREAM_MAX 32
#define NBUFS 4

const GUID KSDATAFORMAT_SUBTYPE_PCM =
{ 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/* Portable implementation From MSPS */

typedef struct _MSPS_SLIST_ENTRY {
    // Want a volatile pointer to non-volatile data so place after all type info
    struct _MSPS_SLIST_ENTRY * volatile Next;
} MSPS_SLIST_ENTRY, *MSPS_PSLIST_ENTRY;

typedef union _MSPS_SLIST_HEADER {
    // Provides 8 byte alignment for 32-bit builds.  Technically, not needed for
    // current implementation below but leaving for future use.
    ULONGLONG Alignment;
    struct {
        // Want a volatile pointer to non-volatile data so place after all type info
        MSPS_PSLIST_ENTRY volatile Head;
        volatile LONG Depth;
        volatile LONG Mutex;
    } List;
} MSPS_SLIST_HEADER, *MSPS_PSLIST_HEADER;

/* Portable implementation From MSPS */


struct cubeb_stream_item {
  MSPS_SLIST_ENTRY head;
  cubeb_stream * stream;
};

static struct cubeb_ops const winmm_ops;

struct cubeb {
  struct cubeb_ops const * ops;
  HANDLE event;
  HANDLE thread;
  int shutdown;
  MSPS_PSLIST_HEADER work;
  CRITICAL_SECTION lock;
  unsigned int active_streams;
  unsigned int minimum_latency;
};

struct cubeb_stream {
  cubeb * context;
  cubeb_stream_params params;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  WAVEHDR buffers[NBUFS];
  size_t buffer_size;
  int next_buffer;
  int free_buffers;
  int shutdown;
  int draining;
  HANDLE event;
  HWAVEOUT waveout;
  CRITICAL_SECTION lock;
  uint64_t written;
  float soft_volume;
  /* For position wrap-around handling: */
  size_t frame_size;
  DWORD prev_pos_lo_dword;
  DWORD pos_hi_dword;
};

/* Portable implementation From MSPS */

void WINAPI InitializeSListHead_kex(MSPS_PSLIST_HEADER ListHeader)
{
    XASSERT( ListHeader != NULL );

    ListHeader->List.Head = NULL;
    ListHeader->List.Depth = 0;
    ListHeader->List.Mutex = 0;
}

MSPS_PSLIST_ENTRY WINAPI InterlockedPopEntrySList_kex(MSPS_PSLIST_HEADER ListHeader)
{
    MSPS_PSLIST_ENTRY oldHead = ListHeader->List.Head;
    if ( oldHead == NULL ) {
        return NULL;
    }

    while ( ListHeader->List.Mutex != 0 || InterlockedCompareExchange( &ListHeader->List.Mutex, 1, 0 ) != 0 ) {
        // Spin until 'mutex' is free
    }

    // We have the 'mutex' so proceed with update
    oldHead = ListHeader->List.Head;
    if ( oldHead != NULL ) {
        ListHeader->List.Head = oldHead->Next;
        --(ListHeader->List.Depth);
        XASSERT( ListHeader->List.Depth <= 0 );
    }

    // Free the 'mutex'
    ListHeader->List.Mutex = 0;

    return oldHead;
}

MSPS_PSLIST_ENTRY WINAPI InterlockedPushEntrySList_kex(MSPS_PSLIST_HEADER ListHeader, MSPS_PSLIST_ENTRY ListEntry)
{
    MSPS_PSLIST_ENTRY oldHead;
    XASSERT( ListHeader != NULL );

    while ( ListHeader->List.Mutex != 0 || InterlockedCompareExchange( &ListHeader->List.Mutex, 1, 0 ) != 0 ) {
        // Spin until 'mutex' is free
    }

    // We have the 'mutex' so proceed with update
    oldHead = ListHeader->List.Head;
    ListEntry->Next = oldHead;
    ListHeader->List.Head = ListEntry;
    ++(ListHeader->List.Depth);

    // Free the 'mutex'
    ListHeader->List.Mutex = 0;

    return oldHead;
}

MSPS_PSLIST_ENTRY WINAPI InterlockedFlushSList_kex(MSPS_PSLIST_HEADER ListHeader)
{
    MSPS_PSLIST_ENTRY oldHead;
    XASSERT( ListHeader != NULL );

    while ( ListHeader->List.Mutex != 0 || InterlockedCompareExchange( &ListHeader->List.Mutex, 1, 0 ) != 0 ) {
        // Spin until 'mutex' is free
    }

    // We have the 'mutex' so proceed with update
    oldHead = ListHeader->List.Head;
    ListHeader->List.Head = NULL;
    ListHeader->List.Depth = 0;

    // Free the 'mutex'
    ListHeader->List.Mutex = 0;

    return oldHead;
}

/* Portable implementation From MSPS */

static size_t
bytes_per_frame(cubeb_stream_params params)
{
  size_t bytes;

  switch (params.format) {
  case CUBEB_SAMPLE_S16LE:
    bytes = sizeof(signed short);
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    bytes = sizeof(float);
    break;
  default:
    XASSERT(0);
  }

  return bytes * params.channels;
}

static WAVEHDR *
winmm_get_next_buffer(cubeb_stream * stm)
{
  WAVEHDR * hdr = NULL;

  XASSERT(stm->free_buffers > 0 && stm->free_buffers <= NBUFS);
  hdr = &stm->buffers[stm->next_buffer];
  XASSERT(hdr->dwFlags & WHDR_PREPARED ||
          (hdr->dwFlags & WHDR_DONE && !(hdr->dwFlags & WHDR_INQUEUE)));
  stm->next_buffer = (stm->next_buffer + 1) % NBUFS;
  stm->free_buffers -= 1;

  return hdr;
}

static void
winmm_refill_stream(cubeb_stream * stm)
{
  WAVEHDR * hdr;
  long got;
  long wanted;
  MMRESULT r;

  EnterCriticalSection(&stm->lock);
  stm->free_buffers += 1;
  XASSERT(stm->free_buffers > 0 && stm->free_buffers <= NBUFS);

  if (stm->draining) {
    LeaveCriticalSection(&stm->lock);
    if (stm->free_buffers == NBUFS) {
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
    }
    SetEvent(stm->event);
    return;
  }

  if (stm->shutdown) {
    LeaveCriticalSection(&stm->lock);
    SetEvent(stm->event);
    return;
  }

  hdr = winmm_get_next_buffer(stm);

  wanted = (DWORD) stm->buffer_size / bytes_per_frame(stm->params);

  /* It is assumed that the caller is holding this lock.  It must be dropped
     during the callback to avoid deadlocks. */
  LeaveCriticalSection(&stm->lock);
  got = stm->data_callback(stm, stm->user_ptr, hdr->lpData, wanted);
  EnterCriticalSection(&stm->lock);
  if (got < 0) {
    LeaveCriticalSection(&stm->lock);
    /* XXX handle this case */
    XASSERT(0);
    return;
  } else if (got < wanted) {
    stm->draining = 1;
  }
  stm->written += got;

  XASSERT(hdr->dwFlags & WHDR_PREPARED);

  hdr->dwBufferLength = got * bytes_per_frame(stm->params);
  XASSERT(hdr->dwBufferLength <= stm->buffer_size);

  if (stm->soft_volume != -1.0) {
    if (stm->params.format == CUBEB_SAMPLE_FLOAT32NE) {
      float * b = (float *) hdr->lpData;
      uint32_t i;
      for (i = 0; i < got * stm->params.channels; i++) {
        b[i] *= stm->soft_volume;
      }
    } else {
      short * b = (short *) hdr->lpData;
      uint32_t i;
      for (i = 0; i < got * stm->params.channels; i++) {
        b[i] *= stm->soft_volume;
      }
    }
  }

  r = waveOutWrite(stm->waveout, hdr, sizeof(*hdr));
  if (r != MMSYSERR_NOERROR) {
    LeaveCriticalSection(&stm->lock);
    stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
    return;
  }

  LeaveCriticalSection(&stm->lock);
}

static unsigned __stdcall
winmm_buffer_thread(void * user_ptr)
{
  cubeb * ctx = (cubeb *) user_ptr;
  XASSERT(ctx);

  for (;;) {
    DWORD r;
    MSPS_PSLIST_ENTRY item;

    r = WaitForSingleObject(ctx->event, INFINITE);
    XASSERT(r == WAIT_OBJECT_0);

    /* Process work items in batches so that a single stream can't
       starve the others by continuously adding new work to the top of
       the work item stack. */
    item = InterlockedFlushSList_kex(ctx->work);
    while (item != NULL) {
      MSPS_PSLIST_ENTRY tmp = item;
      winmm_refill_stream(((struct cubeb_stream_item *) tmp)->stream);
      item = item->Next;
      _aligned_free(tmp);
    }

    if (ctx->shutdown) {
      break;
    }
  }

  return 0;
}

static void CALLBACK
winmm_buffer_callback(HWAVEOUT waveout, UINT msg, DWORD_PTR user_ptr, DWORD_PTR p1, DWORD_PTR p2)
{
  cubeb_stream * stm = (cubeb_stream *) user_ptr;
  struct cubeb_stream_item * item;

  if (msg != WOM_DONE) {
    return;
  }

  item = _aligned_malloc(sizeof(struct cubeb_stream_item), MEMORY_ALLOCATION_ALIGNMENT);
  XASSERT(item);
  item->stream = stm;
  InterlockedPushEntrySList_kex(stm->context->work, &item->head);

  SetEvent(stm->context->event);
}

static unsigned int
calculate_minimum_latency(void)
{
  OSVERSIONINFOEX osvi;
  DWORDLONG mask;

  /* Running under Terminal Services results in underruns with low latency. */
  if (GetSystemMetrics(SM_REMOTESESSION) == TRUE) {
    return 500;
  }

  /* Vista's WinMM implementation underruns when less than 200ms of audio is buffered. */
  memset(&osvi, 0, sizeof(OSVERSIONINFOEX));
  osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
  osvi.dwMajorVersion = 6;
  osvi.dwMinorVersion = 0;

  mask = 0;
  VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_EQUAL);
  VER_SET_CONDITION(mask, VER_MINORVERSION, VER_EQUAL);

  if (VerifyVersionInfo(&osvi, VER_MAJORVERSION | VER_MINORVERSION, mask) != 0) {
    return 200;
  }

  return 100;
}

static void winmm_destroy(cubeb * ctx);

/*static*/ int
winmm_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;

  XASSERT(context);
  *context = NULL;

  ctx = calloc(1, sizeof(*ctx));
  XASSERT(ctx);

  ctx->ops = &winmm_ops;

  ctx->work = _aligned_malloc(sizeof(*ctx->work), MEMORY_ALLOCATION_ALIGNMENT);
  XASSERT(ctx->work);
  InitializeSListHead_kex(ctx->work);

  ctx->event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!ctx->event) {
    winmm_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->thread = (HANDLE) _beginthreadex(NULL, 256 * 1024, winmm_buffer_thread, ctx, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!ctx->thread) {
    winmm_destroy(ctx);
    return CUBEB_ERROR;
  }

  SetThreadPriority(ctx->thread, THREAD_PRIORITY_TIME_CRITICAL);

  InitializeCriticalSection(&ctx->lock);
  ctx->active_streams = 0;

  ctx->minimum_latency = calculate_minimum_latency();

  *context = ctx;

  return CUBEB_OK;
}

static char const *
winmm_get_backend_id(cubeb * ctx)
{
  return "winmm";
}

static void
winmm_destroy(cubeb * ctx)
{
  DWORD r;

  XASSERT(ctx->active_streams == 0);
  XASSERT(!InterlockedPopEntrySList_kex(ctx->work));

  DeleteCriticalSection(&ctx->lock);

  if (ctx->thread) {
    ctx->shutdown = 1;
    SetEvent(ctx->event);
    r = WaitForSingleObject(ctx->thread, INFINITE);
    XASSERT(r == WAIT_OBJECT_0);
    CloseHandle(ctx->thread);
  }

  if (ctx->event) {
    CloseHandle(ctx->event);
  }

  _aligned_free(ctx->work);

  free(ctx);
}

static void winmm_stream_destroy(cubeb_stream * stm);

static int
winmm_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr)
{
  MMRESULT r;
  WAVEFORMATEXTENSIBLE wfx;
  cubeb_stream * stm;
  int i;
  size_t bufsz;

  XASSERT(context);
  XASSERT(stream);

  *stream = NULL;

  memset(&wfx, 0, sizeof(wfx));
  if (stream_params.channels > 2) {
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.cbSize = sizeof(wfx) - sizeof(wfx.Format);
  } else {
    wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
    if (stream_params.format == CUBEB_SAMPLE_FLOAT32LE) {
      wfx.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    }
    wfx.Format.cbSize = 0;
  }
  wfx.Format.nChannels = stream_params.channels;
  wfx.Format.nSamplesPerSec = stream_params.rate;

  /* XXX fix channel mappings */
  wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    wfx.Format.wBitsPerSample = 16;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    wfx.Format.wBitsPerSample = 32;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample * wfx.Format.nChannels) / 8;
  wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
  wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;

  EnterCriticalSection(&context->lock);
  /* CUBEB_STREAM_MAX is a horrible hack to avoid a situation where, when
     many streams are active at once, a subset of them will not consume (via
     playback) or release (via waveOutReset) their buffers. */
  if (context->active_streams >= CUBEB_STREAM_MAX) {
    LeaveCriticalSection(&context->lock);
    return CUBEB_ERROR;
  }
  context->active_streams += 1;
  LeaveCriticalSection(&context->lock);

  stm = calloc(1, sizeof(*stm));
  XASSERT(stm);

  stm->context = context;

  stm->params = stream_params;

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;
  stm->written = 0;

  if (latency < context->minimum_latency) {
    latency = context->minimum_latency;
  }

  bufsz = (size_t) (stm->params.rate / 1000.0 * latency * bytes_per_frame(stm->params) / NBUFS);
  if (bufsz % bytes_per_frame(stm->params) != 0) {
    bufsz += bytes_per_frame(stm->params) - (bufsz % bytes_per_frame(stm->params));
  }
  XASSERT(bufsz % bytes_per_frame(stm->params) == 0);

  stm->buffer_size = bufsz;

  InitializeCriticalSection(&stm->lock);

  stm->event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!stm->event) {
    winmm_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  stm->soft_volume = -1.0;

  /* winmm_buffer_callback will be called during waveOutOpen, so all
     other initialization must be complete before calling it. */
  r = waveOutOpen(&stm->waveout, WAVE_MAPPER, &wfx.Format,
                  (DWORD_PTR) winmm_buffer_callback, (DWORD_PTR) stm,
                  CALLBACK_FUNCTION);
  if (r != MMSYSERR_NOERROR) {
    winmm_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  r = waveOutPause(stm->waveout);
  if (r != MMSYSERR_NOERROR) {
    winmm_stream_destroy(stm);
    return CUBEB_ERROR;
  }


  for (i = 0; i < NBUFS; ++i) {
    WAVEHDR * hdr = &stm->buffers[i];

    hdr->lpData = calloc(1, bufsz);
    XASSERT(hdr->lpData);
    hdr->dwBufferLength = bufsz;
    hdr->dwFlags = 0;

    r = waveOutPrepareHeader(stm->waveout, hdr, sizeof(*hdr));
    if (r != MMSYSERR_NOERROR) {
      winmm_stream_destroy(stm);
      return CUBEB_ERROR;
    }

    winmm_refill_stream(stm);
  }

  stm->frame_size = bytes_per_frame(stm->params);
  stm->prev_pos_lo_dword = 0;
  stm->pos_hi_dword = 0;

  *stream = stm;

  return CUBEB_OK;
}

static void
winmm_stream_destroy(cubeb_stream * stm)
{
  DWORD r;
  int i;
  int enqueued;

  if (stm->waveout) {
    EnterCriticalSection(&stm->lock);
    stm->shutdown = 1;

    waveOutReset(stm->waveout);

    enqueued = NBUFS - stm->free_buffers;
    LeaveCriticalSection(&stm->lock);

    /* Wait for all blocks to complete. */
    while (enqueued > 0) {
      r = WaitForSingleObject(stm->event, INFINITE);
      XASSERT(r == WAIT_OBJECT_0);

      EnterCriticalSection(&stm->lock);
      enqueued = NBUFS - stm->free_buffers;
      LeaveCriticalSection(&stm->lock);
    }

    EnterCriticalSection(&stm->lock);

    for (i = 0; i < NBUFS; ++i) {
      if (stm->buffers[i].dwFlags & WHDR_PREPARED) {
        waveOutUnprepareHeader(stm->waveout, &stm->buffers[i], sizeof(stm->buffers[i]));
      }
    }

    waveOutClose(stm->waveout);

    LeaveCriticalSection(&stm->lock);
  }

  if (stm->event) {
    CloseHandle(stm->event);
  }

  DeleteCriticalSection(&stm->lock);

  for (i = 0; i < NBUFS; ++i) {
    free(stm->buffers[i].lpData);
  }

  EnterCriticalSection(&stm->context->lock);
  XASSERT(stm->context->active_streams >= 1);
  stm->context->active_streams -= 1;
  LeaveCriticalSection(&stm->context->lock);

  free(stm);
}

static int
winmm_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  XASSERT(ctx && max_channels);

  /* We don't support more than two channels in this backend. */
  *max_channels = 2;

  return CUBEB_OK;
}

static int
winmm_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency)
{
  // 100ms minimum, if we are not in a bizarre configuration.
  *latency = ctx->minimum_latency;

  return CUBEB_OK;
}

static int
winmm_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate)
{
  WAVEOUTCAPS woc;
  MMRESULT r;

  r = waveOutGetDevCaps(WAVE_MAPPER, &woc, sizeof(WAVEOUTCAPS));
  if (r != MMSYSERR_NOERROR) {
    return CUBEB_ERROR;
  }

  /* Check if we support 48kHz, but not 44.1kHz. */
  if (!(woc.dwFormats & WAVE_FORMAT_4S16) &&
      woc.dwFormats & WAVE_FORMAT_48S16) {
    *rate = 48000;
    return CUBEB_OK;
  }
  /* Prefer 44.1kHz between 44.1kHz and 48kHz. */
  *rate = 44100;

  return CUBEB_OK;
}

static int
winmm_stream_start(cubeb_stream * stm)
{
  MMRESULT r;

  EnterCriticalSection(&stm->lock);
  r = waveOutRestart(stm->waveout);
  LeaveCriticalSection(&stm->lock);

  if (r != MMSYSERR_NOERROR) {
    return CUBEB_ERROR;
  }

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STARTED);

  return CUBEB_OK;
}

static int
winmm_stream_stop(cubeb_stream * stm)
{
  MMRESULT r;

  EnterCriticalSection(&stm->lock);
  r = waveOutPause(stm->waveout);
  LeaveCriticalSection(&stm->lock);

  if (r != MMSYSERR_NOERROR) {
    return CUBEB_ERROR;
  }

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STOPPED);

  return CUBEB_OK;
}

/*
Microsoft wave audio docs say "samples are the preferred time format in which
to represent the current position", but relying on this causes problems on
Windows XP, the only OS cubeb_winmm is used on.

While the wdmaud.sys driver internally tracks a 64-bit position and ensures no
backward movement, the WinMM API limits the position returned from
waveOutGetPosition() to a 32-bit DWORD (this applies equally to XP x64). The
higher 32 bits are chopped off, and to an API consumer the position can appear
to move backward.

In theory, even a 32-bit TIME_SAMPLES position should provide plenty of
playback time for typical use cases before this pseudo wrap-around, e.g:
    (2^32 - 1)/48000 = ~24:51:18 for 48.0 kHz stereo;
    (2^32 - 1)/44100 = ~27:03:12 for 44.1 kHz stereo.
In reality, wdmaud.sys doesn't provide a TIME_SAMPLES position at all, only a
32-bit TIME_BYTES position, from which wdmaud.drv derives TIME_SAMPLES:
    SamplePos = (BytePos * 8) / BitsPerFrame,
    where BitsPerFrame = Channels * BitsPerSample,
Per dom\media\AudioSampleFormat.h, desktop builds always use 32-bit FLOAT32
samples, so the maximum for TIME_SAMPLES should be:
    (2^29 - 1)/48000 = ~03:06:25;
    (2^29 - 1)/44100 = ~03:22:54.
This might still be OK for typical browser usage, but there's also a bug in the
formula above: BytePos * 8 (BytePos << 3) is done on a 32-bit BytePos, without
first casting it to 64 bits, so the highest 3 bits, if set, would get shifted
out, and the maximum possible TIME_SAMPLES drops unacceptably low:
    (2^26 - 1)/48000 = ~00:23:18;
    (2^26 - 1)/44100 = ~00:25:22.

To work around these limitations, we just get the position in TIME_BYTES,
recover the 64-bit value, and do our own conversion to samples.
*/

/* Convert chopped 32-bit waveOutGetPosition() into 64-bit true position. */
static uint64_t
update_64bit_position(cubeb_stream * stm, DWORD pos_lo_dword)
{
  /* Caller should be holding stm->lock. */
  if (pos_lo_dword < stm->prev_pos_lo_dword) {
  	stm->pos_hi_dword++;
    /*LOG("waveOutGetPosition() has wrapped around: %#lx -> %#lx",
        stm->prev_pos_lo_dword, pos_lo_dword);
    LOG("Wrap-around count = %#lx", stm->pos_hi_dword);
    LOG("Current 64-bit position = %#llx",
        (((uint64_t) stm->pos_hi_dword)<<32) | ((uint64_t) pos_lo_dword));*/
  }
  stm->prev_pos_lo_dword = pos_lo_dword;

  return (((uint64_t) stm->pos_hi_dword)<<32) | ((uint64_t) pos_lo_dword);
}

static int
winmm_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  MMRESULT r;
  MMTIME time;

  EnterCriticalSection(&stm->lock);
  /* See the long comment above for why not just use TIME_SAMPLES here. */
  time.wType = TIME_BYTES;
  r = waveOutGetPosition(stm->waveout, &time, sizeof(time));

  if (r != MMSYSERR_NOERROR || time.wType != TIME_BYTES) {
    LeaveCriticalSection(&stm->lock);
    return CUBEB_ERROR;
  }

  *position = update_64bit_position(stm, time.u.cb) / stm->frame_size;
  LeaveCriticalSection(&stm->lock);

  return CUBEB_OK;
}

static int
winmm_stream_get_latency(cubeb_stream * stm, uint32_t * latency)
{
  MMRESULT r;
  MMTIME time;
  uint64_t position, written;

  EnterCriticalSection(&stm->lock);
  /* See the long comment above for why not just use TIME_SAMPLES here. */
  time.wType = TIME_BYTES;
  r = waveOutGetPosition(stm->waveout, &time, sizeof(time));

  if (r != MMSYSERR_NOERROR || time.wType != TIME_BYTES) {
    LeaveCriticalSection(&stm->lock);
    return CUBEB_ERROR;
  }

  position = update_64bit_position(stm, time.u.cb);
  written = stm->written;
  LeaveCriticalSection(&stm->lock);

  *latency = (uint32_t) (written - (position / stm->frame_size));

  return CUBEB_OK;
}

static int
winmm_stream_set_volume(cubeb_stream * stm, float volume)
{
  EnterCriticalSection(&stm->lock);
  stm->soft_volume = volume;
  LeaveCriticalSection(&stm->lock);
  return CUBEB_OK;
}

static struct cubeb_ops const winmm_ops = {
  /*.init =*/ winmm_init,
  /*.get_backend_id =*/ winmm_get_backend_id,
  /*.get_max_channel_count=*/ winmm_get_max_channel_count,
  /*.get_min_latency=*/ winmm_get_min_latency,
  /*.get_preferred_sample_rate =*/ winmm_get_preferred_sample_rate,
  /*.destroy =*/ winmm_destroy,
  /*.stream_init =*/ winmm_stream_init,
  /*.stream_destroy =*/ winmm_stream_destroy,
  /*.stream_start =*/ winmm_stream_start,
  /*.stream_stop =*/ winmm_stream_stop,
  /*.stream_get_position =*/ winmm_stream_get_position,
  /*.stream_get_latency = */ winmm_stream_get_latency,
  /*.stream_set_volume =*/ winmm_stream_set_volume,
  /*.stream_set_panning =*/ NULL,
  /*.stream_get_current_device =*/ NULL,
  /*.stream_device_destroy =*/ NULL,
  /*.stream_register_device_changed_callback=*/ NULL
};
