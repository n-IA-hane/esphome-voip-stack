#pragma once

#include "ldap_protocol.h"

namespace esphome::ldap_directory {
struct DirectoryContact {
  std::string id, name, user, host, kind, uri, number;
  uint16_t port{5060};
  bool tcp{false};
};
bool map_contact(const Entry &entry, const std::string &default_host, uint16_t default_port,
                 bool default_tcp, DirectoryContact *contact);
}
