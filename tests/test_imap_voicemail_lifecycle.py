"""Execute production source-control methods with the public MediaSource ABI."""

from pathlib import Path
import subprocess
import sys

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/imap_voicemail"


@pytest.fixture(scope="module")
def lifecycle_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("imap-lifecycle")
    includes = folder / "stubs/esphome"
    (includes / "core").mkdir(parents=True)
    (includes / "components/audio").mkdir(parents=True)
    (includes / "core/defines.h").write_text("")
    (includes / "core/helpers.h").write_text("#pragma once\n")
    (includes / "components/audio/audio.h").write_text("""#pragma once
namespace esphome::audio {
struct AudioStreamInfo {};
enum class AudioFileType { NONE, WAV, MP3, OPUS, FLAC };
}
""")
    native = Path(sys.executable).resolve().parents[1] / "lib"
    candidates = list(native.glob("python*/site-packages/esphome/components/media_source/media_source.h"))
    if not candidates:
        import esphome
        candidates = [Path(esphome.__file__).parent / "components/media_source/media_source.h"]
    public_header = candidates[0].resolve()
    cpp = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
#include "esphome/components/imap_voicemail/imap_protocol.h"
#include "PUBLIC_MEDIA_SOURCE"
#define USE_AUDIO_WAV_SUPPORT
#define ESP_LOGW(...) ((void)0)
using namespace esphome;
using namespace esphome::imap_voicemail;
using media_source::MediaSourceState;
using media_source::MediaSourceCommand;
bool main_context=true;
unsigned notifications=0;
void xTaskNotifyGive(void *) { ++notifications; }
uint32_t millis() { return 100; }
template<typename... Ts> struct Trigger { unsigned calls=0; void trigger(const Ts &...) { assert(main_context); ++calls; } };
struct Callbacks {
 unsigned calls=0;
 void call() { assert(main_context); ++calls; }
};
struct Component {
 template<typename F> void set_timeout(const char *,uint32_t,F) { ++timers; }
 void cancel_timeout(const char *) {}
 void status_clear_warning() {}
 void status_set_warning() {}
 unsigned timers=0;
};
struct FakeStream { unsigned interrupts=0; void interrupt() { ++interrupts; } };
struct FakeVoip { bool idle=true; bool is_idle() const { return idle; } };
namespace esphome::imap_voicemail {
class ImapVoicemail : public Component, public media_source::MediaSource {
 public:
 struct Config { std::string host="mail.test", account_host, folder="INBOX"; uint16_t port=143; } config_;
 std::atomic<uint32_t> generation_{1}, play_generation_{1};
 std::atomic<bool> paused_{false}, intentional_wake_{false};
 bool ready_=true, enabled_=true, play_requested_=false, catalogue_ready_=false, no_idle_=false, playback_ready_=false;
 uint32_t pending_generation_=1, pending_playback_generation_=0, last_success_=0, refresh_ms_=300000, page_=0;
 uint8_t max_messages_=16;
 bool pending_error_retry_=true;
 std::string pending_error_;
 MediaSourceState pending_playback_state_=MediaSourceState::IDLE;
 Config play_config_;
 FakeVoip *parent_=nullptr;
 Message play_message_;
 FakeStream fetch_stream_, stream_;
 void *fetch_task_=reinterpret_cast<void *>(1);
 Mailbox mailbox_, pending_mailbox_;
 const char *status_="ready";
 Callbacks changed_;
 Trigger<uint32_t,uint32_t,uint32_t> updated_;
 Trigger<std::string> error_;
 struct Operation { unsigned kind; uint32_t uid,validity; };
 enum class OperationKind { SEEN, UNSEEN, DELETE };
 bool is_failed() const { return false; }
 void lock_() {}
 void unlock_() {}
 void notify_main_() { scheduled=true; }
 void disable_loop() { scheduled=false; }
 void refresh() { ++refreshes; }
 bool scheduled=false;
 unsigned refreshes=0;
 static audio::AudioFileType file_type_(const AudioPart &);
 bool can_play(uint32_t) const;
 bool can_handle(const std::string &) const override;
 bool play_uri(const std::string &) override;
 void handle_command(MediaSourceCommand) override;
 bool request_play_uid(uint32_t,uint32_t=0);
 void stop_playback_(bool=true);
 void publish_playback_(MediaSourceState,uint32_t);
 void loop();
 void set_page(uint32_t);
 HEADER_SET_STATE
 HEADER_SELECTED_UID
 HEADER_SELECTED_VALIDITY
 HEADER_HAS_OLDER
 HEADER_HAS_NEWER
 HEADER_OLDER
 HEADER_NEWER
};
}
METHODS
struct Listener : media_source::MediaSourceListener {
 ImapVoicemail *source;
 std::vector<MediaSourceState> states;
 std::string requested;
 size_t write_audio(const uint8_t *,size_t length,uint32_t,const audio::AudioStreamInfo &) override { return length; }
 void report_state(MediaSourceState state) override { assert(main_context); states.push_back(state); }
 void request_play_uri(const std::string &uri) override { assert(main_context); requested=uri; assert(source->play_uri(uri)); }
 void request_volume(float) override {}
 void request_mute(bool) override {}
};
struct DeferredListener : Listener {
 void request_play_uri(const std::string &uri) override { requested=uri; }
};
int main() {
 ImapVoicemail source;
 Listener listener;listener.source=&source;source.set_listener(&listener);
 Message first;first.uid=1;first.uid_validity=77;first.audio={"2","audio/wav","BASE64",100};
 Message second=first;second.uid=2;
 source.mailbox_.uid_validity=77;source.mailbox_.total=33;source.mailbox_.messages={first,second};
 // A request accepted while idle can reach the player after an incoming call.
 // The real source rechecks its bound telephone before starting that URI.
 FakeVoip phone;ImapVoicemail bound;bound.parent_=&phone;
 DeferredListener deferred;deferred.source=&bound;bound.set_listener(&deferred);
 bound.mailbox_.uid_validity=77;bound.mailbox_.messages={first};
 assert(bound.request_play_uid(1,77));assert(bound.get_state()==MediaSourceState::IDLE);
 phone.idle=false;assert(!bound.play_uri(deferred.requested));
 assert(!bound.request_play_uid(1,77));assert(bound.get_state()==MediaSourceState::IDLE);
 phone.idle=true;assert(bound.play_uri(deferred.requested));
 assert(bound.get_state()==MediaSourceState::PLAYING);bound.handle_command(MediaSourceCommand::STOP);bound.loop();
 // Independent credentials without a bound telephone retain ordinary playback.
 bound.parent_=nullptr;phone.idle=false;assert(bound.play_uri(deferred.requested));
 bound.handle_command(MediaSourceCommand::STOP);bound.loop();
 assert(source.request_play_uid(1,77));
 assert(source.get_state()==MediaSourceState::PLAYING && source.selected_uid()==1 && source.selected_uid_validity()==77);
 source.loop();assert(source.get_state()==MediaSourceState::PLAYING); // cancelled-old-file IDLE must not cancel this start
 const auto old_generation=source.play_generation_.load();
 source.handle_command(MediaSourceCommand::STOP);
 assert(source.request_play_uid(2,77));
 source.loop();assert(source.get_state()==MediaSourceState::PLAYING && source.selected_uid()==2);
 main_context=false;
 source.publish_playback_(MediaSourceState::ERROR,old_generation);
 main_context=true;source.loop();assert(source.get_state()==MediaSourceState::PLAYING);
 assert(!source.request_play_uid(1,76) && !source.play_uri(make_message_uri("mail.test",143,"INBOX",76,1)));
 source.handle_command(MediaSourceCommand::PAUSE);assert(source.get_state()==MediaSourceState::PAUSED);
 source.handle_command(MediaSourceCommand::PLAY);assert(source.get_state()==MediaSourceState::PLAYING);
 source.handle_command(MediaSourceCommand::STOP);
 const auto current=source.play_generation_.load();
 main_context=false;source.publish_playback_(MediaSourceState::ERROR,current-1);main_context=true;
 source.loop();assert(source.get_state()==MediaSourceState::IDLE);
 source.handle_command(MediaSourceCommand::PAUSE);assert(source.get_state()==MediaSourceState::IDLE);
 assert(!source.has_newer() && source.has_older());
 source.older();assert(source.page_==1);source.older();assert(source.page_==2 && !source.has_older());
 source.older();assert(source.page_==2);source.newer();source.newer();assert(source.page_==0 && !source.has_newer());
 assert(source.request_play_uid(1,77));
 source.pending_mailbox_.uid_validity=88;source.pending_mailbox_.total=0;source.catalogue_ready_=true;
 source.pending_generation_=source.generation_.load();source.loop();source.loop();
 assert(source.get_state()==MediaSourceState::IDLE && source.mailbox_.uid_validity==88);
 assert(!source.request_play_uid(1,77));
 // An old owner cannot publish catalogues or player state into a newer generation.
 source.pending_mailbox_.uid_validity=999;source.pending_generation_=0;source.catalogue_ready_=true;source.loop();
 assert(source.mailbox_.uid_validity==88);
 source.set_listener(nullptr);assert(!source.request_play_uid(1,88));
}
'''
    header = (SOURCE / "imap_voicemail.h").read_text()
    source = (SOURCE / "imap_voicemail.cpp").read_text()
    cpp = cpp.replace("PUBLIC_MEDIA_SOURCE", str(public_header))
    for key, marker in {
        "SET_STATE": "void set_state_(", "SELECTED_UID": "uint32_t selected_uid()",
        "SELECTED_VALIDITY": "uint32_t selected_uid_validity()", "HAS_OLDER": "bool has_older()",
        "HAS_NEWER": "bool has_newer()", "OLDER": "void older()", "NEWER": "void newer()",
    }.items():
        cpp = cpp.replace(f"HEADER_{key}", block(header, marker))
    methods = ["audio::AudioFileType ImapVoicemail::file_type_(", "bool ImapVoicemail::can_play(",
               "bool ImapVoicemail::can_handle(", "bool ImapVoicemail::play_uri(",
               "bool ImapVoicemail::request_play_uid(", "void ImapVoicemail::stop_playback_(",
               "void ImapVoicemail::publish_playback_(", "void ImapVoicemail::handle_command(",
               "void ImapVoicemail::loop()", "void ImapVoicemail::set_page("]
    cpp = cpp.replace("METHODS", "\n".join(block(source, marker) for marker in methods))
    path = folder / "lifecycle.cpp"
    path.write_text(cpp)
    binary = folder / "lifecycle"
    result = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-no-pie", "-DUSE_IMAP_VOICEMAIL_ACCOUNT",
                             "-I", str(folder / "stubs"), "-I", str(ROOT), str(path),
                             str(SOURCE / "imap_protocol.cpp"), str(ROOT / "esphome/components/phone_client/net_stream.cpp"),
                             "-o", str(binary)], capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary


def test_actual_media_source_cancellation_uid_generation_paging_and_main_loop_delivery(lifecycle_binary):
    result = subprocess.run([str(lifecycle_binary)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
