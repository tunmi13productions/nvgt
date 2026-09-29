#ifndef miniaudio_convolution_node_c
#define miniaudio_convolution_node_c

#include "ma_convolution_node.h"

#include <string.h> /* For memset()/memcpy(). */
#include <math.h>   /* For cos/sin. */
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#define MA_CONVOLUTION_NODE_SSE 1
#endif

/* MA_PI is only defined inside miniaudio's own implementation section, which this file does not pull in. */
#define MA_CONVOLUTION_NODE_PI 3.14159265358979323846

/* Past this the response is almost certainly a mistake, and 32 bit size arithmetic below could overflow. */
#define MA_CONVOLUTION_NODE_MAX_HEAP_BYTES ((ma_uint64)1024 * 1024 * 1024)

static ma_bool32 ma_convolution_node_is_pow2(ma_uint32 x)
{
    return x > 0 && (x & (x - 1)) == 0;
}

/* Transposed direct form II biquad. */
typedef struct
{
    float b0, b1, b2, a1, a2, z1, z2;
} ma_convolution_biquad;

static void ma_convolution_biquad_lowpass(ma_convolution_biquad* f, double fs, double fc, double q)
{
    double w0    = 2.0 * MA_CONVOLUTION_NODE_PI * fc / fs;
    double cw    = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0    = 1.0 + alpha;

    f->b0 = (float)((1.0 - cw) * 0.5 / a0);
    f->b1 = (float)((1.0 - cw) / a0);
    f->b2 = f->b0;
    f->a1 = (float)(-2.0 * cw / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

static float ma_convolution_biquad_run(ma_convolution_biquad* f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
#ifndef MA_CONVOLUTION_NODE_SSE
    /* A dying tail would otherwise sink into denormal range. SSE builds flush denormals for the whole process callback instead. */
    if (f->z1 > -1e-15f && f->z1 < 1e-15f) f->z1 = 0.0f;
    if (f->z2 > -1e-15f && f->z2 < 1e-15f) f->z2 = 0.0f;
#endif
    return y;
}

/* Sixth-order Butterworth low-pass: three biquads at the Qs of its pole pairs. */
typedef struct
{
    ma_convolution_biquad s[3];
} ma_convolution_lowpass6;

static void ma_convolution_lowpass6_set(ma_convolution_lowpass6* lp, double fs, double fc)
{
    static const double q[3] = {0.51763809, 0.70710678, 1.93185165};
    int i;
    for (i = 0; i < 3; i += 1) {
        ma_convolution_biquad_lowpass(&lp->s[i], fs, fc, q[i]);
    }
}

static float ma_convolution_lowpass6_run(ma_convolution_lowpass6* lp, float x)
{
    return ma_convolution_biquad_run(&lp->s[2], ma_convolution_biquad_run(&lp->s[1], ma_convolution_biquad_run(&lp->s[0], x)));
}

static void ma_convolution_lowpass6_reset(ma_convolution_lowpass6* lp)
{
    int i;
    for (i = 0; i < 3; i += 1) {
        lp->s[i].z1 = 0.0f;
        lp->s[i].z2 = 0.0f;
    }
}

/*
Everything one loaded impulse response needs, in a single allocation, so set_ir() can build it off the audio
thread and swap it in with one pointer write. The per-channel arrays are laid out channel after channel.
*/
struct ma_convolution_ir_state
{
    ma_uint32 channels;
    ma_uint32 div;          /* Rate divisor. */
    ma_uint32 B;            /* Block length at the reduced rate. */
    ma_uint32 N;            /* FFT size, 2 * B. */
    ma_uint32 P;            /* Partitions. */
    ma_uint32 span;         /* B * div: device frames per block. */
    ma_uint32 pos;          /* Newest slot in the frequency delay line. */
    ma_uint32 bpos;         /* Reduced-rate position inside the block being filled. */
    ma_uint32 phase;        /* Which of the div input frames this one is. */
    ma_uint64 tailFrames;   /* Device frames of silent input it takes the delay line to hold nothing but zeros. */
    ma_uint64 idleFrames;
    ma_bool32 asleep;
    ma_uint32* pRev;        /* [N] bit reversal. */
    float* pCos;            /* [N/2] */
    float* pSin;            /* [N/2] */
    float* pHRe;            /* [channels][P * N] response spectrum. */
    float* pHIm;
    float* pXRe;            /* [channels][P * N] input spectrum history. */
    float* pXIm;
    float* pPrev;           /* [channels][B] previous block, the "save" of overlap-save. */
    float* pInBlk;          /* [channels][B] block being filled. */
    float* pWet;            /* [channels][span] last rendered block of wet at the device rate. */
    float* pYRe;            /* [N] */
    float* pYIm;
    ma_convolution_lowpass6* pAA; /* [channels] anti-alias before decimating. */
    ma_convolution_lowpass6* pAI; /* [channels] anti-image after zero-stuffing. */
    void* pHeap;
};

static void ma_convolution_ir_state_free(ma_convolution_ir_state* pState, const ma_allocation_callbacks* pAllocationCallbacks)
{
    if (pState == NULL) {
        return;
    }
    ma_free(pState->pHeap, pAllocationCallbacks);
    ma_free(pState, pAllocationCallbacks);
}

/* Radix-2 FFT over separate real and imaginary arrays, using the state's bit-reversal and twiddle tables. */
static void ma_convolution_fft(const ma_convolution_ir_state* pState, float* pRe, float* pIm, ma_bool32 inverse)
{
    ma_uint32 N = pState->N;
    ma_uint32 i;
    ma_uint32 len;
    float sign = inverse ? 1.0f : -1.0f;

    for (i = 1; i < N; i += 1) {
        ma_uint32 j = pState->pRev[i];
        if (i < j) {
            float t;
            t = pRe[i]; pRe[i] = pRe[j]; pRe[j] = t;
            t = pIm[i]; pIm[i] = pIm[j]; pIm[j] = t;
        }
    }

    for (len = 2; len <= N; len <<= 1) {
        ma_uint32 half = len >> 1;
        ma_uint32 step = N / len;
        ma_uint32 k;
        for (k = 0; k < half; k += 1) {
            float wr = pState->pCos[k * step];
            float wi = sign * pState->pSin[k * step];
            ma_uint32 a;
            for (a = k; a < N; a += len) {
                ma_uint32 b = a + half;
                float vr = pRe[b] * wr - pIm[b] * wi;
                float vi = pRe[b] * wi + pIm[b] * wr;
                pRe[b] = pRe[a] - vr;
                pIm[b] = pIm[a] - vi;
                pRe[a] += vr;
                pIm[a] += vi;
            }
        }
    }

    if (inverse) {
        float inv = 1.0f / (float)N;
        for (i = 0; i < N; i += 1) {
            pRe[i] *= inv;
            pIm[i] *= inv;
        }
    }
}

/* y += x * h, bin by bin. This is where nearly all of a convolver's time goes. */
static void ma_convolution_accumulate(const float* xr, const float* xi, const float* hr, const float* hi, float* yr, float* yi, ma_uint32 n)
{
    ma_uint32 i = 0;
#ifdef MA_CONVOLUTION_NODE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 a = _mm_loadu_ps(xr + i);
        __m128 b = _mm_loadu_ps(xi + i);
        __m128 c = _mm_loadu_ps(hr + i);
        __m128 d = _mm_loadu_ps(hi + i);
        _mm_storeu_ps(yr + i, _mm_add_ps(_mm_loadu_ps(yr + i), _mm_sub_ps(_mm_mul_ps(a, c), _mm_mul_ps(b, d))));
        _mm_storeu_ps(yi + i, _mm_add_ps(_mm_loadu_ps(yi + i), _mm_add_ps(_mm_mul_ps(a, d), _mm_mul_ps(b, c))));
    }
#endif
    for (; i < n; i += 1) {
        yr[i] += xr[i] * hr[i] - xi[i] * hi[i];
        yi[i] += xr[i] * hi[i] + xi[i] * hr[i];
    }
}

/* Convolves the completed block of every channel and replaces pWet with a span of wet per channel. */
static void ma_convolution_render(ma_convolution_ir_state* pState)
{
    ma_uint32 B = pState->B;
    ma_uint32 N = pState->N;
    ma_uint32 P = pState->P;
    ma_uint64 history = (ma_uint64)P * N;
    ma_uint32 c;

    for (c = 0; c < pState->channels; c += 1) {
        float* pXRe   = pState->pXRe + c * history;
        float* pXIm   = pState->pXIm + c * history;
        const float* pHRe = pState->pHRe + c * history;
        const float* pHIm = pState->pHIm + c * history;
        float* pPrev  = pState->pPrev + (ma_uint64)c * B;
        float* pIn    = pState->pInBlk + (ma_uint64)c * B;
        float* pWet   = pState->pWet + (ma_uint64)c * pState->span;
        float* xr     = pXRe + (ma_uint64)pState->pos * N;
        float* xi     = pXIm + (ma_uint64)pState->pos * N;
        const float* pOut;
        ma_uint32 p;

        /* The newest spectrum is built straight in its slot of the history: the previous block, then this one. */
        memcpy(xr, pPrev, sizeof(float) * B);
        memcpy(xr + B, pIn, sizeof(float) * B);
        memset(xi, 0, sizeof(float) * N);
        memcpy(pPrev, pIn, sizeof(float) * B);
        ma_convolution_fft(pState, xr, xi, MA_FALSE);

        memset(pState->pYRe, 0, sizeof(float) * N);
        memset(pState->pYIm, 0, sizeof(float) * N);
        for (p = 0; p < P; p += 1) {
            ma_uint64 slot = (pState->pos + P - p) % P;
            ma_convolution_accumulate(pXRe + slot * N, pXIm + slot * N, pHRe + (ma_uint64)p * N, pHIm + (ma_uint64)p * N, pState->pYRe, pState->pYIm, N);
        }
        ma_convolution_fft(pState, pState->pYRe, pState->pYIm, MA_TRUE);
        pOut = pState->pYRe + (N - B); /* Overlap-save: only the last B are valid. */

        if (pState->div == 1) {
            memcpy(pWet, pOut, sizeof(float) * B);
        } else {
            /* Zero-stuff back up to the device rate, scaled by div to make up for the zeros, and filter out the images. */
            ma_convolution_lowpass6* pImage = &pState->pAI[c];
            float g = (float)pState->div;
            ma_uint32 w = 0;
            ma_uint32 k;
            ma_uint32 z;
            for (k = 0; k < B; k += 1) {
                pWet[w++] = ma_convolution_lowpass6_run(pImage, pOut[k] * g);
                for (z = 1; z < pState->div; z += 1) {
                    pWet[w++] = ma_convolution_lowpass6_run(pImage, 0.0f);
                }
            }
        }
    }

    pState->pos = (pState->pos + 1) % P;
}

/* Called once the input has been silent for longer than the tail. Clearing everything is exact: a history of silent blocks is all zeros anyway. */
static void ma_convolution_go_idle(ma_convolution_ir_state* pState)
{
    ma_uint64 history = (ma_uint64)pState->P * pState->N * pState->channels;
    ma_uint32 c;

    memset(pState->pXRe, 0, sizeof(float) * history);
    memset(pState->pXIm, 0, sizeof(float) * history);
    memset(pState->pPrev, 0, sizeof(float) * pState->B * pState->channels);
    memset(pState->pInBlk, 0, sizeof(float) * pState->B * pState->channels);
    memset(pState->pWet, 0, sizeof(float) * pState->span * pState->channels);
    for (c = 0; c < pState->channels; c += 1) {
        ma_convolution_lowpass6_reset(&pState->pAA[c]);
        ma_convolution_lowpass6_reset(&pState->pAI[c]);
    }
    pState->asleep = MA_TRUE;
}

MA_API ma_convolution_node_config ma_convolution_node_config_init(ma_uint32 channels, ma_uint32 sampleRate)
{
    ma_convolution_node_config config;

    memset(&config, 0, sizeof(config));
    config.nodeConfig    = ma_node_config_init();  /* Input and output channels will be set in ma_convolution_node_init(). */
    config.channels      = channels;
    config.sampleRate    = sampleRate;
    config.partitionSize = 512;

    return config;
}

static void ma_convolution_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    ma_convolution_node* pConvolutionNode = (ma_convolution_node*)pNode;
    ma_convolution_ir_state* pState;
    ma_uint32 frameCount = *pFrameCountOut;
    ma_uint32 channels   = pConvolutionNode->channels;
    const float* pIn     = ppFramesIn[0];
    float* pOut          = ppFramesOut[0];
    ma_uint64 total      = (ma_uint64)frameCount * channels;
    float wetGain;
    float dryGain;
    ma_bool32 silent = MA_TRUE;
    ma_uint64 i;
    ma_uint32 iFrame;
    ma_uint32 iChannel;
#ifdef MA_CONVOLUTION_NODE_SSE
    unsigned int csr;
#endif

    (void)pFrameCountIn;

    ma_mutex_lock(&pConvolutionNode->lock);

    pState = pConvolutionNode->pState;
    if (pState == NULL) {
        /* No impulse response loaded yet: passthrough. */
        ma_copy_pcm_frames(pOut, pIn, frameCount, ma_format_f32, channels);
        ma_mutex_unlock(&pConvolutionNode->lock);
        return;
    }

    wetGain = pConvolutionNode->wet;
    dryGain = pConvolutionNode->dry;

    for (i = 0; i < total; i += 1) {
        if (pIn[i] > 1e-7f || pIn[i] < -1e-7f) {
            silent = MA_FALSE;
            break;
        }
    }
    if (silent) {
        /* Once the input has been quiet for longer than the tail there is nothing left to compute. */
        if (!pState->asleep) {
            pState->idleFrames += frameCount;
            if (pState->idleFrames > pState->tailFrames) {
                ma_convolution_go_idle(pState);
            }
        }
        if (pState->asleep) {
            for (i = 0; i < total; i += 1) {
                pOut[i] = pIn[i] * dryGain;
            }
            ma_mutex_unlock(&pConvolutionNode->lock);
            return;
        }
    } else {
        pState->idleFrames = 0;
        pState->asleep = MA_FALSE;
    }

#ifdef MA_CONVOLUTION_NODE_SSE
    /* A decaying tail would otherwise sink into denormal range, where every operation on it is many times slower. */
    csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040); /* Flush to zero, and treat denormal inputs as zero. */
