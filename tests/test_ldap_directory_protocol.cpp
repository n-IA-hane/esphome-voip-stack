#include "esphome/components/ldap_directory/ldap_protocol.h"
#include "esphome/components/ldap_directory/directory_contact.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <random>

using namespace esphome::ldap_directory;
using namespace esphome::phone_client;

class Fragmented : public Stream {
 public:
  std::string input, output;
  size_t position{0};
  int read(uint8_t *data, size_t length, uint32_t) override {
    if (position == input.size()) return 0;
    const size_t count = std::min<size_t>({length, size_t(1), input.size() - position});
    std::copy_n(input.data() + position, count, data); position += count; return count;
  }
  int write(const uint8_t *data, size_t length, uint32_t) override {
    const size_t count = std::min<size_t>(length, 2); output.append(reinterpret_cast<const char *>(data), count); return count;
  }
  void close() override {}
  void interrupt() override {}
};

int main(int argc, char **argv) {
  assert(argc >= 2);
  const std::string mode = argv[1];
  if (mode == "filter") {
    assert(argc == 3);
    Search search; search.base_dn = "ou=phonebook,dc=voip"; search.filter = argv[2];
    std::string encoded;
    if (!encode_search(9, search, &encoded)) return 2;
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char c : encoded) std::cout << hex[c >> 4] << hex[c & 15];
    std::cout << '\n'; return 0;
  }
  if (mode == "wire") {
    assert(argc == 6);
    TcpStream stream;
    assert(stream.connect("127.0.0.1", static_cast<uint16_t>(std::stoi(argv[2])), 3000));
    Client client(stream);
    char receive[8192]; client.set_receive_buffer(receive, sizeof(receive));
    if (!client.bind("uid=reader,ou=accounts,dc=voip", argv[5], 3000)) {
      std::cout << "BIND_ERROR " << client.result_code() << '\n'; return 0;
    }
    Search query; query.base_dn = "ou=phonebook,dc=voip"; query.filter = argv[3]; query.size_limit = std::stoi(argv[4]);
    std::vector<Entry> entries{{"previous", {}}};
    if (!client.search(query, &entries, 3000)) {
      assert(entries.size() == 1 && entries[0].dn == "previous");
      std::cout << "SEARCH_ERROR " << client.result_code() << '\n'; return 0;
    }
    std::cout << "SUCCESS " << entries.size() << '\n';
    for (const auto &entry : entries) {
      DirectoryContact contact;
      assert(map_contact(entry, "pbx.example.test", 5060, false, &contact));
      std::cout << contact.id << '|' << contact.name << '|' << contact.user << '|' << contact.host << '|' << contact.port << '|' << contact.tcp << '|' << contact.kind << '\n';
    }
    client.unbind(1000); return 0;
  }
  assert(mode == "unit");
  for (const auto &value : {"(objectClass=*)", "(&(cn=Alice*)(|(telephoneNumber=101)(telephoneNumber=102)))",
                            "(!(cn=Bob))", "(cn=Alice\\2a)", "(cn=*li*)", "(uid>=100)", "(cn~=Alice)", "(uid<=200)"})
    assert(valid_filter(value));
  for (const auto &value : {"", "cn=foo", "(cn=foo", "(cn=foo))", "(&(cn=foo))junk", "(!(cn=foo)(cn=bar))",
                            "(cn=bad\\xz)", "(cn:dn:=foo)", "()", "(&)", "(cn>=a*)"})
    assert(!valid_filter(value));
  assert(escape_dn_value(" #a,b+\"\\ ") == "\\ #a\\,b\\+\\\"\\\\\\ ");
  std::string packet;
  assert(encode_bind(1, "uid=reader,dc=voip", "fixture-password", &packet));
  Fragmented fragment; fragment.input = packet;
  std::string copy;
  assert(read_message(fragment, &copy, 8192, 1000) && copy == packet);
  Fragmented truncated; truncated.input = packet.substr(0, packet.size() - 1);
  assert(!read_message(truncated, &copy, 8192, 1000));
  Fragmented indefinite; indefinite.input = std::string("\x30\x80", 2);
  assert(!read_message(indefinite, &copy, 8192, 1000));
  Fragmented oversized; oversized.input = std::string("\x30\x84\xff\xff\xff\xff", 6);
  assert(!read_message(oversized, &copy, 8192, 1000));
  Fragmented writer;
  assert(write_all(writer, packet, 1000) && writer.output == packet);
  Result result;
  const std::string success("\x30\x0c\x02\x01\x01\x61\x07\x0a\x01\x00\x04\x00\x04\x00", 14);
  assert(decode_message(success, &result) && result.code == 0 && result.operation == 0x61);
  std::mt19937 random(17);
  for (size_t i = 0; i < 5000; ++i) {
    std::string bytes(random() % 300, '\0');
    for (auto &c : bytes) c = static_cast<char>(random());
    decode_message(bytes, &result);
  }
  Entry entry{"cn=No uid,ou=phonebook,dc=voip", {{"cn", {"Reception"}}, {"telephoneNumber", {"101"}}, {"ou", {"sip_account"}}}};
  DirectoryContact contact;
  assert(map_contact(entry, "pbx.example.test", 5070, true, &contact));
  assert(contact.id.rfind("ldap:", 0) == 0 && contact.name == "Reception" && contact.user == "101" && contact.tcp && contact.port == 5070);
  entry.attributes.push_back({"labeledURI", {"sip:front%2Ddesk@192.0.2.4:5062;transport=tcp Front desk"}});
  assert(map_contact(entry, "ignored", 1, false, &contact));
  assert(contact.user == "front-desk" && contact.host == "192.0.2.4" && contact.port == 5062 && contact.tcp);
  assert(contact.uri == "sip:front%2Ddesk@192.0.2.4:5062;transport=tcp");
  assert(contact.number == "101");
  entry.attributes[1].values[0] = "+3900123456";
  assert(map_contact(entry, "ignored", 1, false, &contact));
  assert(contact.user == "front-desk" && contact.number == "+3900123456");
  entry.attributes[1].values[0] = "00101";
  assert(map_contact(entry, "ignored", 1, false, &contact));
  assert(contact.number == "00101");
  entry.attributes[1].values.clear();
  assert(map_contact(entry, "ignored", 1, false, &contact));
  assert(contact.number.empty() && contact.user == "front-desk");
  entry.attributes[1].values = {"123\nInjected"};
  assert(map_contact(entry, "ignored", 1, false, &contact) && contact.number.empty());
  entry.attributes[1].values = {std::string(97, '1')};
  assert(map_contact(entry, "ignored", 1, false, &contact) && contact.number.empty());
  entry.attributes[1].values = {"101"};
  entry.attributes.back().values[0] = "SIP:front@192.0.2.4:5062;transport=tcp";
  assert(map_contact(entry, "ignored", 1, false, &contact) && contact.uri.rfind("sip:", 0) == 0);
  entry.attributes.back().values[0] = "sip:user:password@192.0.2.4";
  assert(!map_contact(entry, "ignored", 1, false, &contact));
  entry.attributes.back().values[0] = "sip:front@192.0.2.4;transport=tcp;transport=udp";
  assert(!map_contact(entry, "ignored", 1, false, &contact));
  entry.attributes.back().values[0] = "sip:front@[2001:db8::1]";
  assert(!map_contact(entry, "ignored", 1, false, &contact));
  for (const std::string target : {"Living%20room", "Night%20group"}) {
    entry.attributes.back().values[0] = "sip:" + target + "@192.0.2.4:5062;transport=tcp";
    assert(map_contact(entry, "ignored", 1, false, &contact));
    assert(contact.uri == entry.attributes.back().values[0]);
    assert(contact.user == (target == "Living%20room" ? "Living room" : "Night group"));
  }
  entry.attributes.back().values[0] = "sip:Living%2520room@192.0.2.4";
  assert(map_contact(entry, "ignored", 1, false, &contact));
  assert(contact.user == "Living%20room" && contact.uri == entry.attributes.back().values[0]);
  for (const std::string target : {"Living room", "bad%00user", "bad%0Auser", "bad%1Fuser", "bad%7Fuser"}) {
    entry.attributes.back().values[0] = "sip:" + target + "@192.0.2.4";
    assert(!map_contact(entry, "ignored", 1, false, &contact));
  }
  entry.attributes.pop_back();
  entry.attributes[1].values[0] = "Living room";
  assert(!map_contact(entry, "pbx.example.test", 5060, false, &contact));
  std::cout << "PASS bounded BER/filter/framing/atomic failure/mapping" << '\n';
}
