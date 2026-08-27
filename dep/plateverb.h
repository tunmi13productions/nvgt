/* Plate reverb library
* Plateverb version 1.0
*
* An implementation of the plate reverberator described in Jon Dattorro, "Effect
* Design, Part 1: Reverberator and Other Filters", J. Audio Eng. Soc. vol. 45 no. 9,
* September 1997. The delay and tap lengths in that paper are given for a sample rate
* of 29761 HZ and are scaled to the requested rate here.
*
* IMPORTANT: The reverb works with 1 or 2 channels, at sample rates of 22050 HZ and above.
* The tank is mono in and stereo out, so a stereo input is summed before it enters.
*
* USAGE
*
* This is a single-file library. To use it, do something like the following in one .c file.
* #define PLATEVERB_IMPLEMENTATION
* #include "plateverb.h"
*
* You can then #include this file in other parts of the program as you would with any other header file.
*
* The library never allocates. Ask plateverb_get_required_memory how many bytes a given
* sample rate needs, hand a block of at least that size to plateverb_initialize, and keep
* that block alive for as long as the plateverb structure is in use.
*/

#ifndef PLATEVERB_H
#define PLATEVERB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* COMPILE-TIME OPTIONS */

    /* The longest predelay that can be dialed in, in seconds. Governs how much memory the predelay line takes. */
#ifndef plateverb_max_predelay
#define plateverb_max_predelay 0.25f
#endif

    /* The largest value the size control accepts. Governs how much memory the tank takes. */
#ifndef plateverb_max_size
#define plateverb_max_size 2.0f
#endif

    /* The silence threshold which is used when calculating decay time. */