#endif

    for (iFrame = 0; iFrame < frameCount; iFrame += 1) {
        ma_uint32 at = pState->bpos * pState->div + pState->phase;
        for (iChannel = 0; iChannel < channels; iChannel += 1) {
            float in  = pIn[iFrame * channels + iChannel];
            float wet = pState->pWet[(ma_uint64)iChannel * pState->span + at];
            float x   = in;

            pOut[iFrame * channels + iChannel] = in * dryGain + wet * wetGain;
            if (pState->div > 1) {
                x = ma_convolution_lowpass6_run(&pState->pAA[iChannel], x);
            }
            if (pState->phase == 0) {
                pState->pInBlk[(ma_uint64)iChannel * pState->B + pState->bpos] = x;
            }
        }

        pState->phase += 1;
        if (pState->phase == pState->div) {
            pState->phase = 0;
            pState->bpos += 1;
            if (pState->bpos == pState->B) {
                pState->bpos = 0;
                ma_convolution_render(pState);
            }
        }
    }

#ifdef MA_CONVOLUTION_NODE_SSE
    _mm_setcsr(csr);
#endif

    ma_mutex_unlock(&pConvolutionNode->lock);
}

static ma_node_vtable g_ma_convolution_node_vtable =
{
    ma_convolution_node_process_pcm_frames,
    NULL,
    1,  /* 1 input bus. */
    1,  /* 1 output bus. */
    MA_NODE_FLAG_CONTINUOUS_PROCESSING  /* The convolution tail must keep processing after the input goes quiet. */
};

