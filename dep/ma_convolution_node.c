#ifndef miniaudio_convolution_node_c
#define miniaudio_convolution_node_c

#include "ma_convolution_node.h"

#include <string.h> /* For memset(). */
#include <math.h>   /* For cosf/sinf. */

/* MA_PI is only defined inside miniaudio's own implementation section, which this file does not pull in. */
#define MA_CONVOLUTION_NODE_PI 3.14159265358979323846264f

static ma_bool32 ma_convolution_node_is_pow2(ma_uint32 x)
{
    return x > 0 && (x & (x - 1)) == 0;
}

MA_API ma_convolution_node_config ma_convolution_node_config_init(ma_uint32 channels, ma_uint32 sampleRate)
{
    ma_convolution_node_config config;

    memset(&config, 0, sizeof(config));
    config.nodeConfig    = ma_node_config_init();  /* Input and output channels will be set in ma_convolution_node_init(). */
    config.channels      = channels;
    config.sampleRate    = sampleRate;
    config.partitionSize = 1024;

    return config;
}


/* In-place iterative radix-2 Cooley-Tukey FFT/IFFT over separate real and imaginary arrays. n must be a power of two. */
static void ma_convolution_node_fft(float* pRe, float* pIm, ma_uint32 n, ma_bool32 inverse)
{
    ma_uint32 i;
    ma_uint32 j;
    ma_uint32 k;
    ma_uint32 len;

    j = 0;
    for (i = 0; i < n - 1; i += 1) {
        if (i < j) {
            float t;
            t = pRe[i]; pRe[i] = pRe[j]; pRe[j] = t;
            t = pIm[i]; pIm[i] = pIm[j]; pIm[j] = t;
        }
        k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }

    for (len = 2; len <= n; len *= 2) {
        float angle = (inverse ? 2.0f : -2.0f) * MA_CONVOLUTION_NODE_PI / (float)len;
        float wnRe  = cosf(angle);
        float wnIm  = sinf(angle);
        ma_uint32 half = len / 2;

        for (i = 0; i < n; i += len) {
            float wRe = 1.0f;
            float wIm = 0.0f;
            ma_uint32 jj;

            for (jj = 0; jj < half; jj += 1) {
                ma_uint32 a = i + jj;
                ma_uint32 b = a + half;
                float uRe = pRe[a];
                float uIm = pIm[a];
                float tRe = wRe * pRe[b] - wIm * pIm[b];
                float tIm = wRe * pIm[b] + wIm * pRe[b];
                float nwRe;
                float nwIm;

                pRe[a] = uRe + tRe;
                pIm[a] = uIm + tIm;
                pRe[b] = uRe - tRe;
                pIm[b] = uIm - tIm;

                nwRe = wRe * wnRe - wIm * wnIm;
                nwIm = wRe * wnIm + wIm * wnRe;
                wRe = nwRe;
                wIm = nwIm;
            }
        }
    }

    if (inverse) {
        float invN = 1.0f / (float)n;
        for (i = 0; i < n; i += 1) {
            pRe[i] *= invN;
            pIm[i] *= invN;
        }
    }
}

/* Frees only the per-channel IR/FDL/buffer allocations. The top level channel-indexed pointer arrays and the shared scratch buffers outlive this. Caller must hold pConvolutionNode->lock, or be in uninit where nothing else can be touching the node. */
static void ma_convolution_node_free_ir(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    ma_uint32 iChannel;

    if (pConvolutionNode->ppIrSpectrumRe == NULL) {
        return;
    }

    for (iChannel = 0; iChannel < pConvolutionNode->channels; iChannel += 1) {
        ma_free(pConvolutionNode->ppIrSpectrumRe[iChannel], pAllocationCallbacks);
        ma_free(pConvolutionNode->ppIrSpectrumIm[iChannel], pAllocationCallbacks);
        ma_free(pConvolutionNode->ppFdlRe[iChannel], pAllocationCallbacks);
        ma_free(pConvolutionNode->ppFdlIm[iChannel], pAllocationCallbacks);
        ma_free(pConvolutionNode->ppInputBuffer[iChannel], pAllocationCallbacks);
        ma_free(pConvolutionNode->ppOutputBuffer[iChannel], pAllocationCallbacks);
        pConvolutionNode->ppIrSpectrumRe[iChannel]  = NULL;
        pConvolutionNode->ppIrSpectrumIm[iChannel]  = NULL;
        pConvolutionNode->ppFdlRe[iChannel]         = NULL;
        pConvolutionNode->ppFdlIm[iChannel]         = NULL;
        pConvolutionNode->ppInputBuffer[iChannel]   = NULL;
        pConvolutionNode->ppOutputBuffer[iChannel]  = NULL;
    }

    pConvolutionNode->numPartitions     = 0;
    pConvolutionNode->irLengthInFrames  = 0;
    pConvolutionNode->inputPos          = 0;
    pConvolutionNode->fdlPos            = 0;
}