#ifndef plateverb_silence_threshold
#define plateverb_silence_threshold 80.0 /* In dB (absolute). */
#endif

    /* PUBLIC API */

    typedef struct plateverb plateverb;

    /* Return the number of bytes plateverb_initialize needs for the given sample rate, or 0 if the rate is unsupported. */
    size_t plateverb_get_required_memory ( unsigned long sample_rate );

    /* Initialize a plateverb structure.
    *
    * memory must point at a block of at least plateverb_get_required_memory bytes, and must stay
    * alive and untouched for as long as the plateverb structure is used.
    * Returns nonzero (true) on success or 0 (false) on failure.
    * The function will only fail if one or more of the parameters are invalid.
    */
    int plateverb_initialize ( plateverb* verb, unsigned long sample_rate, unsigned int channels, void* memory );

    /* Run the reverb.
    *
    * Call this function continuously to generate your output.
    * output_buffer may be the same pointer as input_buffer if in place processing is desired.
    * frames specifies the number of sample frames that should be processed.
    */
    void plateverb_process ( plateverb* verb, const float* input_buffer, float* output_buffer, unsigned long frames );

    /* Set the delay before the reverb starts, in seconds, between 0.0 and plateverb_max_predelay.
    * The line cannot deliver less than a single sample, so 0.0 means one sample.
    */
    void plateverb_set_predelay ( plateverb* verb, float value );

    /* Get the predelay in seconds. */
    float plateverb_get_predelay ( const plateverb* verb );

    /* Set how much high frequency content is allowed into the tank, between 0.0 and 1.0, where 1.0 is the full spectrum. */
    void plateverb_set_bandwidth ( plateverb* verb, float value );

    /* Get the input bandwidth. */
    float plateverb_get_bandwidth ( const plateverb* verb );

    /* Set how long the tail lasts, between 0.0 and 1.0. */
    void plateverb_set_decay ( plateverb* verb, float value );

    /* Get the decay. */
    float plateverb_get_decay ( const plateverb* verb );

    /* Set how quickly high frequencies are lost as the tail decays, between 0.0 and 1.0, where 0.0 keeps the tail bright. */
    void plateverb_set_damping ( plateverb* verb, float value );

    /* Get the damping. */
    float plateverb_get_damping ( const plateverb* verb );

    /* Set a scale factor applied to every delay in the tank, between 0.1 and plateverb_max_size.
    * Larger values give a bigger, slower sounding space.
    * Changing this while audio is running retunes the whole tank at once and can be heard as a click,
    * so treat it as a setting rather than something to sweep.
    */
    void plateverb_set_size ( plateverb* verb, float value );

    /* Get the size. */
    float plateverb_get_size ( const plateverb* verb );

    /* Set how much the first pair of input allpasses smear the input, between 0.0 and 0.99. */
    void plateverb_set_input_diffusion_1 ( plateverb* verb, float value );

    /* Get the first input diffusion amount. */
    float plateverb_get_input_diffusion_1 ( const plateverb* verb );

    /* Set how much the second pair of input allpasses smear the input, between 0.0 and 0.99. */
    void plateverb_set_input_diffusion_2 ( plateverb* verb, float value );

    /* Get the second input diffusion amount. */
    float plateverb_get_input_diffusion_2 ( const plateverb* verb );

    /* Set how much the modulated tank allpasses smear the tail, between 0.0 and 0.79.
    * Values are limited because the tank rings audibly as this approaches 1.0.
    */
    void plateverb_set_decay_diffusion_1 ( plateverb* verb, float value );

    /* Get the first decay diffusion amount. */
    float plateverb_get_decay_diffusion_1 ( const plateverb* verb );

    /* Set how much the second pair of tank allpasses smear the tail, between 0.0 and 0.99. */
    void plateverb_set_decay_diffusion_2 ( plateverb* verb, float value );

    /* Get the second decay diffusion amount. */
    float plateverb_get_decay_diffusion_2 ( const plateverb* verb );

    /* Set how far the modulated tank allpasses wander, between 0.0 and 1.0.
    * This is what keeps the tail from settling into a fixed metallic ring. 0.0 disables modulation.
    */
    void plateverb_set_modulation_depth ( plateverb* verb, float value );

    /* Get the modulation depth. */
    float plateverb_get_modulation_depth ( const plateverb* verb );

    /* Set how quickly the modulation wanders, in HZ. Values much above a few HZ become audible as pitch movement. */
    void plateverb_set_modulation_rate ( plateverb* verb, float value );

    /* Get the modulation rate in HZ. */
    float plateverb_get_modulation_rate ( const plateverb* verb );

    /* Set the volume of the wet signal, between 0.0 and 1.0. */
    void plateverb_set_wet ( plateverb* verb, float value );

    /* Get the volume of the wet signal. */
    float plateverb_get_wet ( const plateverb* verb );

    /* Set the volume of the dry signal, between 0.0 and 1.0. */
    void plateverb_set_dry ( plateverb* verb, float value );

    /* Get the volume of the dry signal. */
    float plateverb_get_dry ( const plateverb* verb );

    /* Set the stereo width of the reverb, between 0.0 and 1.0. */
    void plateverb_set_width ( plateverb* verb, float value );

    /* Get the stereo width of the reverb. */
    float plateverb_get_width ( const plateverb* verb );

    /* Set the mode of the reverb, where values below 0.5 mean normal and values above mean frozen.
    * Freezing holds the tank at unity gain and shuts the input, so whatever is in the tail sustains.
    */
    void plateverb_set_mode ( plateverb* verb, float value );

    /* Get the mode of the reverb. */
    float plateverb_get_mode ( const plateverb* verb );

    /* Get the decay time in sample frames based on the current decay and size settings. */
    /* If freeze mode is active, the decay time is infinite and this function returns 0. */
    unsigned long plateverb_get_decay_time_in_frames ( const plateverb* verb );

    /* INTERNAL STRUCTURES */

    /* A circular buffer. Writes land at index, reads count backwards from it, so the effective
    * length can change at any time without disturbing the write position.
    */
    typedef struct plateverb_line plateverb_line;
    struct plateverb_line
    {
        float* buffer;
        int capacity;
        int max_length;
        int length;
        int index;
    };

    /* Reverb model tuning values */
