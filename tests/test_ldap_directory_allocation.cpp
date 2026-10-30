#include "esphome/components/ldap_directory/ldap_protocol.h"
#include "esphome/components/ldap_directory/directory_contact.h"
#include "contact_entry_fixture.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>

namespace allocation_witness {
bool enabled = false;
size_t live = 0, peak = 0, calls = 0;
struct alignas(std::max_align_t) Header { size_t bytes; bool tracked; };
void *allocate(size_t bytes) {
  auto *block = static_cast<Header *>(std::malloc(sizeof(Header) + std::max<size_t>(bytes, 1)));
  if (block == nullptr) throw std::bad_alloc();
  block->bytes = bytes; block->tracked = enabled;
  if (enabled) { live += bytes; peak = std::max(peak, live); ++calls; }
  return block + 1;
}
void release(void *pointer) {
  if (pointer == nullptr) return;
  auto *block = static_cast<Header *>(pointer) - 1;
  if (block->tracked) { assert(live >= block->bytes); live -= block->bytes; }
  std::free(block);
}
}  // namespace allocation_witness
void *operator new(size_t n) { return allocation_witness::allocate(n); }
void *operator new[](size_t n) { return allocation_witness::allocate(n); }
void operator delete(void *p) noexcept { allocation_witness::release(p); }
void operator delete[](void *p) noexcept { allocation_witness::release(p); }
void operator delete(void *p, size_t) noexcept { allocation_witness::release(p); }
void operator delete[](void *p, size_t) noexcept { allocation_witness::release(p); }

class RecordedPeer : public esphome::phone_client::Stream {
 public:
  explicit RecordedPeer(const std::string &wire) : wire_(wire) {}
  int read(uint8_t *out, size_t bytes, uint32_t) override {
    const size_t count = std::min(bytes, this->wire_.size() - this->position_);
    std::memcpy(out, this->wire_.data() + this->position_, count); this->position_ += count; return count;
  }
  int write(const uint8_t *, size_t bytes, uint32_t) override { return bytes; }
  void close() override {}
  void interrupt() override {}
 private:
  const std::string &wire_;
  size_t position_{0};
};

int main(int argc, char **argv) {
  assert(argc == 4);
  std::ifstream file(argv[1], std::ios::binary);
  const std::string wire((std::istreambuf_iterator<char>(file)), {});
  const size_t expected = std::stoul(argv[2]);
  const bool streamed = std::string(argv[3]) == "stream";
  using namespace esphome::ldap_directory;
  using esphome::voip_stack::ContactEntry;
  // Model only the boot-owned stage/receive storage outside the ordinary
  // allocator. This measures host requested bytes, not ESP DMA allocation.
  auto *stage = static_cast<ContactEntry *>(std::malloc(64 * sizeof(ContactEntry)));
  assert(stage != nullptr);
  for (size_t i = 0; i < 64; ++i) new (&stage[i]) ContactEntry();
  char receive[8192];
  Search query; query.base_dn = "ou=phonebook,dc=voip";
  query.filter = "(|(telephoneNumber=*)(labeledURI=*))"; query.size_limit = 64;
  for (unsigned round = 0; round < 5; ++round) {
    assert(allocation_witness::live == 0);
    allocation_witness::peak = allocation_witness::calls = 0;
    allocation_witness::enabled = true;
    size_t count = 0;
    {
      RecordedPeer peer(wire); Client client(peer); client.set_receive_buffer(receive, sizeof(receive));
      auto append = [&](Entry &&entry) {
        assert(count < 64);
        DirectoryContact mapped;
        if (!map_contact(entry, "192.0.2.1", 5060, false, &mapped)) return false;
        auto &item = stage[count++];
        item.id = std::move(mapped.id); item.name = std::move(mapped.name);
        item.route = mapped.uri.empty() ? std::move(mapped.user) : std::move(mapped.uri);
        item.ip = std::move(mapped.host); item.port = mapped.port;
        return true;
      };
      if (streamed) {
        assert(client.search_each(query, append, 3000));
      } else {
        // This is the original production worker's public search contract:
        // retain the complete entry tree until all contacts have been mapped.
        std::vector<Entry> entries;
        assert(client.search(query, &entries, 3000));
        for (auto &entry : entries) assert(append(std::move(entry)));
      }
      assert(count == expected);
      assert(stage[0].route == "sip:receiver000@192.0.2.1:5060;transport=udp");
    }
    for (size_t i = 0; i < count; ++i) { stage[i].~ContactEntry(); new (&stage[i]) ContactEntry(); }
    const size_t peak = allocation_witness::peak, calls = allocation_witness::calls, live = allocation_witness::live;
    allocation_witness::enabled = false;
    std::printf("%u %zu %zu %zu %zu\n", round, peak, calls, live, 64 * sizeof(ContactEntry));
    assert(live == 0);
  }
  for (size_t i = 0; i < 64; ++i) stage[i].~ContactEntry();
  std::free(stage);
}