MA_API void ma_convolution_node_clear_ir(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    if (pConvolutionNode == NULL) {
        return;
    }

    ma_mutex_lock(&pConvolutionNode->lock);
    ma_convolution_node_free_ir(pConvolutionNode, pAllocationCallbacks);
    ma_mutex_unlock(&pConvolutionNode->lock);
}

/* Processes exactly one partition (partitionSize frames, already fully buffered) for every channel, advancing the frequency delay line and refilling the overlap-add output. Caller holds the lock. */
static void ma_convolution_node_process_partition(ma_convolution_node* pConvolutionNode)
{
    ma_uint32 fftSize         = pConvolutionNode->fftSize;
    ma_uint32 partitionSize   = pConvolutionNode->partitionSize;
    ma_uint32 numPartitions   = pConvolutionNode->numPartitions;
    ma_uint32 fdlPos          = pConvolutionNode->fdlPos;
    float* pScratchRe         = pConvolutionNode->pScratchRe;
    float* pScratchIm         = pConvolutionNode->pScratchIm;
    float* pAccumRe           = pConvolutionNode->pAccumRe;
    float* pAccumIm           = pConvolutionNode->pAccumIm;
    ma_uint32 iChannel;
    ma_uint32 iPart;
    ma_uint32 j;

    for (iChannel = 0; iChannel < pConvolutionNode->channels; iChannel += 1) {
        float* pInputBuf  = pConvolutionNode->ppInputBuffer[iChannel];
        float* pOutputBuf = pConvolutionNode->ppOutputBuffer[iChannel];
        float* pFdlRe     = pConvolutionNode->ppFdlRe[iChannel];
        float* pFdlIm     = pConvolutionNode->ppFdlIm[iChannel];
        const float* pIrRe = pConvolutionNode->ppIrSpectrumRe[iChannel];
        const float* pIrIm = pConvolutionNode->ppIrSpectrumIm[iChannel];
        ma_uint32 fdlBase;

        for (j = 0; j < partitionSize; j += 1) {
            pScratchRe[j] = pInputBuf[j];
            pScratchIm[j] = 0.0f;
        }
        for (j = partitionSize; j < fftSize; j += 1) {
            pScratchRe[j] = 0.0f;
            pScratchIm[j] = 0.0f;
        }
        ma_convolution_node_fft(pScratchRe, pScratchIm, fftSize, MA_FALSE);

        fdlBase = fdlPos * fftSize;
        for (j = 0; j < fftSize; j += 1) {
            pFdlRe[fdlBase + j] = pScratchRe[j];
            pFdlIm[fdlBase + j] = pScratchIm[j];
        }

        for (j = 0; j < fftSize; j += 1) {
            pAccumRe[j] = 0.0f;
            pAccumIm[j] = 0.0f;
        }
        for (iPart = 0; iPart < numPartitions; iPart += 1) {
            ma_uint32 idx  = (fdlPos + numPartitions - iPart) % numPartitions;
            ma_uint32 base = idx * fftSize;
            ma_uint32 irBase = iPart * fftSize;

            for (j = 0; j < fftSize; j += 1) {
                float aRe = pFdlRe[base + j];
                float aIm = pFdlIm[base + j];
                float bRe = pIrRe[irBase + j];
                float bIm = pIrIm[irBase + j];

                pAccumRe[j] += aRe * bRe - aIm * bIm;
                pAccumIm[j] += aRe * bIm + aIm * bRe;
            }
        }
        ma_convolution_node_fft(pAccumRe, pAccumIm, fftSize, MA_TRUE);

        /* Overlap-add: the first half combines this partition's pending tail with the new block; the second half becomes the pending tail for next time. */
        for (j = 0; j < partitionSize; j += 1) {
            pOutputBuf[j] = pOutputBuf[j + partitionSize] + pAccumRe[j];
        }
        for (j = 0; j < partitionSize; j += 1) {
            pOutputBuf[j + partitionSize] = pAccumRe[j + partitionSize];
        }
    }

    pConvolutionNode->fdlPos = (fdlPos + 1) % numPartitions;
}