MA_API ma_result ma_convolution_node_init(ma_node_graph* pNodeGraph, const ma_convolution_node_config* pConfig, const ma_allocation_callbacks* pAllocationCallbacks, ma_convolution_node* pConvolutionNode)
{
    ma_result result;
    ma_node_config baseConfig;
    ma_uint32 partitionSize;

    if (pConvolutionNode == NULL) {
        return MA_INVALID_ARGS;
    }

    memset(pConvolutionNode, 0, sizeof(*pConvolutionNode));

    if (pConfig == NULL || pConfig->channels == 0 || pConfig->sampleRate == 0) {
        return MA_INVALID_ARGS;
    }

    partitionSize = (pConfig->partitionSize == 0) ? 512 : pConfig->partitionSize;
    if (!ma_convolution_node_is_pow2(partitionSize) || partitionSize < 16) {
        return MA_INVALID_ARGS;
    }

    result = ma_mutex_init(&pConvolutionNode->lock);
    if (result != MA_SUCCESS) {
        return result;
    }

    pConvolutionNode->channels      = pConfig->channels;
    pConvolutionNode->sampleRate    = pConfig->sampleRate;
    pConvolutionNode->partitionSize = partitionSize;
    pConvolutionNode->pState        = NULL;
    pConvolutionNode->wet           = 1.0f;
    pConvolutionNode->dry           = 0.0f;

    baseConfig = pConfig->nodeConfig;
    baseConfig.vtable          = &g_ma_convolution_node_vtable;
    baseConfig.pInputChannels  = &pConfig->channels;
    baseConfig.pOutputChannels = &pConfig->channels;

    result = ma_node_init(pNodeGraph, &baseConfig, pAllocationCallbacks, &pConvolutionNode->baseNode);
    if (result != MA_SUCCESS) {
        ma_mutex_uninit(&pConvolutionNode->lock);
        return result;
    }

    return MA_SUCCESS;
}

