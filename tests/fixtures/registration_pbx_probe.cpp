// Real registrar qualification driver. Only hardware/socket scheduling is
// replaced; REGISTER generation, matching, authentication and timers are real.
#define main registration_fixture_main
#include "registration_engine.cpp"
#undef main
#include "sip_sdp_features.h"
#include <poll.h>

#include <chrono>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
  if (argc != 7) return 64;
  const bool tcp = std::string(argv[1]) == "tcp";
  const std::string host = argv[2];
  const unsigned port = std::strtoul(argv[3], nullptr, 10);
  const std::string username = argv[4], domain = argv[5];
  const unsigned hold = std::strtoul(argv[6], nullptr, 10);
  const char* password = std::getenv("REG_TEST_PASSWORD");
  if (password == nullptr) return 65;
  struct Driver {
    SipRegistration manager;
    int socket{-1};
    bool tcp{false};
    uint32_t ip{0};
    uint16_t port{0};
    std::string local_ip;
  } driver;
  driver.tcp = tcp;
  driver.socket = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_port = htons(port);
  if (inet_aton(host.c_str(), &server.sin_addr) == 0 || driver.socket < 0) return 66;
  driver.ip = ntohl(server.sin_addr.s_addr);
  driver.port = port;
  if (connect(driver.socket, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0) return 67;
  sockaddr_in local{};
  socklen_t local_size = sizeof(local);
  if (getsockname(driver.socket, reinterpret_cast<sockaddr*>(&local), &local_size) != 0) return 68;
  char local_address[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &local.sin_addr, local_address, sizeof(local_address)) == nullptr) return 68;
  driver.local_ip = local_address;
  SipRegistration::Hooks hooks;
  hooks.context = &driver;
  // Each external run is a distinct SIP UA transaction namespace. Reusing the
  // deterministic unit-test branches makes the PBX replay an earlier result.
  hooks.random = []() -> uint32_t {
    static std::random_device random;
    return random();
  };
  hooks.hash = hash_value;
  hooks.local_port = ntohs(local.sin_port);
  hooks.directional_audio_supported = sip_sdp_supports_directional_audio_v1;
  hooks.resolve = [](void* context, size_t, const std::string&) { return static_cast<Driver*>(context)->ip; };
  hooks.local_ip = [](void* context, uint32_t) { return static_cast<Driver*>(context)->local_ip; };
  hooks.send = [](void* context, size_t, std::string_view data, uint32_t, uint16_t, bool) {
    return send(static_cast<Driver*>(context)->socket, data.data(), data.size(), 0) ==
           static_cast<ssize_t>(data.size());
  };
  hooks.notify = [](void*, const std::string& id, RegistrationState state, uint16_t status) {
    std::cout << "{\"account_id\":\"" << id << "\",\"state\":\"" << registration_state_name(state)
              << "\",\"status\":" << status << "}" << std::endl;
  };
  driver.manager.set_hooks(hooks);
  if (const char* keepalive = std::getenv("REG_TEST_KEEPALIVE_SECONDS"))
    driver.manager.set_keepalive_interval(std::strtoul(keepalive, nullptr, 10) * 1000);
  SipAccountConfig config;
  config.id = "pbx-probe";
  config.registrar = host;
  config.domain = domain;
  config.username = username;
  config.password = password;
  config.port = port;
  config.tcp = tcp;
  const char* expires = std::getenv("REG_TEST_EXPIRES");
  config.expires = expires == nullptr ? 60 : std::strtoul(expires, nullptr, 10);
  if (!driver.manager.set_account(config, 0)) return 69;
  const auto started = std::chrono::steady_clock::now();
  bool registered = false, unregistering = false;
  uint32_t registered_at = 0;
  std::string stream;
  while (true) {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto now =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    if (now > 45000 + hold * 1000) {
      close(driver.socket);
      return 70;
    }
    driver.manager.tick(now);
    const auto state = driver.manager.account(0).state;
    if (!registered && state == RegistrationState::REGISTERED) {
      registered = true;
      registered_at = now;
    }
    if (registered && !unregistering && now >= registered_at + hold * 1000) {
      driver.manager.request_registration("pbx-probe", false, now);
      unregistering = true;
      continue;
    }
    if (unregistering && state == RegistrationState::DISABLED) {
      close(driver.socket);
      return 0;
    }
    if (state == RegistrationState::FAILED) {
      close(driver.socket);
      return 71;
    }
    uint32_t wait = 1000;
    driver.manager.next_deadline(now, &wait);
    wait = std::min<uint32_t>(wait, 1000);
    pollfd item{driver.socket, POLLIN, 0};
    if (poll(&item, 1, wait) <= 0) continue;
    char bytes[8192];
    const int count = recv(driver.socket, bytes, sizeof(bytes), 0);
    if (count <= 0) return 72;
    if (!tcp) {
      const std::string message(bytes, count);
      if (message.rfind("OPTIONS ", 0) == 0) {
        const std::string answer = response(message, 200);
        if (send(driver.socket, answer.data(), answer.size(), 0) != static_cast<ssize_t>(answer.size())) return 73;
        std::cout << "{\"incoming_options\":1}" << std::endl;
      } else {
        driver.manager.handle_response(message, driver.ip, driver.port, false, -1, now);
      }
      continue;
    }
    stream.append(bytes, count);
    while (true) {
      const size_t header_end = stream.find("\r\n\r\n");
      if (header_end == std::string::npos) break;
      const size_t content = std::strtoul(field(stream, "Content-Length").c_str(), nullptr, 10);
      const size_t total = header_end + 4 + content;
      if (stream.size() < total) break;
      const std::string message = stream.substr(0, total);
      if (message.rfind("OPTIONS ", 0) == 0) {
        const std::string answer = response(message, 200);
        if (send(driver.socket, answer.data(), answer.size(), 0) != static_cast<ssize_t>(answer.size())) return 73;
        std::cout << "{\"incoming_options\":1}" << std::endl;
      } else {
        driver.manager.handle_response(message, driver.ip, driver.port, true, 0, now);
      }
      stream.erase(0, total);
    }
  }
}
