#include "roster_contact.h"
#include "phonebook.h"

#include <cassert>

using namespace esphome::voip_stack;

int main() {
  RosterContactIdentity row;
  const char *queue = "queue:869caa77-3f43-4ffa-9495-851279dca47e";
  assert(resolve_roster_contact({queue, queue, "Reception queue", "701", "39001", "sip:702@pbx.test"}, &row));
  assert(row.stable_id == queue && row.route == "701" && row.name == "Reception queue" && row.number == "701");
  assert(resolve_roster_contact({queue, queue, "Reception queue", "", "39001", "sip:702@pbx.test"}, &row));
  assert(row.route == "39001");
  assert(resolve_roster_contact({queue, queue, "Reception queue", "", "", "sip:702@pbx.test;transport=tcp"}, &row));
  assert(row.route == "sip:702@pbx.test;transport=tcp");
  assert(!resolve_roster_contact({queue, queue, "Queue without destination", "", "", ""}, &row));
  assert(!resolve_roster_contact({"paging:869caa77", "paging:869caa77", "Paging", "", "", ""}, &row));
  assert(resolve_roster_contact({"paging:869caa77", "paging:869caa77", "Paging", "799", "", ""}, &row));
  assert(row.route == "799");
  assert(resolve_roster_contact({"account-key", "stable-account", "  Caffè,@%|;!  ", "200", "", ""}, &row));
  assert(row.name == "  Caffè,@%|;!  ");
  assert(resolve_roster_contact({queue, queue, "", "701", "", ""}, &row) && row.name == "701");
  assert(!resolve_roster_contact({"esp_84c7bb000001", "device-uuid", "", "", "", ""}, &row));
  std::string long_name(63, 'a'); long_name += "è room";
  assert(resolve_roster_contact({"201", "contact-201", long_name, "201", "", ""}, &row));
  assert(row.name == std::string(63, 'a'));
  std::string full_name(62, 'a'); full_name += "è";
  assert(resolve_roster_contact({"201", "contact-201", full_name, "201", "", ""}, &row) && row.name == full_name);
  auto invalid = long_name; invalid += '\n';
  assert(!resolve_roster_contact({"201", "contact-201", invalid, "201", "", ""}, &row));
  assert(!resolve_roster_contact({queue, queue, "Queue", "queue:bad", "701", ""}, &row));
  Phonebook book;
  assert(resolve_roster_contact({queue, queue, "Reception", "701", "", ""}, &row));
  ContactEntry contact; contact.id = row.stable_id; contact.name = row.name; contact.route = row.route;
  assert(book.add_entry(contact) == AddResult::Added && book.select(queue));
  assert(book.current_route() == "701" && book.current_name() == "Reception");
}