MA_API void ma_convolution_node_uninit(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    if (pConvolutionNode == NULL) {
        return;
    }

    /* The base node is always uninitialized first, so that nothing is still reading the state being freed below. */
    ma_node_uninit(&pConvolutionNode->baseNode, pAllocationCallbacks);
    ma_convolution_ir_state_free(pConvolutionNode->pState, pAllocationCallbacks);
    pConvolutionNode->pState = NULL;
    ma_mutex_uninit(&pConvolutionNode->lock);
}

static size_t ma_convolution_align16(size_t n)
{
    return (n + 15) & ~(size_t)15;
}

MA_API ma_result ma_convolution_node_set_ir_ex(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, float tailSeconds, ma_uint32 rateDivisor, const ma_allocation_callbacks* pAllocationCallbacks)
{
    ma_convolution_ir_state* pState;
    ma_convolution_ir_state* pOld;
    ma_uint32 channels;
    ma_uint32 div;
    ma_uint32 B;
    ma_uint32 N;
    ma_uint32 P;
    ma_uint64 L;
    ma_uint64 Ld;
    ma_uint64 fade = 0;
    ma_bool32 cut = MA_FALSE;
    ma_uint64 history;
    size_t offRev, offCos, offSin, offHRe, offHIm, offXRe, offXIm, offPrev, offIn, offWet, offYRe, offYIm, offAA, offAI, heapSize;
    ma_uint8* pHeap;
    float* pTmp;
    double fs;
    double fc;
    ma_uint32 c;
    ma_uint32 prevSrc = 0xFFFFFFFF;
    ma_uint64 i;

    if (pConvolutionNode == NULL || pFramesIn == NULL || irFrameCount == 0 || irChannels == 0) {
        return MA_INVALID_ARGS;
    }

    channels = pConvolutionNode->channels;
    fs       = (double)pConvolutionNode->sampleRate;
    div      = rateDivisor < 1 ? 1 : rateDivisor;
    if (!ma_convolution_node_is_pow2(div) || pConvolutionNode->partitionSize / div < 8) {
        return MA_INVALID_ARGS;
    }
    B = pConvolutionNode->partitionSize / div;
    N = B * 2;
    fc = 0.8 * fs / (2.0 * div);

    L = irFrameCount;
    if (tailSeconds > 0.0f) {
        ma_uint64 maxLen = (ma_uint64)(fs * tailSeconds);
        if (maxLen < 1) maxLen = 1;
        if (L > maxLen) {
            L = maxLen;
            cut = MA_TRUE;
            /* About 10 ms, so the cut does not click. */
            fade = (ma_uint64)(fs * 0.01);
            if (fade < 1) fade = 1;
            if (fade > L) fade = L;
        }
    }
    Ld = (L + div - 1) / div;
    if ((Ld + B - 1) / B > 0xFFFFFFu) {
        return MA_OUT_OF_MEMORY;
    }
    P = (ma_uint32)((Ld + B - 1) / B);
    if (P == 0) P = 1;
    history = (ma_uint64)P * N * channels;
    if (history * sizeof(float) * 4 > MA_CONVOLUTION_NODE_MAX_HEAP_BYTES) {
        return MA_OUT_OF_MEMORY;
    }

    heapSize = 0;
    offRev  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(ma_uint32) * N);
    offCos  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (N / 2));
    offSin  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (N / 2));
    offHRe  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)history);
    offHIm  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)history);
    offXRe  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)history);
    offXIm  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)history);
    offPrev = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)B * channels);
    offIn   = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)B * channels);
    offWet  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * (size_t)B * div * channels);
    offYRe  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * N);
    offYIm  = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(float) * N);
    offAA   = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(ma_convolution_lowpass6) * channels);
    offAI   = heapSize; heapSize = ma_convolution_align16(heapSize + sizeof(ma_convolution_lowpass6) * channels);

    pState = (ma_convolution_ir_state*)ma_malloc(sizeof(*pState), pAllocationCallbacks);
    if (pState == NULL) {
        return MA_OUT_OF_MEMORY;
    }
    memset(pState, 0, sizeof(*pState));
    pHeap = (ma_uint8*)ma_malloc(heapSize, pAllocationCallbacks);
    pTmp  = (float*)ma_malloc(sizeof(float) * (size_t)L, pAllocationCallbacks);
    if (pHeap == NULL || pTmp == NULL) {
        ma_free(pHeap, pAllocationCallbacks);
        ma_free(pTmp, pAllocationCallbacks);
        ma_free(pState, pAllocationCallbacks);
        return MA_OUT_OF_MEMORY;
    }
    memset(pHeap, 0, heapSize);

    pState->pHeap      = pHeap;
    pState->channels   = channels;
    pState->div        = div;
    pState->B          = B;
    pState->N          = N;
    pState->P          = P;
    pState->span       = B * div;
    pState->tailFrames = ((ma_uint64)P + 2) * pState->span;
    pState->pRev       = (ma_uint32*)(pHeap + offRev);
    pState->pCos       = (float*)(pHeap + offCos);
    pState->pSin       = (float*)(pHeap + offSin);
    pState->pHRe       = (float*)(pHeap + offHRe);
    pState->pHIm       = (float*)(pHeap + offHIm);
    pState->pXRe       = (float*)(pHeap + offXRe);
    pState->pXIm       = (float*)(pHeap + offXIm);
    pState->pPrev      = (float*)(pHeap + offPrev);
    pState->pInBlk     = (float*)(pHeap + offIn);
    pState->pWet       = (float*)(pHeap + offWet);
    pState->pYRe       = (float*)(pHeap + offYRe);
    pState->pYIm       = (float*)(pHeap + offYIm);
    pState->pAA        = (ma_convolution_lowpass6*)(pHeap + offAA);
    pState->pAI        = (ma_convolution_lowpass6*)(pHeap + offAI);

    {
        ma_uint32 j = 0;
        ma_uint32 k;
        pState->pRev[0] = 0;
        for (k = 1; k < N; k += 1) {
            ma_uint32 bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            pState->pRev[k] = j;
        }
        for (k = 0; k < N / 2; k += 1) {
            double a = 2.0 * MA_CONVOLUTION_NODE_PI * (double)k / (double)N;
            pState->pCos[k] = (float)cos(a);
            pState->pSin[k] = (float)sin(a);
        }
    }

    for (c = 0; c < channels; c += 1) {
        /* A mono IR is broadcast to every node channel; otherwise the last available IR channel covers any node channel past the end of the IR. */
        ma_uint32 srcChannel = (irChannels == 1) ? 0 : ((c < irChannels) ? c : (irChannels - 1));
        float* pHRe = pState->pHRe + (ma_uint64)c * P * N;
        float* pHIm = pState->pHIm + (ma_uint64)c * P * N;
        ma_uint32 p;

        if (srcChannel == prevSrc) {
            memcpy(pHRe, pHRe - (ma_uint64)P * N, sizeof(float) * (size_t)P * N);
            memcpy(pHIm, pHIm - (ma_uint64)P * N, sizeof(float) * (size_t)P * N);
            continue;
        }
        prevSrc = srcChannel;

        for (i = 0; i < L; i += 1) {
            pTmp[i] = pFramesIn[i * irChannels + srcChannel];
        }
        if (cut) {
            for (i = 0; i < fade; i += 1) {
                pTmp[L - fade + i] *= 1.0f - (float)i / (float)fade;
            }
        }
        if (div > 1) {
            /* Low-passed forwards and back so the response keeps its timing, then every div-th sample, scaled by div so the sum over the sparser response comes out the same size. */
            ma_convolution_lowpass6 lp;
            ma_convolution_lowpass6_set(&lp, fs, fc);
            for (i = 0; i < L; i += 1) pTmp[i] = ma_convolution_lowpass6_run(&lp, pTmp[i]);
            ma_convolution_lowpass6_reset(&lp);
            for (i = L; i-- > 0;) pTmp[i] = ma_convolution_lowpass6_run(&lp, pTmp[i]);
            for (i = 0; i < Ld; i += 1) pTmp[i] = pTmp[i * div] * (float)div;
        }

        for (p = 0; p < P; p += 1) {
            float* hr = pHRe + (ma_uint64)p * N;
            float* hi = pHIm + (ma_uint64)p * N;
            ma_uint64 base = (ma_uint64)p * B;
            ma_uint32 k;
            for (k = 0; k < B && base + k < Ld; k += 1) {
                hr[k] = pTmp[base + k];
            }
            ma_convolution_fft(pState, hr, hi, MA_FALSE);
        }
    }
    ma_free(pTmp, pAllocationCallbacks);

    if (div > 1) {
        for (c = 0; c < channels; c += 1) {
            ma_convolution_lowpass6_set(&pState->pAA[c], fs, fc);
            ma_convolution_lowpass6_set(&pState->pAI[c], fs, fc);
        }
    }

    /* The expensive part is done. The audio thread only ever waits for a pointer swap, and the old state is freed after the lock is released. */
    ma_mutex_lock(&pConvolutionNode->lock);
    pOld = pConvolutionNode->pState;
    pConvolutionNode->pState = pState;
    pConvolutionNode->irLengthInFrames = L;
    ma_mutex_unlock(&pConvolutionNode->lock);
    ma_convolution_ir_state_free(pOld, pAllocationCallbacks);

    return MA_SUCCESS;
}

