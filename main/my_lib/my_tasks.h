#ifndef MAIN_MY_FUNCTIONS_MY_TASKS_H_
#define MAIN_MY_FUNCTIONS_MY_TASKS_H_

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Task to sample data from ADS8320 and trigger AutoEQ Algorithm
void vSample_task(void *args);

// Task to play WAV file through I2S for testing
void vPlay_WAV_task(void *args);

// Task to play audio data through USB
void vUSB_playback_task(void *args);

// Task to play audio data through Bluetooth
void vBT_playback_task(void *args);

#ifdef __cplusplus
}
#endif

#endif