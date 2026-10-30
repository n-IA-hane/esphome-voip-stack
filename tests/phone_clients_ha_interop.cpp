// Real production protocol clients, exercised by Intercom's ephemeral HA servers.
#include "esphome/components/ldap_directory/ldap_protocol.h"
#include "esphome/components/ldap_directory/directory_contact.h"
#include "esphome/components/imap_voicemail/imap_protocol.h"
#include <array>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  std::array<std::string, 9> input;
  for (auto &field : input) if (!std::getline(std::cin, field)) return 3;
  // Credentials arrive on stdin, never process arguments or diagnostic output.
  const std::string mode = argv[1];
  esphome::phone_client::TcpStream stream;
  if (!stream.connect(input[0], static_cast<uint16_t>(std::stoul(input[1])), 3000)) return 4;
  if (mode == "directory") {
    esphome::ldap_directory::Client client(stream);
    char receive[8192];
    client.set_receive_buffer(receive, sizeof(receive));
    if (!client.bind(input[5], input[3], 3000)) {
      std::cout << "AUTH_FAILED " << client.result_code() << '\n';
      return 0;
    }
    esphome::ldap_directory::Search query;
    query.base_dn = input[4];
    query.filter = "(|(telephoneNumber=*)(labeledURI=*))";
    query.size_limit = static_cast<uint32_t>(std::stoul(input[8]));
    std::vector<esphome::ldap_directory::DirectoryContact> contacts;
    if (!client.search_each(query, [&](esphome::ldap_directory::Entry &&entry) {
          esphome::ldap_directory::DirectoryContact contact;
          if (!esphome::ldap_directory::map_contact(entry, input[0], 5060, false, &contact)) return false;
          contacts.push_back(std::move(contact)); return true;
        }, 3000)) {
      std::cout << "SEARCH_FAILED " << client.result_code() << '\n';
      return 0;
    }
    std::cout << "DIRECTORY " << contacts.size() << '\n';
    for (const auto &contact : contacts) {
      std::cout << contact.id << '|' << contact.name << '|' << contact.uri << '|' << contact.kind << '\n';
    }
    client.unbind(1000);
    return 0;
  }
  using namespace esphome::imap_voicemail;
  uint8_t literal[16384], fetch[4096];
  Client client(stream, literal, sizeof(literal));
  client.set_fetch_buffer(fetch, sizeof(fetch));
  if (!client.login(input[2], input[3], 3000)) {
    std::cout << "AUTH_FAILED\n";
    return 0;
  }
  if (!client.select(input[6], 3000)) return 7;
  Mailbox mailbox;
  const auto catalogue = [&]() { return client.catalogue(16, 0, &mailbox, 3000); };
  if (!catalogue()) return 8;
  if (mode == "idle") {
    if (!client.supports_idle() || !client.start_idle(3000)) return 9;
    std::cout << "IDLE_READY" << std::endl;
    bool changed = false;
    if (!client.wait_idle(5000, &changed) || !changed || !client.end_idle(3000)) return 10;
    if (!client.select(input[6], 3000) || !catalogue()) return 11;
    std::cout << "UPDATED " << mailbox.total << '\n';
  } else if (mode == "fetch" || mode == "seen" || mode == "unseen" || mode == "delete") {
    if (mailbox.messages.empty()) return 12;
    const auto message = mailbox.messages.front();
    if (mode == "fetch") {
      if (!message.audio.valid()) return 13;
      std::ofstream output(input[7], std::ios::binary);
      TransferDecoder decoder(message.audio.encoding);
      uint32_t offset = 0;
      for (;;) {
        size_t received = 0;
        if (!client.fetch_audio(message.uid, message.audio, offset, sizeof(fetch), &received, 3000)) return 14;
        if (offset > 1024 * 1024 || received > sizeof(fetch)) return 15;
        if (!decoder.feed(client.fetched_data(), received, [&](const uint8_t *data, size_t count) {
              output.write(reinterpret_cast<const char *>(data), count);
              return bool(output);
            })) return 16;
        offset += received;
        if (received < sizeof(fetch)) break;
      }
      if (!decoder.finish()) return 17;
      std::cout << "FETCHED " << message.uid << '\n';
    } else {
      const bool success = mode == "delete" ? client.delete_uid(message.uid, 3000) :
          client.store_seen(message.uid, mode == "seen", 3000);
      if (!success) return 18;
      std::cout << "CHANGED " << message.uid << '\n';
    }
  } else if (mode == "catalogue") {
    std::cout << "MAILBOX " << mailbox.uid_validity << ' ' << mailbox.total << ' ' << mailbox.messages.size() << '\n';
    for (const auto &message : mailbox.messages)
      std::cout << message.uid << '|' << message.seen << '|' << message.from << '|' << message.subject << '|'
                << message.has_duration << '|' << message.duration_seconds << '|' << message.audio.section << '|'
                << message.audio.mime << '\n';
  } else return 19;
  client.logout(1000);
  return 0;
}