static void ma_convolution_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    ma_convolution_node* pConvolutionNode = (ma_convolution_node*)pNode;
    ma_uint32 frameCount = *pFrameCountOut;
    ma_uint32 channels   = pConvolutionNode->channels;
    const float* pIn     = ppFramesIn[0];
    float* pOut          = ppFramesOut[0];
    ma_uint32 iFrame;
    ma_uint32 iChannel;

    (void)pFrameCountIn;

    ma_mutex_lock(&pConvolutionNode->lock);

    if (pConvolutionNode->numPartitions == 0) {
        /* No impulse response loaded yet: dry passthrough. */
        ma_copy_pcm_frames(pOut, pIn, frameCount, ma_format_f32, channels);
        ma_mutex_unlock(&pConvolutionNode->lock);
        return;
    }

    for (iFrame = 0; iFrame < frameCount; iFrame += 1) {
        for (iChannel = 0; iChannel < channels; iChannel += 1) {
            float in  = pIn[iFrame * channels + iChannel];
            float wet = pConvolutionNode->ppOutputBuffer[iChannel][pConvolutionNode->inputPos];

            pConvolutionNode->ppInputBuffer[iChannel][pConvolutionNode->inputPos] = in;
            pOut[iFrame * channels + iChannel] = in * pConvolutionNode->dry + wet * pConvolutionNode->wet;
        }

        pConvolutionNode->inputPos += 1;
        if (pConvolutionNode->inputPos >= pConvolutionNode->partitionSize) {
            ma_convolution_node_process_partition(pConvolutionNode);
            pConvolutionNode->inputPos = 0;
        }
    }

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
    ma_uint32 fftSize;

    if (pConvolutionNode == NULL) {
        return MA_INVALID_ARGS;
    }

    memset(pConvolutionNode, 0, sizeof(*pConvolutionNode));

    if (pConfig == NULL || pConfig->channels == 0) {
        return MA_INVALID_ARGS;
    }

    partitionSize = (pConfig->partitionSize == 0) ? 1024 : pConfig->partitionSize;
    if (!ma_convolution_node_is_pow2(partitionSize)) {
        return MA_INVALID_ARGS;
    }
    fftSize = partitionSize * 2;

    result = ma_mutex_init(&pConvolutionNode->lock);
    if (result != MA_SUCCESS) {
        return result;
    }

    pConvolutionNode->channels      = pConfig->channels;
    pConvolutionNode->partitionSize = partitionSize;
    pConvolutionNode->fftSize       = fftSize;
    pConvolutionNode->wet           = 1.0f;
    pConvolutionNode->dry           = 0.0f;

    pConvolutionNode->ppIrSpectrumRe = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->ppIrSpectrumIm = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->ppFdlRe        = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->ppFdlIm        = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->ppInputBuffer  = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->ppOutputBuffer = (float**)ma_malloc(sizeof(float*) * pConfig->channels, pAllocationCallbacks);
    pConvolutionNode->pScratchRe     = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);
    pConvolutionNode->pScratchIm     = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);
    pConvolutionNode->pAccumRe       = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);
    pConvolutionNode->pAccumIm       = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);

    if (pConvolutionNode->ppIrSpectrumRe == NULL || pConvolutionNode->ppIrSpectrumIm == NULL ||
        pConvolutionNode->ppFdlRe == NULL || pConvolutionNode->ppFdlIm == NULL ||
        pConvolutionNode->ppInputBuffer == NULL || pConvolutionNode->ppOutputBuffer == NULL ||
        pConvolutionNode->pScratchRe == NULL || pConvolutionNode->pScratchIm == NULL ||
        pConvolutionNode->pAccumRe == NULL || pConvolutionNode->pAccumIm == NULL) {
        ma_convolution_node_uninit(pConvolutionNode, pAllocationCallbacks);
        return MA_OUT_OF_MEMORY;
    }
    memset(pConvolutionNode->ppIrSpectrumRe, 0, sizeof(float*) * pConfig->channels);
    memset(pConvolutionNode->ppIrSpectrumIm, 0, sizeof(float*) * pConfig->channels);
    memset(pConvolutionNode->ppFdlRe, 0, sizeof(float*) * pConfig->channels);
    memset(pConvolutionNode->ppFdlIm, 0, sizeof(float*) * pConfig->channels);
    memset(pConvolutionNode->ppInputBuffer, 0, sizeof(float*) * pConfig->channels);
    memset(pConvolutionNode->ppOutputBuffer, 0, sizeof(float*) * pConfig->channels);

    baseConfig = pConfig->nodeConfig;
    baseConfig.vtable          = &g_ma_convolution_node_vtable;
    baseConfig.pInputChannels  = &pConfig->channels;
    baseConfig.pOutputChannels = &pConfig->channels;

    result = ma_node_init(pNodeGraph, &baseConfig, pAllocationCallbacks, &pConvolutionNode->baseNode);
    if (result != MA_SUCCESS) {
        ma_convolution_node_uninit(pConvolutionNode, pAllocationCallbacks);
        return result;
    }

    return MA_SUCCESS;
}

