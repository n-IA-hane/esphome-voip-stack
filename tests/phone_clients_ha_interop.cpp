// Production LDAP client exercised against Intercom's ephemeral HA directory.
#include "esphome/components/ldap_directory/directory_contact.h"
#include "esphome/components/ldap_directory/ldap_protocol.h"
#include <array>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::array<std::string, 6> input;
  for (auto &field : input)
    if (!std::getline(std::cin, field))
      return 3;
  // Credentials arrive on stdin, never process arguments or diagnostic output.
  const std::string mode = argv[1];
  esphome::phone_client::TcpStream stream;
  if (!stream.connect(input[0], static_cast<uint16_t>(std::stoul(input[1])),
                      3000))
    return 4;
  if (mode == "directory") {
    esphome::ldap_directory::Client client(stream);
    char receive[8192];
    client.set_receive_buffer(receive, sizeof(receive));
    if (!client.bind(input[2], input[3], 3000)) {
      std::cout << "AUTH_FAILED " << client.result_code() << '\n';
      return 0;
    }
    esphome::ldap_directory::Search query;
    query.base_dn = input[4];
    query.filter = "(|(telephoneNumber=*)(labeledURI=*))";
    query.size_limit = static_cast<uint32_t>(std::stoul(input[5]));
    std::vector<esphome::ldap_directory::DirectoryContact> contacts;
    if (!client.search_each(
            query,
            [&](esphome::ldap_directory::Entry &&entry) {
              esphome::ldap_directory::DirectoryContact contact;
              if (!esphome::ldap_directory::map_contact(entry, input[0], 5060,
                                                        false, &contact))
                return false;
              contacts.push_back(std::move(contact));
              return true;
            },
            3000)) {
      std::cout << "SEARCH_FAILED " << client.result_code() << '\n';
      return 0;
    }
    std::cout << "DIRECTORY " << contacts.size() << '\n';
    for (const auto &contact : contacts) {
      std::cout << contact.id << '|' << contact.name << '|' << contact.uri
                << '|' << contact.kind << '\n';
    }
    client.unbind(1000);
    return 0;
  }
  return 2;
}
