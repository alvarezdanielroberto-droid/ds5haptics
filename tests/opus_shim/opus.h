// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Declaraciones mínimas de libopus para compilar los tests en Linux sin las cabeceras
// de desarrollo (se enlaza contra libopus.so.0 del sistema). En Windows se usa la real.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error);
void opus_encoder_destroy(OpusEncoder* st);
int opus_encoder_ctl(OpusEncoder* st, int request, ...);
int opus_encode_float(OpusEncoder* st, const float* pcm, int frame_size, unsigned char* data, int max_data_bytes);
OpusDecoder* opus_decoder_create(int Fs, int channels, int* error);
int opus_decode_float(OpusDecoder* st, const unsigned char* data, int len, float* pcm, int frame_size, int decode_fec);
void opus_decoder_destroy(OpusDecoder* st);
#ifdef __cplusplus
}
#endif
#define OPUS_OK 0
#define OPUS_APPLICATION_RESTRICTED_LOWDELAY 2051
#define OPUS_SET_BITRATE(x) 4002, (int)(x)
#define OPUS_SET_VBR(x) 4006, (int)(x)
#define OPUS_SET_COMPLEXITY(x) 4010, (int)(x)