MA_API void ma_convolution_node_uninit(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    if (pConvolutionNode == NULL) {
        return;
    }

    /* The base node is always uninitialized first, so that nothing is still reading the buffers being freed below. */
    ma_node_uninit(&pConvolutionNode->baseNode, pAllocationCallbacks);

    if (pConvolutionNode->ppIrSpectrumRe != NULL) {
        ma_convolution_node_free_ir(pConvolutionNode, pAllocationCallbacks);
    }
    ma_free(pConvolutionNode->ppIrSpectrumRe, pAllocationCallbacks);
    ma_free(pConvolutionNode->ppIrSpectrumIm, pAllocationCallbacks);
    ma_free(pConvolutionNode->ppFdlRe, pAllocationCallbacks);
    ma_free(pConvolutionNode->ppFdlIm, pAllocationCallbacks);
    ma_free(pConvolutionNode->ppInputBuffer, pAllocationCallbacks);
    ma_free(pConvolutionNode->ppOutputBuffer, pAllocationCallbacks);
    ma_free(pConvolutionNode->pScratchRe, pAllocationCallbacks);
    ma_free(pConvolutionNode->pScratchIm, pAllocationCallbacks);
    ma_free(pConvolutionNode->pAccumRe, pAllocationCallbacks);
    ma_free(pConvolutionNode->pAccumIm, pAllocationCallbacks);

    ma_mutex_uninit(&pConvolutionNode->lock);
}

