#pragma once
/* Fixed 48 kHz clock synchronized to SOF; host volume remains in software.
 * Poll every microframe at HS (six stereo samples). The pinned DWC2 driver
 * arms the next microframe parity, so an eight-microframe interval can miss
 * every host OUT packet after the initial activation. */
#define JETKVM_USB_AUDIO_DESCRIPTOR(itf, ep, hs) \
 TUD_AUDIO_DESC_IAD(itf, 2, 0), \
 TUD_AUDIO_DESC_STD_AC(itf, 0, 0), \
 TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER, 8+17+12, 0), \
 TUD_AUDIO_DESC_CLK_SRC(4, 5, AUDIO_CTRL_R, 1, 0), \
 TUD_AUDIO_DESC_INPUT_TERM(1, AUDIO_TERM_TYPE_USB_STREAMING, 0, 4, 2, 3, 0, 0, 0), \
 TUD_AUDIO_DESC_OUTPUT_TERM(3, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0, 1, 4, 0, 0), \
 TUD_AUDIO_DESC_STD_AS_INT((itf)+1, 0, 0, 0), \
 TUD_AUDIO_DESC_STD_AS_INT((itf)+1, 1, 1, 0), \
 TUD_AUDIO_DESC_CS_AS_INT(1, 0, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, 2, 3, 0), \
 TUD_AUDIO_DESC_TYPE_I_FORMAT(2, 16), \
 TUD_AUDIO_DESC_STD_AS_ISO_EP(ep, TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS, (hs) ? 28 : 196, 1), \
 TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, 0, 0, 0)
