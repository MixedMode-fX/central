#ifndef USB_AUDIO_HOST_UAC_H
#define USB_AUDIO_HOST_UAC_H

#include <stdint.h>

// The USB Audio Class constants this driver speaks: USB Device Class
// Definition for Audio Devices 1.0 (UAC1) and 2.0 (UAC2). Only what is used.
namespace usb_audio_host {
namespace uac {

// Standard descriptor types
constexpr uint8_t DT_INTERFACE = 0x04;
constexpr uint8_t DT_ENDPOINT = 0x05;
constexpr uint8_t DT_IAD = 0x0B;
constexpr uint8_t DT_CS_INTERFACE = 0x24;
constexpr uint8_t DT_CS_ENDPOINT = 0x25;

// Interface class and subclasses
constexpr uint8_t CLASS_AUDIO = 0x01;
constexpr uint8_t SUBCLASS_AUDIOCONTROL = 0x01;
constexpr uint8_t SUBCLASS_AUDIOSTREAMING = 0x02;
constexpr uint8_t SUBCLASS_MIDISTREAMING = 0x03;
constexpr uint8_t PROTOCOL_UAC2 = 0x20;

// AudioControl interface descriptor subtypes
constexpr uint8_t AC_HEADER = 0x01;
constexpr uint8_t AC_INPUT_TERMINAL = 0x02;
constexpr uint8_t AC_OUTPUT_TERMINAL = 0x03;
constexpr uint8_t AC_FEATURE_UNIT = 0x06;
constexpr uint8_t AC_CLOCK_SOURCE = 0x0A;     // UAC2
constexpr uint8_t AC_CLOCK_SELECTOR = 0x0B;   // UAC2
constexpr uint8_t AC_CLOCK_MULTIPLIER = 0x0C; // UAC2

// AudioStreaming interface descriptor subtypes
constexpr uint8_t AS_GENERAL = 0x01;
constexpr uint8_t AS_FORMAT_TYPE = 0x02;
constexpr uint8_t FORMAT_TYPE_I = 0x01;
constexpr uint16_t UAC1_FORMAT_PCM = 0x0001;
constexpr uint32_t UAC2_FORMAT_PCM = 0x00000001;

// Class-specific endpoint descriptor subtype
constexpr uint8_t EP_GENERAL = 0x01;
constexpr uint8_t UAC1_EP_SAMPLING_FREQ_CONTROL = 0x01; // bmAttributes bit

// Endpoint bmAttributes
constexpr uint8_t EP_TRANSFER_MASK = 0x03;
constexpr uint8_t EP_TRANSFER_ISOCHRONOUS = 0x01;
constexpr uint8_t EP_SYNC_MASK = 0x0C;
constexpr uint8_t EP_SYNC_NONE = 0x00;
constexpr uint8_t EP_SYNC_ASYNC = 0x04;
constexpr uint8_t EP_SYNC_ADAPTIVE = 0x08;
constexpr uint8_t EP_SYNC_SYNC = 0x0C;
constexpr uint8_t EP_USAGE_MASK = 0x30;
constexpr uint8_t EP_USAGE_DATA = 0x00;
constexpr uint8_t EP_USAGE_FEEDBACK = 0x10;
constexpr uint8_t EP_USAGE_IMPLICIT_FEEDBACK = 0x20;

// Standard requests
constexpr uint8_t REQ_SET_INTERFACE = 0x0B;

// Class requests, UAC1
constexpr uint8_t UAC1_SET_CUR = 0x01;
constexpr uint8_t UAC1_GET_CUR = 0x81;
constexpr uint8_t UAC1_SAMPLING_FREQ_CONTROL = 0x01;

// Class requests, UAC2
constexpr uint8_t UAC2_CUR = 0x01;
constexpr uint8_t UAC2_RANGE = 0x02;
constexpr uint8_t UAC2_CS_SAM_FREQ_CONTROL = 0x01;
constexpr uint8_t UAC2_CS_CLOCK_VALID_CONTROL = 0x02;
constexpr uint8_t UAC2_CX_CLOCK_SELECTOR_CONTROL = 0x01;

// bmRequestType
constexpr uint8_t RT_SET_INTERFACE = 0x01;       // host to device, standard, interface
constexpr uint8_t RT_SET_CLASS_INTERFACE = 0x21; // host to device, class, interface
constexpr uint8_t RT_GET_CLASS_INTERFACE = 0xA1; // device to host, class, interface
constexpr uint8_t RT_SET_CLASS_ENDPOINT = 0x22;  // host to device, class, endpoint
constexpr uint8_t RT_GET_CLASS_ENDPOINT = 0xA2;  // device to host, class, endpoint

// Clock source bmAttributes bits 1:0
constexpr uint8_t CLOCK_TYPE_MASK = 0x03;
constexpr uint8_t CLOCK_TYPE_EXTERNAL = 0x00;
// Clock source bmControls bits 1:0: 3 = sampling frequency host-programmable
constexpr uint8_t CLOCK_FREQ_CONTROL_MASK = 0x03;
constexpr uint8_t CLOCK_FREQ_PROGRAMMABLE = 0x03;

} // namespace uac
} // namespace usb_audio_host

#endif