#define plateverb_num_input_diffusers 4
#define plateverb_num_tank_lines 8
#define plateverb_num_taps 7
#define plateverb_first_tank_line ( 1 + plateverb_num_input_diffusers )
#define plateverb_num_lines ( 1 + plateverb_num_input_diffusers + plateverb_num_tank_lines )
#define plateverb_line_guard 4
#define plateverb_base_sample_rate 29761.0
#define plateverb_min_sample_rate 22050
#define plateverb_max_excursion 32.0f /* In samples at the base rate. */
#define plateverb_scale_damping 0.9f
#define plateverb_output_gain 0.6f
/* Trims the tank so that a default plate sits at the same level as a default freeverb, which keeps
* the send volumes in reverb3d meaning the same thing whichever reverb is hung off it.
*/
#define plateverb_input_gain 0.625f
#define plateverb_freezemode 0.5f
#define plateverb_initialpredelay 0.0f
#define plateverb_initialbandwidth 0.9995f
#define plateverb_initialdecay 0.5f
#define plateverb_initialdamping 0.25f
#define plateverb_initialsize 1.0f
#define plateverb_initialinputdiffusion1 0.75f
#define plateverb_initialinputdiffusion2 0.625f
#define plateverb_initialdecaydiffusion1 0.7f
#define plateverb_initialdecaydiffusion2 0.5f
#define plateverb_initialmodulationdepth 0.25f
#define plateverb_initialmodulationrate 1.0f
#define plateverb_initialwet 1.0f
#define plateverb_initialdry 0.0f
#define plateverb_initialwidth 1.0f
#define plateverb_initialmode 0.0f

    /* The main reverb structure. This is the structure that you will create an instance of when using the reverb. */
    struct plateverb
    {
        unsigned int channels;
        unsigned long sample_rate;
        float rate_scale; /* sample_rate divided by plateverb_base_sample_rate. */

        /* Values as the caller set them. */
        float predelay, bandwidth, decay, damping, size;
        float input_diffusion_1, input_diffusion_2;
        float decay_diffusion_1, decay_diffusion_2;
        float modulation_depth, modulation_rate;
        float wet, dry, width, mode;

        /* Values derived from the above, recalculated whenever a parameter changes. */
        float wet1, wet2;
        float input_gain;
        float decay_gain;
        float damping_coefficient;
        float bandwidth_coefficient;
        float predelay_offset; /* In samples. */
        float excursion; /* In samples. */
        int tap_offset_left[plateverb_num_taps];
        int tap_offset_right[plateverb_num_taps];

        /* A pair of quadrature oscillators, one per tank half, kept at slightly different rates. */
        float lfo1_cos, lfo1_sin, lfo1_step_cos, lfo1_step_sin;
        float lfo2_cos, lfo2_sin, lfo2_step_cos, lfo2_step_sin;

        /* Filter and feedback state. */
        float bandwidth_state;
        float damping_state_left, damping_state_right;
        float feedback_left, feedback_right;

        plateverb_line predelay_line;
        plateverb_line diffuser[plateverb_num_input_diffusers];
        plateverb_line tank[plateverb_num_tank_lines];
    };

#ifdef __cplusplus
}
#endif

#endif  /* PLATEVERB_H */

/* IMPLEMENTATION */

#ifdef PLATEVERB_IMPLEMENTATION

#include <math.h>
#include <string.h>

#ifdef _MSC_VER
#define PLATEVERB_INLINE __forceinline
#else
#ifdef __GNUC__
#define PLATEVERB_INLINE inline __attribute__((always_inline))
#else
#define PLATEVERB_INLINE inline
#endif
#endif

#define plateverb_pi 3.14159265358979323846

#define plateverb_undenormalise(sample) do { (sample) += 1.0f; (sample) -= 1.0f; } while ( 0 )

/* An interpolated read looks at the sample before the write position and the one after it, so it has
* to stay two samples clear of the write position for both of them to be sane.
*/
#define plateverb_min_offset 2.0f

static PLATEVERB_INLINE float plateverb_clamp ( float value, float low, float high )
{
    if ( value < low )
    {
        return low;
    }
    if ( value > high )
    {
        return high;
    }
    return value;
}

/* Lengths at plateverb_base_sample_rate, from figure 4 of the paper. */
static const int plateverb_input_lengths[plateverb_num_input_diffusers] = { 142, 107, 379, 277 };

/* Per tank half: the modulated allpass, the first delay, the second allpass, the second delay. */
static const int plateverb_tank_lengths[plateverb_num_tank_lines] = { 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };

typedef struct plateverb_tap plateverb_tap;
struct plateverb_tap
{
    int line;   /* Index into plateverb.tank. */
    int offset; /* In samples at the base rate. */
    float gain;
};

/* The output taps from table 2 of the paper. Each ear reads mostly from the far half of the
* tank, which is what gives the plate its stereo image without any panning.
*/
static const plateverb_tap plateverb_taps_left[plateverb_num_taps] =
{
    { 5,  266,  1.0f }, { 5, 2974,  1.0f }, { 6, 1913, -1.0f }, { 7, 1996,  1.0f },
    { 1, 1990, -1.0f }, { 2,  187, -1.0f }, { 3, 1066, -1.0f }
};
static const plateverb_tap plateverb_taps_right[plateverb_num_taps] =
{
    { 1,  353,  1.0f }, { 1, 3627,  1.0f }, { 2, 1228, -1.0f }, { 3, 2673,  1.0f },
    { 5, 2111, -1.0f }, { 6,  335, -1.0f }, { 7,  121, -1.0f }
};

