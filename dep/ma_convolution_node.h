/* Include ma_convolution_node.h after miniaudio.h */
#ifndef miniaudio_convolution_node_h
#define miniaudio_convolution_node_h

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Uniformly partitioned FFT convolution, for applying a recorded impulse response (a "space") to
audio in real time. The node has one input and one output, both at the configured channel count.

Unlike ma_plate_node and ma_reverb_node, the impulse response is not known at init time, so its
buffers are allocated by ma_convolution_node_set_ir() rather than carved from one heap block up
front. set_ir() is safe to call while the node is attached to a running graph: it builds the new
partitioned spectrum in scratch memory first and only swaps it in, under a lock shared with the
audio thread, once it is complete.
*/
typedef struct
{
    ma_node_config nodeConfig;
    ma_uint32 channels;      /* The number of channels of the source, which will be the same as the output. */
    ma_uint32 sampleRate;
    ma_uint32 partitionSize; /* Frames per FFT partition. Must be a power of two. 0 = default (1024). */
} ma_convolution_node_config;

MA_API ma_convolution_node_config ma_convolution_node_config_init(ma_uint32 channels, ma_uint32 sampleRate);

typedef struct
{
    ma_node_base baseNode;
    ma_uint32 channels;
    ma_uint32 partitionSize;
    ma_uint32 fftSize;         /* partitionSize * 2 */

    ma_mutex lock;             /* Guards everything below against a concurrent ma_convolution_node_set_ir(). */
    ma_uint32 numPartitions;   /* 0 means no impulse response is loaded, so the node passes the dry signal through untouched. */
    ma_uint64 irLengthInFrames;
    float** ppIrSpectrumRe;    /* [channels][numPartitions * fftSize] */
    float** ppIrSpectrumIm;
    float** ppFdlRe;           /* Frequency delay line. [channels][numPartitions * fftSize] */
    float** ppFdlIm;
    float** ppInputBuffer;     /* [channels][partitionSize] */
    float** ppOutputBuffer;    /* [channels][fftSize], holding the pending overlap-add tail in its second half */
    ma_uint32 inputPos;
    ma_uint32 fdlPos;
    float* pScratchRe;         /* [fftSize], reused across channels since processing is serial */
    float* pScratchIm;
    float* pAccumRe;
    float* pAccumIm;

    float wet;
    float dry;
} ma_convolution_node;

MA_API ma_result ma_convolution_node_init(ma_node_graph* pNodeGraph, const ma_convolution_node_config* pConfig, const ma_allocation_callbacks* pAllocationCallbacks, ma_convolution_node* pConvolutionNode);
MA_API void ma_convolution_node_uninit(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks);

/*
Loads an impulse response, replacing any previously loaded one. pFramesIn is interleaved float
PCM. irChannels need not match the node's channel count: a mono IR is broadcast to every node
channel, and if irChannels falls short of the node's channel count the last IR channel is reused
for the remainder. Safe to call from any thread while the node is processing audio.
*/
MA_API ma_result ma_convolution_node_set_ir(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, const ma_allocation_callbacks* pAllocationCallbacks);

/* Returns the node to a dry passthrough state and frees the impulse response buffers. */
MA_API void ma_convolution_node_clear_ir(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks);

MA_API void ma_convolution_node_set_wet(ma_convolution_node* pConvolutionNode, float wet);
MA_API float ma_convolution_node_get_wet(const ma_convolution_node* pConvolutionNode);
MA_API void ma_convolution_node_set_dry(ma_convolution_node* pConvolutionNode, float dry);
MA_API float ma_convolution_node_get_dry(const ma_convolution_node* pConvolutionNode);
MA_API ma_uint64 ma_convolution_node_get_ir_length_in_frames(const ma_convolution_node* pConvolutionNode);
MA_API ma_uint32 ma_convolution_node_get_partition_size(const ma_convolution_node* pConvolutionNode);

#ifdef __cplusplus
}
#endif
#endif  /* miniaudio_convolution_node_h */
