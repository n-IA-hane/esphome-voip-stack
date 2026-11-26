#include "phonebook.h"
#include "sip_account_view.h"
#include "voip_fsm.h"
#include <atomic>
#include <cassert>

#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)

namespace esphome::voip_stack {
// Scheduling/entity boundaries only. Selection, account scoping, directory
// snapshots and ordering below execute their actual production methods.
struct VoipStack {
  Phonebook phonebook_;
  SipAccountSnapshot known_account;
  bool get_account_snapshot(const std::string &line, SipAccountSnapshot *out) {
    if (line != known_account.config.id) return false;
    *out = known_account;
    return true;
  }
  std::atomic<CallState> call_state_{CallState::IDLE};
  std::string initial_contact_, selected_dial_target_, default_line_{"office"};
  bool selected_dial_target_resolved_{false}, initial_contact_ready_{false}, initial_contact_applied_{false};
  enum class TransportType { UDP, TCP }; TransportType protocol_{TransportType::UDP};
  struct Provider { const void *owner{nullptr}; std::string account_id; };
  std::array<Provider, 4> directory_providers_{};
  struct Trigger { template<class... T> void trigger(T...) {} } call_failed_trigger_, phonebook_update_trigger_;
  unsigned publications{0};
  void publish_destination_() { ++publications; }
  void publish_contacts_() {}
  void publish_last_reason_(const std::string &) {}
  void clear_terminal_call_snapshot_() {}
  template<class F> void defer(F fn) { fn(); }
  bool is_registered(const std::string &line) const { return line == "office" || line == "home"; }
  bool maybe_select_initial_contact_();
  bool resolve_selected_dial_target_();
  CONTACT_SELECTION_CHANGED
  bool set_contact(const std::string &);
  bool select_line(const std::string &);
  void next_contact(); void prev_contact();
  void publish_phonebook_changed_();
  std::string_view get_contact_account_view(const ContactEntry &) const;
  std::string get_current_destination() const;
  std::string get_current_destination_route() const;
  std::string get_current_contact_ip() const;
  uint16_t get_current_contact_port() const;
  uint16_t get_current_contact_rtp_port() const;
  bool get_current_contact_sip_transport_tcp() const;
  void boot() { initial_contact_ready_ = true; maybe_select_initial_contact_(); }
};
METHODS
}
using namespace esphome::voip_stack;