static PLATEVERB_INLINE float plateverb_line_read ( const plateverb_line* line, int offset )
{
    int index = line->index - offset;
    if ( index < 0 )
    {
        index += line->capacity;
    }
    return line->buffer[index];
}

static PLATEVERB_INLINE float plateverb_line_read_fractional ( const plateverb_line* line, float offset )
{
    float position;
    float fraction;
    int index, next;

    position = ( float ) line->index - offset;
    while ( position < 0.0f )
    {
        position += ( float ) line->capacity;
    }
    index = ( int ) position;
    fraction = position - ( float ) index;
    next = index + 1;
    if ( next >= line->capacity )
    {
        next -= line->capacity;
    }
    return line->buffer[index] + fraction * ( line->buffer[next] - line->buffer[index] );
}

static PLATEVERB_INLINE void plateverb_line_write ( plateverb_line* line, float value )
{
    line->buffer[line->index] = value;
    if ( ++line->index >= line->capacity )
    {
        line->index = 0;
    }
}

/* A Schroeder allpass, y[n] = -g*x[n] + x[n-M] + g*y[n-M], realized through a single delay line. */
static PLATEVERB_INLINE float plateverb_allpass ( plateverb_line* line, float input, float gain )
{
    float delayed = plateverb_line_read ( line, line->length );
    float stored;
    plateverb_undenormalise ( delayed );
    stored = input + gain * delayed;
    plateverb_line_write ( line, stored );
    return delayed - gain * stored;
}

/* The same filter with a wandering read position, which is what stops the tail ringing on fixed pitches. */
static PLATEVERB_INLINE float plateverb_allpass_modulated ( plateverb_line* line, float input, float gain, float excursion )
{
    float offset = ( float ) line->length + excursion;
    float delayed;
    float stored;

    if ( offset < plateverb_min_offset )
    {
        offset = plateverb_min_offset;
    }
    delayed = plateverb_line_read_fractional ( line, offset );
    plateverb_undenormalise ( delayed );
    stored = input + gain * delayed;
    plateverb_line_write ( line, stored );
    return delayed - gain * stored;
}

static PLATEVERB_INLINE float plateverb_delay ( plateverb_line* line, float input )
{
    float output = plateverb_line_read ( line, line->length );
    plateverb_undenormalise ( output );
    plateverb_line_write ( line, input );
    return output;
}

static PLATEVERB_INLINE float plateverb_delay_fractional ( plateverb_line* line, float input, float offset )
{
    float output = plateverb_line_read_fractional ( line, offset );
    plateverb_undenormalise ( output );
    plateverb_line_write ( line, input );
    return output;
}

static PLATEVERB_INLINE float plateverb_onepole ( float* state, float input, float coefficient )
{
    *state += coefficient * ( input - *state );
    plateverb_undenormalise ( *state );
    return *state;
}

/* Pulls the oscillator back onto the unit circle. One Newton step is plenty when it only drifts by rounding. */
static PLATEVERB_INLINE void plateverb_renormalize ( float* cosine, float* sine )
{
    float correction = ( 3.0f - ( *cosine * *cosine + *sine * *sine ) ) * 0.5f;
    *cosine *= correction;
    *sine *= correction;
}

static void plateverb_compute_layout ( unsigned long sample_rate, int* max_lengths, int* capacities )
{
    int i;
    int excursion_room;
    double rate_scale = ( double ) sample_rate / plateverb_base_sample_rate;

    excursion_room = ( int ) ( ( double ) plateverb_max_excursion * rate_scale ) + 2;

    max_lengths[0] = ( int ) ( ( double ) plateverb_max_predelay * ( double ) sample_rate ) + 2;
    for ( i = 0; i < plateverb_num_input_diffusers; i++ )
    {
        max_lengths[1 + i] = ( int ) ( ( double ) plateverb_input_lengths[i] * rate_scale + 0.5 );
    }
    for ( i = 0; i < plateverb_num_tank_lines; i++ )
    {
        max_lengths[plateverb_first_tank_line + i] = ( int ) ( ( double ) plateverb_tank_lengths[i] * rate_scale * ( double ) plateverb_max_size + 0.5 );
    }

    for ( i = 0; i < plateverb_num_lines; i++ )
    {
        if ( max_lengths[i] < 1 )
        {
            max_lengths[i] = 1;
        }
        capacities[i] = max_lengths[i] + plateverb_line_guard;
    }

    /* The two modulated allpasses read past their nominal length, so they need room for the excursion. */
    capacities[plateverb_first_tank_line + 0] += excursion_room;
    capacities[plateverb_first_tank_line + 4] += excursion_room;
}

