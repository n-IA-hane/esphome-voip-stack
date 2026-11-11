"""Compile the complete production TX overloads in existing host adapters."""

import re

from test_refer_dialog_runtime import SOURCE, block


def audio_tx_fixture(code):
    source = (SOURCE / "sip_transport.cpp").read_text()
    legacy = block(source, "void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes)")
    implementation = "\n".join((
        block(source, "TransportAudioTxContext SipTransport::audio_tx_frame_context("),
        legacy,
        block(source, "void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes,"),
    ))
    context = ("" if '#include "esphome/components/voip_stack/transport.h"' in code else
               block((SOURCE / "transport.h").read_text(), "struct TransportAudioTxContext {") + ";\n")
    owner = re.search(r"(?:struct|class) SipTransport\s*(?::[^{}]+)?\{", code)
    assert owner is not None
    additions = ("" if re.search(r"uint32_t\s+audio_tx_clock_rate_", code) else
                 "uint32_t audio_tx_clock_rate_=0,audio_tx_clock_generation_=0;\n") + """
 TransportAudioTxContext audio_tx_frame_context(const AudioFormat&);
 void send_audio_frame(const uint8_t*,size_t,const TransportAudioTxContext&);
"""
    code = code[:owner.start()] + context + code[owner.start():owner.end()] + additions + code[owner.end():]
    assert legacy in code
    return code.replace(legacy, implementation, 1)
