/* Include ma_plate_node.h after miniaudio.h */
#ifndef miniaudio_plate_node_h
#define miniaudio_plate_node_h

#include "miniaudio.h"
#include "plateverb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
The plate node has one input and one output.
*/
typedef struct
{
    ma_node_config nodeConfig;
    ma_uint32 channels;         /* The number of channels of the source, which will be the same as the output. Must be 1 or 2. */
    ma_uint32 sampleRate;
} ma_plate_node_config;

MA_API ma_plate_node_config ma_plate_node_config_init(ma_uint32 channels, ma_uint32 sampleRate);


typedef struct
{
    ma_node_base baseNode;
    plateverb reverb;
    void* pHeap;    /* The delay lines, which plateverb carves up but never owns. */
} ma_plate_node;

MA_API ma_result ma_plate_node_init(ma_node_graph* pNodeGraph, const ma_plate_node_config* pConfig, const ma_allocation_callbacks* pAllocationCallbacks, ma_plate_node* pPlateNode);
MA_API void ma_plate_node_uninit(ma_plate_node* pPlateNode, const ma_allocation_callbacks* pAllocationCallbacks);

#ifdef __cplusplus
}
#endif
#endif  /* miniaudio_plate_node_h */