static int plateverb_scaled_tap ( const plateverb* verb, const plateverb_tap* tap, float scale )
{
    int length = verb->tank[tap->line].length;
    int offset = ( int ) ( ( float ) tap->offset * scale + 0.5f );

    if ( offset >= length )
    {
        offset = length - 1;
    }
    if ( offset < 1 )
    {
        offset = 1;
    }
    return offset;
}

static void plateverb_update ( plateverb* verb )
{
    /* Recalculate internal values after parameter change. */

    int i;
    float scale = verb->rate_scale * verb->size;

    verb->wet1 = verb->wet * ( verb->width / 2.0f + 0.5f );
    verb->wet2 = verb->wet * ( ( 1.0f - verb->width ) / 2.0f );

    if ( verb->mode >= plateverb_freezemode )
    {
        verb->decay_gain = 1.0f;
        verb->damping_coefficient = 1.0f;
        verb->bandwidth_coefficient = 1.0f;
        verb->input_gain = 0.0f;
    }
    else
    {
        verb->decay_gain = verb->decay;
        verb->damping_coefficient = 1.0f - verb->damping * plateverb_scale_damping;
        verb->bandwidth_coefficient = verb->bandwidth;
        verb->input_gain = plateverb_input_gain;
    }

    for ( i = 0; i < plateverb_num_tank_lines; i++ )
    {
        int length = ( int ) ( ( float ) plateverb_tank_lengths[i] * scale + 0.5f );
        if ( length < 1 )
        {
            length = 1;
        }
        if ( length > verb->tank[i].max_length )
        {
            length = verb->tank[i].max_length;
        }
        verb->tank[i].length = length;
    }

    for ( i = 0; i < plateverb_num_taps; i++ )
    {
        verb->tap_offset_left[i] = plateverb_scaled_tap ( verb, &plateverb_taps_left[i], scale );
        verb->tap_offset_right[i] = plateverb_scaled_tap ( verb, &plateverb_taps_right[i], scale );
    }

    verb->excursion = verb->modulation_depth * plateverb_max_excursion * verb->rate_scale;

    verb->predelay_offset = verb->predelay * ( float ) verb->sample_rate;
    if ( verb->predelay_offset < plateverb_min_offset )
    {
        verb->predelay_offset = plateverb_min_offset;
    }
    if ( verb->predelay_offset > ( float ) verb->predelay_line.max_length )
    {
        verb->predelay_offset = ( float ) verb->predelay_line.max_length;
    }
}

static void plateverb_update_modulation_rate ( plateverb* verb )
{
    double step = 2.0 * plateverb_pi * ( double ) verb->modulation_rate / ( double ) verb->sample_rate;

    verb->lfo1_step_cos = ( float ) cos ( step );
    verb->lfo1_step_sin = ( float ) sin ( step );

    /* The second half runs slightly faster so the two never settle into the same wobble. */
    step *= 1.13;
    verb->lfo2_step_cos = ( float ) cos ( step );
    verb->lfo2_step_sin = ( float ) sin ( step );
}

static void plateverb_mute ( plateverb* verb )
{
    int i;

    memset ( verb->predelay_line.buffer, 0, ( size_t ) verb->predelay_line.capacity * sizeof ( float ) );
    for ( i = 0; i < plateverb_num_input_diffusers; i++ )
    {
        memset ( verb->diffuser[i].buffer, 0, ( size_t ) verb->diffuser[i].capacity * sizeof ( float ) );
    }
    for ( i = 0; i < plateverb_num_tank_lines; i++ )
    {
        memset ( verb->tank[i].buffer, 0, ( size_t ) verb->tank[i].capacity * sizeof ( float ) );
    }

    verb->bandwidth_state = 0.0f;
    verb->damping_state_left = 0.0f;
    verb->damping_state_right = 0.0f;
    verb->feedback_left = 0.0f;
    verb->feedback_right = 0.0f;
}

size_t plateverb_get_required_memory ( unsigned long sample_rate )
{
    int max_lengths[plateverb_num_lines];
    int capacities[plateverb_num_lines];
    size_t total = 0;
    int i;

    if ( sample_rate < plateverb_min_sample_rate )
    {
        return 0;
    }

    plateverb_compute_layout ( sample_rate, max_lengths, capacities );
    for ( i = 0; i < plateverb_num_lines; i++ )
    {
        total += ( size_t ) capacities[i];
    }
    return total * sizeof ( float );
}

