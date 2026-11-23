int main(int argc, char **argv) {
  assert(argc == 3);
  const bool tcp = std::string(argv[1]) == "tcp";
  const bool select_other = std::string(argv[2]) == "other";
  VoipStack app;
  auto &phone = *app.transport_;
  Peer office(tcp), other(tcp);
  bind_account(phone, office, "100");
  auto config = phone.registration_.account(0).config;
  config.id = "other";
  config.domain = "other.example";
  config.port = ntohs(other.address.sin_port);
  if (tcp) phone.registration_flows_[1].socket = other.esp;
  assert(phone.registration_.set_account(config, clock_ms));
  phone.registration_.tick(clock_ms);
  auto registration = one(other, "REGISTER ");
  assert(header_value(registration, "From").find("100@other.example") != std::string::npos);
  auto registered = response(registration, 200);
  registered.insert(registered.find("Content-Length:"), "Contact: " + header_value(registration, "Contact") + "\r\n");
  other.send_to(phone, registered);
  assert(phone.registration_.is_registered("office") && phone.registration_.is_registered("other"));

  ContactEntry a, b;
  a.name = b.name = "Desk";
  a.id = b.id = "101";
  a.route = "sip:101@pbx.example";
  b.route = "sip:101@other.example";
  a.kind = b.kind = ContactKind::SIP_ACCOUNT;
  a.account_id = "office";
  b.account_id = "other";
  assert(app.phonebook_.add_entry(a) == AddResult::Added);
  assert(app.phonebook_.add_entry(b) == AddResult::Added);
  assert(phone.configure_blf(2, nullptr));
  SipMwi::Hooks hooks;
  hooks.context = &phone;
  hooks.send = SipTransport::registration_send_view_;
  hooks.reply = SipTransport::registration_mwi_reply_;
  hooks.local_ip = SipTransport::registration_local_ip_;
  hooks.random = esp_random;
  hooks.hash = SipTransport::registration_hash_;
  hooks.local_port = phone.sip_port_;
  phone.mwi_.set_hooks(hooks);
  assert(phone.set_blf_watches({{"101", "office", "sip:101@pbx.example"},
                                {"101", "other", "sip:101@other.example"}}));
  phone.pump_registration_mwi_(clock_ms);
  const auto subscribe = [&](Peer &peer, const char *domain) {
    std::string dialog;
    for (const auto &record : peer.drain()) {
      assert(record.rfind("SUBSCRIBE ", 0) == 0);
      if (header_value(record, "Event") == "dialog") {
        dialog = record;
      } else {
        assert(header_value(record, "Event") == "message-summary");
        peer.send_to(phone, response(record, 200));
        peer.send_to(phone, notification(record, 1));
        one(peer, "SIP/2.0 200");
      }
    }
    assert(!dialog.empty());
    assert(dialog.rfind(std::string("SUBSCRIBE sip:101@") + domain + " SIP/2.0\r\n", 0) == 0);
    assert(header_value(dialog, "From").find(std::string("100@") + domain) != std::string::npos);
    assert(header_value(dialog, "Accept") == "application/dialog-info+xml");
    assert(header_value(dialog, "X-Voip-Call-ID").empty());
    peer.send_to(phone, response(dialog, 200));
    return dialog;
  };
  const auto office_subscribe = subscribe(office, "pbx.example");
  const auto other_subscribe = subscribe(other, "other.example");
  assert(header_value(office_subscribe, "Call-ID") != header_value(other_subscribe, "Call-ID"));
  const auto document = [](const char *domain, const char *state, unsigned version = 0) {
    return std::string("<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' entity='sip:101@") + domain +
        "' version='" + std::to_string(version) + "' state='full'><dialog id='incoming' direction='recipient'><state>" +
        state + "</state></dialog></dialog-info>";
  };
  office.send_to(phone, notify_dialog(office_subscribe, 1,
      document("pbx.example", select_other ? "confirmed" : "early")));
  one(office, "SIP/2.0 200");
  other.send_to(phone, notify_dialog(other_subscribe, 1,
      document("other.example", select_other ? "early" : "confirmed")));
  one(other, "SIP/2.0 200");
  const std::string selected_line = select_other ? "other" : "office";
  const std::string active_line = select_other ? "office" : "other";
  BlfStateInfo info;
  assert(!phone.get_blf_info("101", &info));
  assert(phone.get_blf_info("101", &info, selected_line) && info.available &&
         info.state == BlfState::RINGING && info.ringing_dialogs == 1);
  assert(phone.get_blf_info("101", &info, active_line) && info.available && info.state == BlfState::ACTIVE);
  assert(!app.pickup_contact("101") && !app.pickup_contact("101", "missing"));
  assert(!app.pickup_contact("101", active_line));
  assert(office.drain().empty() && other.drain().empty());
  assert(select_other ? app.pickup_contact("sip:101@other.example") : app.pickup_contact("101", selected_line));
  Peer &selected = select_other ? other : office;
  Peer &unselected = select_other ? office : other;
  const std::string domain = select_other ? "other.example" : "pbx.example";
  const auto invite = one(selected, "INVITE ");
  assert(invite.rfind("INVITE sip:%2A99101@" + domain + " SIP/2.0\r\n", 0) == 0);
  assert(header_value(invite, "From").find("100@" + domain) != std::string::npos);
  assert(app.phonebook_.current()->account_id == selected_line);
  assert(unselected.drain().empty());
  int audio = socket(AF_INET, SOCK_DGRAM, 0);
  assert(audio >= 0);
  sockaddr_in media{};
  media.sin_family = AF_INET;
  media.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(audio, reinterpret_cast<sockaddr *>(&media), sizeof(media)) == 0);
  socklen_t media_size = sizeof(media);
  assert(getsockname(audio, reinterpret_cast<sockaddr *>(&media), &media_size) == 0);
  const auto offer = message_body(invite);
  const auto format = offer.find(" L16/16000");
  assert(format != std::string::npos);
  const auto payload_at = offer.rfind("a=rtpmap:", format) + 9;
  const auto payload = offer.substr(payload_at, format - payload_at);
  const std::string answer = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Independent PBX\r\n"
      "c=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio " + std::to_string(ntohs(media.sin_port)) +
      " RTP/AVP " + payload + "\r\na=rtpmap:" + payload + " L16/16000\r\na=ptime:20\r\na=sendrecv\r\n";
  auto accepted = response(invite, 200);
  accepted.resize(accepted.find("Content-Length:"));
  accepted += "Contact: <" + selected.uri() + ">\r\nContent-Type: application/sdp\r\nContent-Length: " +
      std::to_string(answer.size()) + "\r\n\r\n" + answer;
  selected.send_to(phone, accepted);
  one(selected, "ACK ");
  const auto call_id = phone.call_id_;
  assert(!call_id.empty() && phone.send_bye_unlocked_(call_id));
  const auto bye = one(selected, "BYE ");
  assert(header_value(bye, "Call-ID") == call_id);
  selected.send_to(phone, response(bye, 200));
  assert(phone.call_id_.empty() && unselected.drain().empty());
  selected.send_to(phone, notify_dialog(select_other ? other_subscribe : office_subscribe, 2,
      document(domain.c_str(), "terminated", 1)));
  one(selected, "SIP/2.0 200");
  assert(phone.get_blf_info("101", &info, selected_line) && info.available && info.state == BlfState::IDLE);
  assert(phone.get_blf_info("101", &info, active_line) && info.available && info.state == BlfState::ACTIVE);
  for (auto &flow : phone.registration_flows_) phone.close_tcp_flow_(flow);
  close(phone.sip_socket_);
  close(audio);
}