static ContactEntry row(const char *id, const char *name, const char *route, const char *account = "office") {
  ContactEntry entry;
  entry.id = id; entry.name = name; entry.route = route; entry.account_id = account;
  entry.ip = "192.0.2.20"; entry.port = 5060; entry.endpoint_type = ContactEndpointType::SIP;
  return entry;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  VoipStack phone;
  assert(phone.phonebook_.prepare_directory_storage());
  if (mode == "alphabetical") {
    assert(phone.phonebook_.add_entry(row("z", "Zulu", "300")) == AddResult::Added);
    phone.publish_phonebook_changed_();
    assert(!phone.initial_contact_applied_); // setup owns the complete static batch
    assert(phone.phonebook_.add_entry(row("b", "beta", "200")) == AddResult::Added);
    assert(phone.phonebook_.add_entry(row("a2", "Alpha", "102")) == AddResult::Added);
    assert(phone.phonebook_.add_entry(row("a1", "alpha", "101")) == AddResult::Added);
    phone.publish_phonebook_changed_(); phone.boot();
    assert(phone.get_current_destination_route() == "101");
    assert(phone.phonebook_.at(0)->stable_id() == "a1" && phone.phonebook_.at(1)->stable_id() == "a2");
    assert(phone.phonebook_.at(2)->stable_id() == "b" && phone.phonebook_.at(3)->stable_id() == "z");
    assert(phone.set_contact("b"));
    std::vector<ContactEntry> reverse;
    for (size_t i = phone.phonebook_.size(); i-- > 0;) reverse.push_back(*phone.phonebook_.at(i));
    reverse[1].name = "A renamed beta";
    assert(phone.phonebook_.replace_all(std::move(reverse))); phone.publish_phonebook_changed_();
    assert(phone.phonebook_.current()->stable_id() == "b");
    assert(phone.get_current_destination() == "A renamed beta");
  } else if (mode == "explicit-no-directory") {
    phone.initial_contact_ = "sip:door@192.0.2.30:5070;transport=tcp";
    phone.default_line_ = "direct"; phone.boot();
    assert(phone.get_current_destination_route() == phone.initial_contact_);
    assert(phone.get_current_contact_ip().empty() && phone.get_current_contact_port() == 0);
    assert(phone.phonebook_.add_entry(row("other", "First alphabetical", "101")) == AddResult::Added);
    phone.publish_phonebook_changed_();
    assert(phone.get_current_destination_route() == phone.initial_contact_);
    int provider;
    auto door = row("door-account", "Door", phone.initial_contact_.c_str(), "direct");
    assert(phone.phonebook_.replace_directory(&provider, {door}) == DirectoryCommitResult::APPLIED);
    phone.publish_phonebook_changed_(); assert(phone.selected_dial_target_resolved_);
    assert(phone.get_current_destination() == "Door" && phone.get_current_destination_route() == phone.initial_contact_);
    assert(phone.phonebook_.replace_directory(&provider, {}) == DirectoryCommitResult::APPLIED);
    phone.publish_phonebook_changed_();
    assert(!phone.selected_dial_target_resolved_ && phone.get_current_destination_route() == phone.initial_contact_);
    phone.next_contact(); assert(phone.get_current_destination_route() == "101");
    assert(phone.selected_dial_target_.empty());
    assert(phone.phonebook_.replace_directory(&provider, {door}) == DirectoryCommitResult::APPLIED);
    phone.publish_phonebook_changed_(); assert(phone.get_current_destination_route() == "101");
  } else if (mode == "account-scoped") {
    phone.initial_contact_ = "101"; phone.boot();
    assert(phone.get_current_destination_route() == "101");
    auto office = row("office-101", "Desk", "sip:101@office.example"); office.number = "101";
    auto home = row("home-101", "Desk", "sip:101@home.example", "home"); home.number = "101";
    assert(phone.phonebook_.replace_all({home, office})); phone.publish_phonebook_changed_();
    assert(phone.get_current_destination_route() == "sip:101@office.example");
    assert(phone.select_line("home")); assert(phone.get_current_destination_route() == "sip:101@home.example");
    assert(phone.select_line("direct")); // two account rows cannot decide a direct destination
    assert(!phone.selected_dial_target_resolved_ && phone.get_current_destination_route() == "101");
    assert(!phone.select_line("missing") && phone.default_line_ == "direct");
  } else if (mode == "late-directory") {
    phone.boot(); assert(!phone.initial_contact_applied_);
    phone.call_state_ = CallState::IN_CALL;
    assert(phone.phonebook_.replace_all({row("z", "Zulu", "300"), row("a", "Alpha", "100")}));
    phone.publish_phonebook_changed_(); assert(!phone.initial_contact_applied_);
    phone.call_state_ = CallState::IDLE;
    assert(phone.maybe_select_initial_contact_());
    assert(phone.get_current_destination_route() == "100");
    phone.next_contact(); assert(phone.get_current_destination_route() == "300");
    phone.publish_phonebook_changed_(); assert(phone.get_current_destination_route() == "300");
    phone.prev_contact(); assert(phone.get_current_destination_route() == "100");
  } else if (mode == "static-self" || mode == "only-self" || mode == "explicit-self" ||
             mode == "same-user-other-host" || mode == "bare-self") {
    phone.known_account.config.id = "office";
    phone.known_account.config.username = "100";
    phone.known_account.config.domain = "pbx.example";
    phone.known_account.config.registrar = "pbx.example";
    const char *self_route = mode == "bare-self" ? "100" : "sip:%31%30%30@PBX.example:5060";
    assert(phone.phonebook_.add_entry(row("self", "A Self", self_route)) == AddResult::Added);
    if (mode != "only-self") {
      const char *peer_route = mode == "same-user-other-host" ? "sip:100@another.example" : "sip:202@pbx.example";
      assert(phone.phonebook_.add_entry(row("peer", "Z Peer", peer_route)) == AddResult::Added);
    }
    if (mode == "explicit-self") phone.initial_contact_ = "self";
    phone.boot();
    if (mode == "only-self") {
      assert(!phone.initial_contact_applied_);
      assert(phone.get_current_destination_route().empty());
      assert(phone.phonebook_.add_entry(row("peer", "Z Peer", "sip:202@pbx.example")) == AddResult::Added);
      phone.publish_phonebook_changed_();
      assert(phone.initial_contact_applied_ && phone.get_current_destination_route() == "sip:202@pbx.example");
    } else if (mode == "explicit-self") {
      assert(phone.get_current_destination_route() == self_route);
    } else {
      assert(phone.phonebook_.current()->stable_id() == "peer");
      assert(phone.set_contact("self"));
      phone.publish_phonebook_changed_();
      assert(phone.get_current_destination_route() == self_route);
    }
  } else if (mode == "username-tie") {
    assert(phone.phonebook_.add_entry(row("a-manual", "Desk", "sip:202@pbx.example")) == AddResult::Added);
    assert(phone.phonebook_.add_entry(row("z-manual", "Desk", "sip:101@pbx.example")) == AddResult::Added);
    phone.boot();
    assert(phone.get_current_destination_route() == "sip:101@pbx.example");
    assert(phone.phonebook_.current()->stable_id() == "z-manual");
  } else { assert(false); }
}