int plateverb_initialize ( plateverb* verb, unsigned long sample_rate, unsigned int channels, void* memory )
{
    int max_lengths[plateverb_num_lines];
    int capacities[plateverb_num_lines];
    plateverb_line* lines[plateverb_num_lines];
    float* cursor;
    int i;

    if ( verb == NULL || memory == NULL )
    {
        return 0;
    }
    if ( channels != 1 && channels != 2 )
    {
        return 0;    /* Currently supports only 1 or 2 channels. */
    }
    if ( sample_rate < plateverb_min_sample_rate )
    {
        return 0;    /* The minimum supported sample rate is 22050 HZ. */
    }

    memset ( verb, 0, sizeof ( *verb ) );
    verb->channels = channels;
    verb->sample_rate = sample_rate;
    verb->rate_scale = ( float ) ( ( double ) sample_rate / plateverb_base_sample_rate );

    plateverb_compute_layout ( sample_rate, max_lengths, capacities );

    lines[0] = &verb->predelay_line;
    for ( i = 0; i < plateverb_num_input_diffusers; i++ )
    {
        lines[1 + i] = &verb->diffuser[i];
    }
    for ( i = 0; i < plateverb_num_tank_lines; i++ )
    {
        lines[plateverb_first_tank_line + i] = &verb->tank[i];
    }

    /* Carve the caller's block into one circular buffer per line. */
    cursor = ( float* ) memory;
    for ( i = 0; i < plateverb_num_lines; i++ )
    {
        lines[i]->buffer = cursor;
        lines[i]->capacity = capacities[i];
        lines[i]->max_length = max_lengths[i];
        lines[i]->length = max_lengths[i];
        lines[i]->index = 0;
        cursor += capacities[i];
    }

    /* The input diffusers are not retuned by the size control, so their lengths are fixed here. */
    for ( i = 0; i < plateverb_num_input_diffusers; i++ )
    {
        verb->diffuser[i].length = max_lengths[1 + i];
    }

    verb->lfo1_cos = 1.0f;
    verb->lfo1_sin = 0.0f;
    verb->lfo2_cos = 0.0f;
    verb->lfo2_sin = 1.0f;

    /* Set default values. */
    verb->predelay = plateverb_initialpredelay;
    verb->bandwidth = plateverb_initialbandwidth;
    verb->decay = plateverb_initialdecay;
    verb->damping = plateverb_initialdamping;
    verb->size = plateverb_initialsize;
    verb->input_diffusion_1 = plateverb_initialinputdiffusion1;
    verb->input_diffusion_2 = plateverb_initialinputdiffusion2;
    verb->decay_diffusion_1 = plateverb_initialdecaydiffusion1;
    verb->decay_diffusion_2 = plateverb_initialdecaydiffusion2;
    verb->modulation_depth = plateverb_initialmodulationdepth;
    verb->modulation_rate = plateverb_initialmodulationrate;
    verb->wet = plateverb_initialwet;
    verb->dry = plateverb_initialdry;
    verb->width = plateverb_initialwidth;
    verb->mode = plateverb_initialmode;

    plateverb_update_modulation_rate ( verb );
    plateverb_update ( verb );

    /* The buffers will be full of rubbish - so we MUST mute them. */
    plateverb_mute ( verb );

    return 1;
}

