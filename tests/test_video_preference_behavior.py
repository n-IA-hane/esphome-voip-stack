"""Execute the actual transport request method with controlled dialog state."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_video_preference_uses_existing_dialog_transaction(tmp_path):
    source = (ROOT / 'esphome/components/voip_stack/sip_transport.cpp').read_text()
    start = source.index('bool SipTransport::request_video_send(bool enabled)')
    end = source.index('\nbool SipTransport::apply_video_direction_answer_', start)
    method = source[start:end]
    probe = tmp_path / 'video_preference.cpp'
    probe.write_text(r'''
#include <atomic>
#include <cassert>
#include <string>
#define ESP_LOGW(...) ((void)0)
struct LockGuard { explicit LockGuard(int&) {} };
struct Pending { bool value=false; bool pending() const {return value;} };
class SipTransport {
public:
 int dialog_mutex_=0;
 std::string call_id_;
 void *video_source_=this;
 std::atomic<bool> running_{true}, transport_stopping_{false}, media_active_{false};
 std::atomic<bool> outgoing_invite_pending_{false}, video_send_requested_{true};
 bool video_negotiated_=false, video_send_enabled_=false, terminal=false;
 Pending pending_video_direction_invite_;
 struct {bool awaiting_ack=false;} completed_invite_;
 int changes=0, events=0;
 bool reported=false;
 bool terminal_transaction_pending_locked_() const {return terminal;}
 void emit_video_send_state_(bool enabled, bool pending) {
   assert(!pending); ++events; reported=enabled;
 }
 bool send_video_direction_reinvite_unlocked_(bool) {++changes; return true;}
 bool request_video_send(bool enabled);
};
''' + method + r'''
int main() {
 SipTransport idle;
 assert(idle.request_video_send(false));
 assert(!idle.video_send_requested_ && !idle.reported);
 assert(idle.changes==0 && idle.events==1);
 assert(idle.request_video_send(true));
 assert(idle.video_send_requested_ && idle.reported && idle.changes==0);
 for (int scenario=0; scenario<5; ++scenario) {
   SipTransport blocked;
   if (scenario==0) blocked.outgoing_invite_pending_=true;
   if (scenario==1) blocked.call_id_="pending-invite";
   if (scenario==2) blocked.terminal=true;
   if (scenario==3) blocked.transport_stopping_=true;
   if (scenario==4) blocked.video_source_=nullptr;
   assert(!blocked.request_video_send(false));
   assert(blocked.video_send_requested_ && blocked.events==0 && blocked.changes==0);
 }
 SipTransport call;
 call.call_id_="confirmed"; call.media_active_=true;
 call.video_negotiated_=true; call.video_send_enabled_=true;
 assert(call.request_video_send(false));
 assert(call.changes==1 && call.events==0);
 assert(call.video_send_requested_); // No premature confirmation.
 call.pending_video_direction_invite_.value=true;
 assert(!call.request_video_send(false));
 assert(call.changes==1);
}
''')
    binary = tmp_path / 'video_preference'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