MA_API ma_result ma_convolution_node_set_ir(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, const ma_allocation_callbacks* pAllocationCallbacks)
{
    return ma_convolution_node_set_ir_ex(pConvolutionNode, pFramesIn, irFrameCount, irChannels, 0.0f, 1, pAllocationCallbacks);
}

MA_API void ma_convolution_node_clear_ir(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    ma_convolution_ir_state* pOld;

    if (pConvolutionNode == NULL) {
        return;
    }

    ma_mutex_lock(&pConvolutionNode->lock);
    pOld = pConvolutionNode->pState;
    pConvolutionNode->pState = NULL;
    pConvolutionNode->irLengthInFrames = 0;
    ma_mutex_unlock(&pConvolutionNode->lock);
    ma_convolution_ir_state_free(pOld, pAllocationCallbacks);
}

MA_API void ma_convolution_node_set_wet(ma_convolution_node* pConvolutionNode, float wet)
{
    if (pConvolutionNode == NULL) return;
    pConvolutionNode->wet = wet;
}
MA_API float ma_convolution_node_get_wet(const ma_convolution_node* pConvolutionNode)
{
    return pConvolutionNode ? pConvolutionNode->wet : 0.0f;
}
MA_API void ma_convolution_node_set_dry(ma_convolution_node* pConvolutionNode, float dry)
{
    if (pConvolutionNode == NULL) return;
    pConvolutionNode->dry = dry;
}
MA_API float ma_convolution_node_get_dry(const ma_convolution_node* pConvolutionNode)
{
    return pConvolutionNode ? pConvolutionNode->dry : 0.0f;
}
MA_API ma_uint64 ma_convolution_node_get_ir_length_in_frames(const ma_convolution_node* pConvolutionNode)
{
    return pConvolutionNode ? pConvolutionNode->irLengthInFrames : 0;
}
MA_API ma_uint32 ma_convolution_node_get_partition_size(const ma_convolution_node* pConvolutionNode)
{
    return pConvolutionNode ? pConvolutionNode->partitionSize : 0;
}

#endif  /* miniaudio_convolution_node_c */