void plateverb_process ( plateverb* verb, const float* input_buffer, float* output_buffer, unsigned long frames )
{
    int i;

    while ( frames-- > 0 )
    {
        float dry_left, dry_right;
        float input, left, right, wet_left, wet_right, rotated;

        if ( verb->channels == 1 )
        {
            dry_left = input_buffer[0];
            dry_right = dry_left;
            input = dry_left;
        }
        else
        {
            dry_left = input_buffer[0];
            dry_right = input_buffer[1];
            input = ( dry_left + dry_right ) * 0.5f;
        }

        input *= verb->input_gain;

        /* Predelay, then the input lowpass, then four allpasses that smear the input before it reaches the tank. */
        input = plateverb_delay_fractional ( &verb->predelay_line, input, verb->predelay_offset );
        input = plateverb_onepole ( &verb->bandwidth_state, input, verb->bandwidth_coefficient );
        input = plateverb_allpass ( &verb->diffuser[0], input, verb->input_diffusion_1 );
        input = plateverb_allpass ( &verb->diffuser[1], input, verb->input_diffusion_1 );
        input = plateverb_allpass ( &verb->diffuser[2], input, verb->input_diffusion_2 );
        input = plateverb_allpass ( &verb->diffuser[3], input, verb->input_diffusion_2 );

        /* The tank is a figure of eight: each half is fed by the input plus the other half's output.
        * Both halves read the feedback stored last frame so neither one leads the other.
        */
        left = input + verb->feedback_right;
        right = input + verb->feedback_left;

        left = plateverb_allpass_modulated ( &verb->tank[0], left, -verb->decay_diffusion_1, verb->excursion * verb->lfo1_sin );
        left = plateverb_delay ( &verb->tank[1], left );
        left = plateverb_onepole ( &verb->damping_state_left, left, verb->damping_coefficient );
        left *= verb->decay_gain;
        left = plateverb_allpass ( &verb->tank[2], left, verb->decay_diffusion_2 );
        left = plateverb_delay ( &verb->tank[3], left );

        right = plateverb_allpass_modulated ( &verb->tank[4], right, -verb->decay_diffusion_1, verb->excursion * verb->lfo2_sin );
        right = plateverb_delay ( &verb->tank[5], right );
        right = plateverb_onepole ( &verb->damping_state_right, right, verb->damping_coefficient );
        right *= verb->decay_gain;
        right = plateverb_allpass ( &verb->tank[6], right, verb->decay_diffusion_2 );
        right = plateverb_delay ( &verb->tank[7], right );

        verb->feedback_left = left * verb->decay_gain;
        verb->feedback_right = right * verb->decay_gain;

        wet_left = 0.0f;
        wet_right = 0.0f;
        for ( i = 0; i < plateverb_num_taps; i++ )
        {
            wet_left += plateverb_taps_left[i].gain * plateverb_line_read ( &verb->tank[plateverb_taps_left[i].line], verb->tap_offset_left[i] );
            wet_right += plateverb_taps_right[i].gain * plateverb_line_read ( &verb->tank[plateverb_taps_right[i].line], verb->tap_offset_right[i] );
        }
        wet_left *= plateverb_output_gain;
        wet_right *= plateverb_output_gain;

        /* Advance both oscillators by one sample through a rotation. */
        rotated = verb->lfo1_cos * verb->lfo1_step_cos - verb->lfo1_sin * verb->lfo1_step_sin;
        verb->lfo1_sin = verb->lfo1_cos * verb->lfo1_step_sin + verb->lfo1_sin * verb->lfo1_step_cos;
        verb->lfo1_cos = rotated;
        rotated = verb->lfo2_cos * verb->lfo2_step_cos - verb->lfo2_sin * verb->lfo2_step_sin;
        verb->lfo2_sin = verb->lfo2_cos * verb->lfo2_step_sin + verb->lfo2_sin * verb->lfo2_step_cos;
        verb->lfo2_cos = rotated;

        /* Calculate output REPLACING anything already there. */
        if ( verb->channels == 1 )
        {
            output_buffer[0] = ( wet_left + wet_right ) * 0.5f * verb->wet + dry_left * verb->dry;
            ++input_buffer;
            ++output_buffer;
        }
        else
        {
            output_buffer[0] = wet_left * verb->wet1 + wet_right * verb->wet2 + dry_left * verb->dry;
            output_buffer[1] = wet_right * verb->wet1 + wet_left * verb->wet2 + dry_right * verb->dry;
            input_buffer += 2;
            output_buffer += 2;
        }
    }

    plateverb_renormalize ( &verb->lfo1_cos, &verb->lfo1_sin );
    plateverb_renormalize ( &verb->lfo2_cos, &verb->lfo2_sin );
}

void plateverb_set_predelay ( plateverb* verb, float value )
{
    verb->predelay = plateverb_clamp ( value, 0.0f, plateverb_max_predelay );
    plateverb_update ( verb );
}

float plateverb_get_predelay ( const plateverb* verb )
{
    return verb->predelay;
}

void plateverb_set_bandwidth ( plateverb* verb, float value )
{
    verb->bandwidth = plateverb_clamp ( value, 0.0f, 1.0f );
    plateverb_update ( verb );
}

float plateverb_get_bandwidth ( const plateverb* verb )
{
    return verb->bandwidth;
}

void plateverb_set_decay ( plateverb* verb, float value )
{
    verb->decay = plateverb_clamp ( value, 0.0f, 0.999f );
    plateverb_update ( verb );
}

float plateverb_get_decay ( const plateverb* verb )
{
    return verb->decay;
}

void plateverb_set_damping ( plateverb* verb, float value )
{
    verb->damping = plateverb_clamp ( value, 0.0f, 1.0f );
    plateverb_update ( verb );
}

float plateverb_get_damping ( const plateverb* verb )
{
    return verb->damping;
}

