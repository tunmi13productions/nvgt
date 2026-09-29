/* Include ma_convolution_node.h after miniaudio.h */
#ifndef miniaudio_convolution_node_h
#define miniaudio_convolution_node_h

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Uniformly partitioned overlap-save FFT convolution, for applying a recorded impulse response (a "space") to
audio in real time. The node has one input and one output, both at the configured channel count.

Unlike ma_plate_node and ma_reverb_node, the impulse response is not known at init time, so everything it
needs is built by ma_convolution_node_set_ir() into one self-contained state and swapped in whole, under a lock
shared with the audio thread, only once it is complete. set_ir() is therefore safe to call from any thread while
the node is attached to a running graph, and the audio thread never allocates or frees.

What a convolver costs is proportional to the response's length times the rate it runs at, so set_ir takes two
quality knobs:
    tailSeconds   the response is cut, with a short fade, at this length. 0 keeps the whole response.
    rateDivisor   the wet path runs at sampleRate / rateDivisor. The input is low-passed and decimated, the
                  response is low-passed forwards and back (keeping its timing) and decimated to match, and the
                  result is zero-stuffed back up and low-passed again. 2 is about half the work, and loses what is
                  above about 0.4 * sampleRate / rateDivisor. 1 does none of this.

The wet signal lags the input by exactly partitionSize frames regardless of rateDivisor.

Once the input has been silent for longer than the response takes to ring out, the node stops convolving until
the input comes back, so an idle convolver costs almost nothing.
*/
typedef struct
{
    ma_node_config nodeConfig;
    ma_uint32 channels;      /* The number of channels of the source, which will be the same as the output. */
    ma_uint32 sampleRate;
    ma_uint32 partitionSize; /* Frames per block at the full rate. Must be a power of two. 0 = default (512). */
} ma_convolution_node_config;

MA_API ma_convolution_node_config ma_convolution_node_config_init(ma_uint32 channels, ma_uint32 sampleRate);

typedef struct ma_convolution_ir_state ma_convolution_ir_state; /* Opaque, see ma_convolution_node.c. */

typedef struct
{
    ma_node_base baseNode;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint32 partitionSize;

    ma_mutex lock;                   /* Guards pState against a concurrent set_ir()/clear_ir(). */
    ma_convolution_ir_state* pState; /* NULL means no impulse response is loaded, so the node passes its input through untouched. */
    ma_uint64 irLengthInFrames;

    float wet;
    float dry;
} ma_convolution_node;

MA_API ma_result ma_convolution_node_init(ma_node_graph* pNodeGraph, const ma_convolution_node_config* pConfig, const ma_allocation_callbacks* pAllocationCallbacks, ma_convolution_node* pConvolutionNode);
MA_API void ma_convolution_node_uninit(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks);

/*
Loads an impulse response, replacing any previously loaded one. pFramesIn is interleaved float PCM at the node's
sample rate. irChannels need not match the node's channel count: a mono IR is broadcast to every node channel,
and if irChannels falls short of the node's channel count the last IR channel is reused for the remainder. See
the top of this file for tailSeconds and rateDivisor. Safe to call from any thread while the node is processing.
*/
MA_API ma_result ma_convolution_node_set_ir_ex(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, float tailSeconds, ma_uint32 rateDivisor, const ma_allocation_callbacks* pAllocationCallbacks);
/* The whole response at the full rate. */
MA_API ma_result ma_convolution_node_set_ir(ma_convolution_node* pConvolutionNode, const float* pFramesIn, ma_uint64 irFrameCount, ma_uint32 irChannels, const ma_allocation_callbacks* pAllocationCallbacks);

/* Returns the node to a passthrough state and frees the impulse response. */
MA_API void ma_convolution_node_clear_ir(ma_convolution_node* pConvolutionNode, const ma_allocation_callbacks* pAllocationCallbacks);

MA_API void ma_convolution_node_set_wet(ma_convolution_node* pConvolutionNode, float wet);
MA_API float ma_convolution_node_get_wet(const ma_convolution_node* pConvolutionNode);
MA_API void ma_convolution_node_set_dry(ma_convolution_node* pConvolutionNode, float dry);
MA_API float ma_convolution_node_get_dry(const ma_convolution_node* pConvolutionNode);
/* Length of the loaded response at the node's sample rate, after any tailSeconds cut. 0 if none is loaded. */
MA_API ma_uint64 ma_convolution_node_get_ir_length_in_frames(const ma_convolution_node* pConvolutionNode);
MA_API ma_uint32 ma_convolution_node_get_partition_size(const ma_convolution_node* pConvolutionNode);

#ifdef __cplusplus
}
#endif
#endif  /* miniaudio_convolution_node_h */