MA_API ma_result ma_convolution_node_set_ir(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, const ma_allocation_callbacks* pAllocationCallbacks)
{
    ma_uint32 partitionSize;
    ma_uint32 fftSize;
    ma_uint32 channels;
    ma_uint32 numPartitions;
    float** ppIrSpectrumRe  = NULL;
    float** ppIrSpectrumIm  = NULL;
    float** ppFdlRe         = NULL;
    float** ppFdlIm         = NULL;
    float** ppInputBuffer   = NULL;
    float** ppOutputBuffer  = NULL;
    float* pScratchRe       = NULL;
    float* pScratchIm       = NULL;
    ma_uint32 iChannel;
    ma_uint32 iPart;
    ma_uint32 j;
    ma_result result = MA_SUCCESS;

    if (pConvolutionNode == NULL || pFramesIn == NULL || irFrameCount == 0 || irChannels == 0) {
        return MA_INVALID_ARGS;
    }

    partitionSize  = pConvolutionNode->partitionSize;
    fftSize        = pConvolutionNode->fftSize;
    channels       = pConvolutionNode->channels;
    numPartitions  = (ma_uint32)((irFrameCount + partitionSize - 1) / partitionSize);

    ppIrSpectrumRe = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    ppIrSpectrumIm = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    ppFdlRe        = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    ppFdlIm        = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    ppInputBuffer  = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    ppOutputBuffer = (float**)ma_malloc(sizeof(float*) * channels, pAllocationCallbacks);
    pScratchRe     = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);
    pScratchIm     = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);

    if (ppIrSpectrumRe == NULL || ppIrSpectrumIm == NULL || ppFdlRe == NULL || ppFdlIm == NULL ||
        ppInputBuffer == NULL || ppOutputBuffer == NULL || pScratchRe == NULL || pScratchIm == NULL) {
        result = MA_OUT_OF_MEMORY;
        goto done;
    }
    memset(ppIrSpectrumRe, 0, sizeof(float*) * channels);
    memset(ppIrSpectrumIm, 0, sizeof(float*) * channels);
    memset(ppFdlRe, 0, sizeof(float*) * channels);
    memset(ppFdlIm, 0, sizeof(float*) * channels);
    memset(ppInputBuffer, 0, sizeof(float*) * channels);
    memset(ppOutputBuffer, 0, sizeof(float*) * channels);

    for (iChannel = 0; iChannel < channels; iChannel += 1) {
        ppIrSpectrumRe[iChannel] = (float*)ma_malloc(sizeof(float) * numPartitions * fftSize, pAllocationCallbacks);
        ppIrSpectrumIm[iChannel] = (float*)ma_malloc(sizeof(float) * numPartitions * fftSize, pAllocationCallbacks);
        ppFdlRe[iChannel]        = (float*)ma_malloc(sizeof(float) * numPartitions * fftSize, pAllocationCallbacks);
        ppFdlIm[iChannel]        = (float*)ma_malloc(sizeof(float) * numPartitions * fftSize, pAllocationCallbacks);
        ppInputBuffer[iChannel]  = (float*)ma_malloc(sizeof(float) * partitionSize, pAllocationCallbacks);
        ppOutputBuffer[iChannel] = (float*)ma_malloc(sizeof(float) * fftSize, pAllocationCallbacks);

        if (ppIrSpectrumRe[iChannel] == NULL || ppIrSpectrumIm[iChannel] == NULL ||
            ppFdlRe[iChannel] == NULL || ppFdlIm[iChannel] == NULL ||
            ppInputBuffer[iChannel] == NULL || ppOutputBuffer[iChannel] == NULL) {
            result = MA_OUT_OF_MEMORY;
            goto done;
        }

        memset(ppFdlRe[iChannel], 0, sizeof(float) * numPartitions * fftSize);
        memset(ppFdlIm[iChannel], 0, sizeof(float) * numPartitions * fftSize);
        memset(ppInputBuffer[iChannel], 0, sizeof(float) * partitionSize);
        memset(ppOutputBuffer[iChannel], 0, sizeof(float) * fftSize);

        /* A mono IR is broadcast to every node channel; otherwise the last available IR channel covers any node channel past the end of the IR. */
        {
            ma_uint32 srcChannel = (irChannels == 1) ? 0 : ((iChannel < irChannels) ? iChannel : (irChannels - 1));

            for (iPart = 0; iPart < numPartitions; iPart += 1) {
                ma_uint64 base = (ma_uint64)iPart * partitionSize;

                for (j = 0; j < partitionSize; j += 1) {
                    ma_uint64 srcFrame = base + j;
                    pScratchRe[j] = (srcFrame < irFrameCount) ? pFramesIn[srcFrame * irChannels + srcChannel] : 0.0f;
                    pScratchIm[j] = 0.0f;
                }
                for (j = partitionSize; j < fftSize; j += 1) {
                    pScratchRe[j] = 0.0f;
                    pScratchIm[j] = 0.0f;
                }

                ma_convolution_node_fft(pScratchRe, pScratchIm, fftSize, MA_FALSE);

                memcpy(ppIrSpectrumRe[iChannel] + (ma_uint64)iPart * fftSize, pScratchRe, sizeof(float) * fftSize);
                memcpy(ppIrSpectrumIm[iChannel] + (ma_uint64)iPart * fftSize, pScratchIm, sizeof(float) * fftSize);
            }
        }
    }