void plateverb_set_size ( plateverb* verb, float value )
{
    verb->size = plateverb_clamp ( value, 0.1f, plateverb_max_size );
    plateverb_update ( verb );
}

float plateverb_get_size ( const plateverb* verb )
{
    return verb->size;
}

void plateverb_set_input_diffusion_1 ( plateverb* verb, float value )
{
    verb->input_diffusion_1 = plateverb_clamp ( value, 0.0f, 0.99f );
}

float plateverb_get_input_diffusion_1 ( const plateverb* verb )
{
    return verb->input_diffusion_1;
}

void plateverb_set_input_diffusion_2 ( plateverb* verb, float value )
{
    verb->input_diffusion_2 = plateverb_clamp ( value, 0.0f, 0.99f );
}

float plateverb_get_input_diffusion_2 ( const plateverb* verb )
{
    return verb->input_diffusion_2;
}

void plateverb_set_decay_diffusion_1 ( plateverb* verb, float value )
{
    verb->decay_diffusion_1 = plateverb_clamp ( value, 0.0f, 0.79f );
}

float plateverb_get_decay_diffusion_1 ( const plateverb* verb )
{
    return verb->decay_diffusion_1;
}

void plateverb_set_decay_diffusion_2 ( plateverb* verb, float value )
{
    verb->decay_diffusion_2 = plateverb_clamp ( value, 0.0f, 0.99f );
}

float plateverb_get_decay_diffusion_2 ( const plateverb* verb )
{
    return verb->decay_diffusion_2;
}

void plateverb_set_modulation_depth ( plateverb* verb, float value )
{
    verb->modulation_depth = plateverb_clamp ( value, 0.0f, 1.0f );
    plateverb_update ( verb );
}

float plateverb_get_modulation_depth ( const plateverb* verb )
{
    return verb->modulation_depth;
}

void plateverb_set_modulation_rate ( plateverb* verb, float value )
{
    verb->modulation_rate = plateverb_clamp ( value, 0.0f, 20.0f );
    plateverb_update_modulation_rate ( verb );
}

float plateverb_get_modulation_rate ( const plateverb* verb )
{
    return verb->modulation_rate;
}

void plateverb_set_wet ( plateverb* verb, float value )
{
    verb->wet = value;
    plateverb_update ( verb );
}

float plateverb_get_wet ( const plateverb* verb )
{
    return verb->wet;
}

void plateverb_set_dry ( plateverb* verb, float value )
{
    verb->dry = value;
}

float plateverb_get_dry ( const plateverb* verb )
{
    return verb->dry;
}

void plateverb_set_width ( plateverb* verb, float value )
{
    verb->width = plateverb_clamp ( value, 0.0f, 1.0f );
    plateverb_update ( verb );
}

float plateverb_get_width ( const plateverb* verb )
{
    return verb->width;
}

void plateverb_set_mode ( plateverb* verb, float value )
{
    verb->mode = value;
    plateverb_update ( verb );
}

float plateverb_get_mode ( const plateverb* verb )
{
    if ( verb->mode >= plateverb_freezemode )
    {
        return 1.0f;
    }
    return 0.0f;
}

unsigned long plateverb_get_decay_time_in_frames ( const plateverb* verb )
{
    double gain;
    double loops;
    double loop_frames;

    if ( verb->mode >= plateverb_freezemode )
    {
        return 0; /* Freeze mode creates an infinite decay. */
    }

    /* One trip around the figure of eight passes through the decay gain four times. */
    gain = ( double ) verb->decay;
    gain = gain * gain * gain * gain;
    if ( gain <= 0.0 || gain >= 1.0 )
    {
        return 0;
    }

    loops = plateverb_silence_threshold / fabs ( 20.0 * log10 ( gain ) );
    loop_frames = ( double ) ( verb->tank[1].length + verb->tank[3].length + verb->tank[5].length + verb->tank[7].length );
    return ( unsigned long ) ( loops * loop_frames );
}

#endif /* PLATEVERB_IMPLEMENTATION */

/* LICENSE

NVGT - NonVisual Gaming Toolkit (https://nvgt.dev)
Copyright (c) 2022-2025 Sam Tupy

This software is provided "as-is", without any express or implied warranty. In no event will
the authors be held liable for any damages arising from the use of this software.
Permission is granted to anyone to use this software for any purpose, including commercial
applications, and to alter it and redistribute it freely, subject to the following restrictions:
1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as
   being the original software.
3. This notice may not be removed or altered from any source distribution.
*/
