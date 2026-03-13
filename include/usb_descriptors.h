#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_configuration[];
extern const char *desc_strings[];
extern const int desc_strings_count;

// UAC2 entity IDs (matching TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR macro)
#define AUDIO_CTRL_ID_CLK_SRC          0x04
#define AUDIO_CTRL_ID_INPUT_TERMINAL   0x01
#define AUDIO_CTRL_ID_FEATURE_UNIT     0x02
#define AUDIO_CTRL_ID_OUTPUT_TERMINAL  0x03

#endif