done:
    if (result != MA_SUCCESS) {
        if (ppIrSpectrumRe != NULL) { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppIrSpectrumRe[iChannel], pAllocationCallbacks); }
        if (ppIrSpectrumIm != NULL) { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppIrSpectrumIm[iChannel], pAllocationCallbacks); }
        if (ppFdlRe != NULL)        { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppFdlRe[iChannel], pAllocationCallbacks); }
        if (ppFdlIm != NULL)        { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppFdlIm[iChannel], pAllocationCallbacks); }
        if (ppInputBuffer != NULL)  { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppInputBuffer[iChannel], pAllocationCallbacks); }
        if (ppOutputBuffer != NULL) { for (iChannel = 0; iChannel < channels; iChannel += 1) ma_free(ppOutputBuffer[iChannel], pAllocationCallbacks); }
        ma_free(ppIrSpectrumRe, pAllocationCallbacks);
        ma_free(ppIrSpectrumIm, pAllocationCallbacks);
        ma_free(ppFdlRe, pAllocationCallbacks);
        ma_free(ppFdlIm, pAllocationCallbacks);
        ma_free(ppInputBuffer, pAllocationCallbacks);
        ma_free(ppOutputBuffer, pAllocationCallbacks);
        ma_free(pScratchRe, pAllocationCallbacks);
        ma_free(pScratchIm, pAllocationCallbacks);
        return result;
    }

    /* The expensive part is done. Swap the new impulse response in under the same lock the audio thread takes, which only ever has to hold it long enough to update a handful of pointers. */
    ma_mutex_lock(&pConvolutionNode->lock);
    ma_convolution_node_free_ir(pConvolutionNode, pAllocationCallbacks);
    memcpy(pConvolutionNode->ppIrSpectrumRe, ppIrSpectrumRe, sizeof(float*) * channels);
    memcpy(pConvolutionNode->ppIrSpectrumIm, ppIrSpectrumIm, sizeof(float*) * channels);
    memcpy(pConvolutionNode->ppFdlRe, ppFdlRe, sizeof(float*) * channels);
    memcpy(pConvolutionNode->ppFdlIm, ppFdlIm, sizeof(float*) * channels);
    memcpy(pConvolutionNode->ppInputBuffer, ppInputBuffer, sizeof(float*) * channels);
    memcpy(pConvolutionNode->ppOutputBuffer, ppOutputBuffer, sizeof(float*) * channels);
    pConvolutionNode->numPartitions    = numPartitions;
    pConvolutionNode->irLengthInFrames = irFrameCount;
    pConvolutionNode->inputPos         = 0;
    pConvolutionNode->fdlPos           = 0;
    ma_mutex_unlock(&pConvolutionNode->lock);

    ma_free(ppIrSpectrumRe, pAllocationCallbacks);
    ma_free(ppIrSpectrumIm, pAllocationCallbacks);
    ma_free(ppFdlRe, pAllocationCallbacks);
    ma_free(ppFdlIm, pAllocationCallbacks);
    ma_free(ppInputBuffer, pAllocationCallbacks);
    ma_free(ppOutputBuffer, pAllocationCallbacks);
    ma_free(pScratchRe, pAllocationCallbacks);
    ma_free(pScratchIm, pAllocationCallbacks);

    return MA_SUCCESS;
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
