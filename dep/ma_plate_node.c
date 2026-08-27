#ifndef miniaudio_plate_node_c
#define miniaudio_plate_node_c

#define PLATEVERB_IMPLEMENTATION
#include "ma_plate_node.h"

#include <string.h> /* For memset(). */

MA_API ma_plate_node_config ma_plate_node_config_init(ma_uint32 channels, ma_uint32 sampleRate)
{
    ma_plate_node_config config;

    memset(&config, 0, sizeof(config));
    config.nodeConfig = ma_node_config_init();  /* Input and output channels will be set in ma_plate_node_init(). */
    config.channels   = channels;
    config.sampleRate = sampleRate;

    return config;
}


static void ma_plate_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    ma_plate_node* pPlateNode = (ma_plate_node*)pNode;

    (void)pFrameCountIn;

    plateverb_process(&pPlateNode->reverb, ppFramesIn[0], ppFramesOut[0], *pFrameCountOut);
}

static ma_node_vtable g_ma_plate_node_vtable =
{
    ma_plate_node_process_pcm_frames,
    NULL,
    1,  /* 1 input bus. */
    1,  /* 1 output bus. */
    MA_NODE_FLAG_CONTINUOUS_PROCESSING  /* Reverb requires continuous processing to ensure the tail get's processed. */
};

MA_API ma_result ma_plate_node_init(ma_node_graph* pNodeGraph, const ma_plate_node_config* pConfig, const ma_allocation_callbacks* pAllocationCallbacks, ma_plate_node* pPlateNode)
{
    ma_result result;
    ma_node_config baseConfig;
    size_t heapSize;

    if (pPlateNode == NULL) {
        return MA_INVALID_ARGS;
    }

    memset(pPlateNode, 0, sizeof(*pPlateNode));

    if (pConfig == NULL) {
        return MA_INVALID_ARGS;
    }

    heapSize = plateverb_get_required_memory(pConfig->sampleRate);
    if (heapSize == 0) {
        return MA_INVALID_ARGS;
    }

    pPlateNode->pHeap = ma_malloc(heapSize, pAllocationCallbacks);
    if (pPlateNode->pHeap == NULL) {
        return MA_OUT_OF_MEMORY;
    }

    if (plateverb_initialize(&pPlateNode->reverb, (unsigned long)pConfig->sampleRate, (unsigned int)pConfig->channels, pPlateNode->pHeap) == 0) {
        ma_free(pPlateNode->pHeap, pAllocationCallbacks);
        pPlateNode->pHeap = NULL;
        return MA_INVALID_ARGS;
    }

    baseConfig = pConfig->nodeConfig;
    baseConfig.vtable          = &g_ma_plate_node_vtable;
    baseConfig.pInputChannels  = &pConfig->channels;
    baseConfig.pOutputChannels = &pConfig->channels;

    result = ma_node_init(pNodeGraph, &baseConfig, pAllocationCallbacks, &pPlateNode->baseNode);
    if (result != MA_SUCCESS) {
        ma_free(pPlateNode->pHeap, pAllocationCallbacks);
        pPlateNode->pHeap = NULL;
        return result;
    }

    return MA_SUCCESS;
}

MA_API void ma_plate_node_uninit(ma_plate_node* pPlateNode, const ma_allocation_callbacks* pAllocationCallbacks)
{
    if (pPlateNode == NULL) {
        return;
    }

    /* The base node is always uninitialized first, so that nothing is still reading the delay lines. */
    ma_node_uninit(pPlateNode, pAllocationCallbacks);

    ma_free(pPlateNode->pHeap, pAllocationCallbacks);
    pPlateNode->pHeap = NULL;
}

#endif  /* miniaudio_plate_node_c */
