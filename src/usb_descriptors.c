#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------
// Device Descriptor
//--------------------------------------------------------------------
const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,  // Espressif VID
    .idProduct          = 0x4002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

//--------------------------------------------------------------------
// Configuration Descriptor - UAC2 mono microphone, built manually
// so we can use synchronous EP (Windows-compatible)
//--------------------------------------------------------------------
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_TOTAL
};

#define EPNUM_AUDIO_IN    0x01
#define EP_SIZE           96

// AC descriptor body length (everything inside CS AC header)
#define AC_DESC_BODY_LEN  (TUD_AUDIO_DESC_CLK_SRC_LEN \
                         + TUD_AUDIO_DESC_INPUT_TERM_LEN \
                         + TUD_AUDIO_DESC_OUTPUT_TERM_LEN \
                         + TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN)

// Total audio function length (IAD + all interfaces)
#define AUDIO_FUNC_DESC_LEN (TUD_AUDIO_DESC_IAD_LEN \
                           + TUD_AUDIO_DESC_STD_AC_LEN \
                           + TUD_AUDIO_DESC_CS_AC_LEN \
                           + AC_DESC_BODY_LEN \
                           + TUD_AUDIO_DESC_STD_AS_INT_LEN \
                           + TUD_AUDIO_DESC_STD_AS_INT_LEN \
                           + TUD_AUDIO_DESC_CS_AS_INT_LEN \
                           + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN \
                           + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN \
                           + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + AUDIO_FUNC_DESC_LEN)

const uint8_t desc_configuration[] = {
    // Configuration descriptor
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // IAD
    TUD_AUDIO_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 0x02, 0x00),

    // Standard AC Interface
    TUD_AUDIO_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x00),

    // Class-Specific AC Interface Header (UAC2)
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_MICROPHONE, AC_DESC_BODY_LEN,
                         AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),

    // Clock Source (ID=0x04): internal fixed clock
    TUD_AUDIO_DESC_CLK_SRC(0x04, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                           (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS),
                           0x01, 0x00),

    // Input Terminal (ID=0x01): generic microphone
    TUD_AUDIO_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x03, 0x04,
                              0x01, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,
                              AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS, 0x00),

    // Output Terminal (ID=0x03): USB streaming
    TUD_AUDIO_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_USB_STREAMING, 0x01, 0x02,
                               0x04, 0x0000, 0x00),

    // Feature Unit (ID=0x02): mute + volume
    TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL(0x02, 0x01,
        AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS,
        AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS,
        0x00),

    // AS Interface Alt 0 (zero-bandwidth)
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(ITF_NUM_AUDIO_STREAMING), 0x00, 0x00, 0x00),

    // AS Interface Alt 1 (active, 1 EP)
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(ITF_NUM_AUDIO_STREAMING), 0x01, 0x01, 0x00),

    // CS AS Interface
    TUD_AUDIO_DESC_CS_AS_INT(0x03, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x01,
                             AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),

    // Type I Format: 2 bytes per sample, 16-bit resolution
    TUD_AUDIO_DESC_TYPE_I_FORMAT(2, 16),

    // Isochronous EP IN - SYNCHRONOUS (not async) for Windows compatibility
    TUD_AUDIO_DESC_STD_AS_ISO_EP(0x80 | EPNUM_AUDIO_IN,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS | TUSB_ISO_EP_ATT_DATA),
        EP_SIZE, 0x01),

    // CS AS ISO EP
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
                                AUDIO_CTRL_NONE,
                                AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED,
                                0x0000),
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
    "Configuration descriptor size mismatch");

//--------------------------------------------------------------------
// String Descriptors
//--------------------------------------------------------------------
const char *desc_strings[] = {
    (const char[]){0x09, 0x04},  // 0: English
    "BlazeEmbedded",             // 1: Manufacturer
    "USB Microphone",            // 2: Product
    "000001",                    // 3: Serial
};
const int desc_strings_count = sizeof(desc_strings) / sizeof(desc_strings[0]);
