// Exercise literal SIP account identity at the production LDAP boundary.
#include "directory_contact.h"
#include "sip_uri_user.h"

#include <cassert>
#include <string>

using namespace esphome::ldap_directory;
using namespace esphome::voip_stack;

int main() {
  for (const std::string username : {"Reception queue", "Caffè,@%|;!", "*97", "Desk@Office", "100%", "Family, Night"}) {
    Entry entry{"uid=fixture,ou=phones,dc=voip", {{"uid", {username}}, {"displayName", {username}},
        {"ou", {"sip_account"}}, {"labeledURI", {"sip:" + sip_uri_user_encode(username) + "@pbx.test"}}}};
    DirectoryContact contact;
    assert(map_contact(entry, "pbx.test", 5060, false, &contact, true));
    assert(contact.id == username && contact.user == username && contact.name == username);
    assert(contact.uri == "sip:" + sip_uri_user_encode(username) + "@pbx.test");
    entry.attributes[0].values[0] = "legacy-ha-device-uuid";
    assert(!map_contact(entry, "pbx.test", 5060, false, &contact, true));
  }
  Entry entry{"uid=fixture,dc=voip", {{"uid", {std::string(64, 'x')}}, {"ou", {"phone"}}}};
  DirectoryContact contact;
  assert(map_contact(entry, "pbx.test", 5060, false, &contact, true));
  assert(contact.id == std::string(64, 'x') && contact.kind == "sip_account");
  entry.attributes[0].values[0] += 'x';
  assert(!map_contact(entry, "pbx.test", 5060, false, &contact, true));
  for (const std::string username : {std::string("bad\xc0\xaf"), std::string("bad\xed\xa0\x80"), std::string("bad\nname")}) {
    entry.attributes[0].values[0] = username;
    assert(!map_contact(entry, "pbx.test", 5060, false, &contact, true));
  }
  const std::string base = "sip:Desk%20Office@pbx.test;x=";
  const std::string maximum = base + std::string(SIP_URI_BYTES - base.size(), 'a');
  SipTargetParts parts;
  assert(maximum.size() == 2048 && sip_parse_target_uri(maximum, parts));
  assert(parts.user == "Desk Office");
  assert(!sip_parse_target_uri(maximum + 'a', parts));
  assert(!sip_parse_target_uri("sip:bad%C0%AF@pbx.test", parts));
  assert(sip_parse_target_uri("sip:" + std::string(65, 'x') + "@pbx.test", parts));
  assert(sip_parse_target_uri("sip:%2A%2A" + std::string(64, 'x') + "@pbx.test", parts));
  assert(parts.user.size() == 66);
  assert(!sip_parse_target_uri("sip:" + std::string(1025, 'x') + "@pbx.test", parts));

  const std::string route = "Trunk." + std::string(CONTACT_ID_BYTES - 6, 'n');
  Entry group{"cn=public-route,dc=voip", {{"uid", {route}}, {"ou", {"ring_group"}},
      {"displayName", {std::string(63, 'a') + "è public name"}},
      {"labeledURI", {"sip:" + sip_uri_user_encode(route) + "@pbx.test"}}}};
  assert(map_contact(group, "pbx.test", 5060, false, &contact, true));
  assert(contact.id == route && contact.user == route && contact.kind == "ring_group");
  assert(contact.name == std::string(63, 'a'));
  assert(contact.uri == group.attributes.back().values[0]);
  group.attributes[0].values[0] += 'n';
  assert(!map_contact(group, "pbx.test", 5060, false, &contact, true));

  // Encoded wire length is independent of the decoded public identity limit.
  group.attributes[0].values[0] = std::string(700, ' ');
  group.attributes.back().values[0] = "sip:" + sip_uri_user_encode(group.attributes[0].values[0]) + "@pbx.test";
  assert(!map_contact(group, "pbx.test", 5060, false, &contact, true));

  Entry without_uid{"cn=unrelated-dn,dc=voip", {{"entryUUID", {"opaque-directory-object"}},
      {"labeledURI", {"sip:public-route@pbx.test"}}}};
  assert(map_contact(without_uid, "pbx.test", 5060, false, &contact));
  assert(contact.id == "public-route" && contact.name == "public-route");
  without_uid.attributes.pop_back();
  assert(!map_contact(without_uid, "pbx.test", 5060, false, &contact));
}
