#include "esphome/components/imap_voicemail/imap_protocol.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>

using namespace esphome::imap_voicemail;
using namespace esphome::phone_client;

int main(int argc, char **argv) {
  assert(argc >= 2);
  const std::string action = argv[1];
  if (action == "unit") {
    uint32_t uid;
    assert(parse_uid("4294967295", &uid) && uid == UINT32_MAX);
    assert(!parse_uid("4294967296", &uid) && !parse_uid("0", &uid) && !parse_uid("1\r\nNOOP", &uid));
    assert(quote("a\\b\"c") == "\"a\\\\b\\\"c\"");
    assert(quote("bad\ncommand").empty());
    uint32_t duration;
    assert(parse_content_duration("Content-Duration: 33\r\n\r\n", &duration) && duration == 33);
    assert(parse_content_duration("content-duration: 0\r\n\r\n", &duration) && duration == 0);
    assert(!parse_content_duration("Subject: no duration\r\n\r\n", &duration));
    assert(!parse_content_duration("Content-Duration: 1.5\r\n\r\n", &duration));
    assert(!parse_content_duration("Content-Duration: 2147483648\r\n\r\n", &duration));
    assert(!parse_content_duration("Content-Duration: 1\r\nContent-Duration: 2\r\n\r\n", &duration));
    assert(mailbox_wire_name("Inbox & Archive") == "Inbox &- Archive");
    assert(mailbox_wire_name("台北") == "&U,BTFw-");
    assert(decode_header("=?UTF-8?B?TcO8bGxlcg==?=") == "Müller");
    assert(decode_header("=?ISO-8859-1?Q?Andr=E9?=") == "André");
    assert(decode_header("=?UTF-8?Q?Voice_?= =?UTF-8?Q?mail?=") == "Voice mail");
    const auto uri = make_message_uri("mail.example.test", 143, "Voice mail", 123, 4294967295U);
    uint32_t validity;
    assert(parse_message_uri(uri, "mail.example.test", 143, "Voice mail", &validity, &uid) && validity == 123 && uid == UINT32_MAX);
    assert(!parse_message_uri(uri, "different.example.test", 143, "Voice mail", &validity, &uid));
    for (size_t chunk = 1; chunk < 40; ++chunk) {
      TransferDecoder decoder("BASE64");
      const std::string encoded = "UklGRgABAgP/\r\nAA==\r\n", expected("RIFF\0\1\2\3\xff\0", 10);
      std::string result;
      auto sink = [&](const uint8_t *data, size_t size) { result.append(reinterpret_cast<const char *>(data), size); return true; };
      for (size_t at = 0; at < encoded.size(); at += chunk)
        assert(decoder.feed(reinterpret_cast<const uint8_t *>(encoded.data() + at), std::min(chunk, encoded.size() - at), sink));
      assert(decoder.finish() && result == expected);
    }
    for (const std::string encoded : {"AA=A", "TR==", "AAA", "AA==x", "@@=="}) {
      TransferDecoder decoder("BASE64");
      auto sink = [](const uint8_t *, size_t) { return true; };
      const bool fed = decoder.feed(reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size(), sink);
      assert(!fed || !decoder.finish());
    }
    TransferDecoder quoted("quoted-printable");
    std::string decoded;
    const std::string input = "a=00b=FF=\r\nc";
    for (const unsigned char c : input)
      assert(quoted.feed(&c, 1, [&](const uint8_t *data, size_t size) { decoded.append(reinterpret_cast<const char *>(data), size); return true; }));
    assert(quoted.finish() && decoded == std::string("a\0b\xff" "c", 5));
    Response response;
    response.text = "* 1 FETCH (UID 7 BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 4 1) (\"AUDIO\" \"X-WAV\" NIL NIL NIL \"BASE64\" 128) \"MIXED\"))";
    std::vector<Value> values;
    assert(parse_values(response, nullptr, &values));
    AudioPart part;
    assert(parse_bodystructure(values[3].children[3], &part));
    assert(part.section == "2" && part.mime == "audio/x-wav" && part.encoding == "BASE64" && part.encoded_size == 128);
    std::mt19937 random(91);
    for (size_t i = 0; i < 5000; ++i) {
      response.text.assign(random() % 200, '\0');
      for (auto &c : response.text) c = static_cast<char>(random());
      parse_values(response, nullptr, &values);
    }
    std::cout << "PASS IMAP framing/bodystructure/UID/MIME boundaries" << '\n';
    return 0;
  }
  assert(argc >= 3);
  TcpStream stream;
  assert(stream.connect("127.0.0.1", static_cast<uint16_t>(std::stoi(argv[2])), 3000));
  uint8_t literal[16384], fetch[16384];
  Client client(stream, literal, sizeof(literal)); client.set_fetch_buffer(fetch, sizeof(fetch));
  if (!client.login("reader", action == "bad-login" ? "incorrect" : "fixture-password", 5000)) {
    std::cout << "ERROR " << client.error() << '\n'; return action == "bad-login" ? 0 : 11;
  }
  if (!client.select("INBOX", 5000)) { std::cout << "ERROR " << client.error() << '\n'; return 12; }
  Mailbox mailbox;
  if (!client.catalogue(64, 0, &mailbox, 5000)) { std::cout << "ERROR " << client.error() << '\n'; return 13; }
  if (action == "idle") {
    assert(client.supports_idle() && client.start_idle(5000));
    std::cout << "IDLE_READY" << std::endl;
    bool changed = false;
    assert(client.wait_idle(5000, &changed) && changed);
    assert(client.end_idle(5000));
    assert(client.select("INBOX", 5000) && client.catalogue(64, 0, &mailbox, 5000));
    std::cout << "UPDATED " << mailbox.total << '\n';
  } else if (action == "fetch") {
    assert(argc == 4 && !mailbox.messages.empty());
    const auto &message = mailbox.messages.front();
    assert(message.audio.valid());
    std::ofstream file(argv[3], std::ios::binary);
    TransferDecoder decoder(message.audio.encoding);
    uint32_t offset = 0;
    for (;;) {
      size_t count;
      if (!client.fetch_audio(message.uid, message.audio, offset, 113, &count, 5000)) {
        std::cout << "ERROR " << client.error() << '\n'; return 14;
      }
      assert(count <= 113 && offset < 1024 * 1024);
      assert(decoder.feed(client.fetched_data(), count, [&](const uint8_t *data, size_t size) { file.write(reinterpret_cast<const char *>(data), size); return bool(file); }));
      offset += static_cast<uint32_t>(count);
      if (count < 113) break;
    }
    assert(decoder.finish());
    std::cout << "FETCHED " << message.uid << ' ' << message.audio.section << ' ' << message.audio.mime << '\n';
  } else if (action == "seen" || action == "unseen" || action == "delete") {
    assert(!mailbox.messages.empty());
    const auto uid = mailbox.messages.front().uid;
    assert(action == "delete" ? client.delete_uid(uid, 5000) : client.store_seen(uid, action == "seen", 5000));
    std::cout << "CHANGED " << uid << '\n';
  } else {
    assert(action == "list");
    std::cout << "MAILBOX " << mailbox.uid_validity << ' ' << mailbox.total << ' ' << mailbox.messages.size() << '\n';
    for (const auto &message : mailbox.messages)
      std::cout << message.uid << '|' << message.seen << '|' << message.subject << '|' << message.from << '|' << message.audio.section << '|' << message.audio.mime << '|' << message.has_duration << '|' << message.duration_seconds << '\n';
  }
  client.logout(1000);
}
