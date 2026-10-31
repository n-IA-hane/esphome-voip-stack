#include "sip_registration.h"
#ifdef USE_ESPHOME_VOIP_REGISTRATION

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

#include "sip_header_name.h"

namespace esphome::voip_stack {
namespace {
bool reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }
std::string lower(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
std::string trim(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t");
  return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
bool safe(const std::string& s, size_t max, bool empty = false) {
  if ((!empty && s.empty()) || s.size() > max) return false;
  for (unsigned char c : s)
    if (c < 32 || c == 127) return false;
  return true;
}
bool token_safe(const std::string& s, size_t max, bool empty = false) {
  return safe(s, max, empty) && s.find_first_of(" \t<>\"\\;,@/?#:") == std::string::npos;
}
std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + '"';
}
std::string header(const std::string& message, const std::string& name) {
  size_t pos = message.find("\r\n");
  while (pos != std::string::npos) {
    const size_t next = message.find("\r\n", pos + 2);
    if (next == std::string::npos || next == pos + 2) break;
    const size_t colon = message.find(':', pos + 2);
    if (colon < next && sip_header_name_equal(std::string_view(message).substr(pos + 2, colon - pos - 2), name))
      return trim(message.substr(colon + 1, next - colon - 1));
    pos = next;
  }
  return "";
}
bool decimal(const std::string& s, uint32_t* out) {
  if (s.empty()) return false;
  uint32_t value = 0;
  for (unsigned char c : s) {
    if (c < '0' || c > '9' || value > (std::numeric_limits<uint32_t>::max() - (c - '0')) / 10) return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}
// Quoted commas and escaped quotes occur in Digest qop/opaque values.
std::string parameter(const std::string& source, const std::string& wanted) {
  size_t pos = 0;
  while (pos < source.size()) {
    while (pos < source.size() && (source[pos] == ',' || source[pos] == ';' || source[pos] == ' ')) ++pos;
    const size_t eq = source.find('=', pos);
    if (eq == std::string::npos) break;
    const std::string key = lower(trim(source.substr(pos, eq - pos)));
    pos = eq + 1;
    while (pos < source.size() && source[pos] == ' ') ++pos;
    std::string value;
    if (pos < source.size() && source[pos] == '"') {
      ++pos;
      bool closed = false;
      while (pos < source.size()) {
        char c = source[pos++];
        if (c == '"') {
          closed = true;
          break;
        }
        if (c == '\\' && pos < source.size()) c = source[pos++];
        value += c;
      }
      if (!closed) return "";
    } else {
      const size_t end = source.find_first_of(",;", pos);
      value = trim(source.substr(pos, end == std::string::npos ? end : end - pos));
      pos = end == std::string::npos ? source.size() : end;
    }
    if (key == wanted) return value;
    while (pos < source.size() && source[pos] != ',' && source[pos] != ';') ++pos;
  }
  return "";
}
std::string uri(const SipAccountConfig& c) { return "sip:" + c.username + "@" + c.domain; }

bool contact_uri_equal(const std::string& left, const std::string& right) {
  // RFC 3261 19.1.4 preserves an explicit port/transport even at default values.
  auto split = [](const std::string& value, std::string* user, std::string* authority, std::string* params) {
    if (lower(value.substr(0, 4)) != "sip:" || value.find('?') != std::string::npos) return false;
    const auto at = value.find('@', 4);
    if (at == std::string::npos) return false;
    const auto semicolon = value.find(';', at);
    *user = value.substr(4, at - 4);
    *authority = lower(value.substr(at + 1, semicolon == std::string::npos ? semicolon : semicolon - at - 1));
    *params = semicolon == std::string::npos ? "" : value.substr(semicolon + 1);
    return true;
  };
  std::string lu, la, lp, ru, ra, rp;
  if (!split(left, &lu, &la, &lp) || !split(right, &ru, &ra, &rp) || lu != ru || la != ra) return false;
  for (const char* key : {"transport", "user", "ttl", "method", "maddr"})
    if (lower(parameter(lp, key)) != lower(parameter(rp, key))) return false;
  return true;
}

bool find_registered_contact(const std::string& message, const std::string& expected, std::string* expires) {
  size_t line = message.find("\r\n");
  while (line != std::string::npos) {
    const size_t end = message.find("\r\n", line + 2);
    if (end == std::string::npos || end == line + 2) break;
    const size_t colon = message.find(':', line + 2);
    if (colon < end && sip_header_name_equal(std::string_view(message).substr(line + 2, colon - line - 2), "Contact")) {
      const std::string values = message.substr(colon + 1, end - colon - 1);
      size_t first = 0;
      bool quoted = false, angle = false, escaped = false;
      for (size_t p = 0; p <= values.size(); ++p) {
        const char c = p == values.size() ? ',' : values[p];
        if (escaped) {
          escaped = false;
          continue;
        }
        if (quoted && c == '\\') {
          escaped = true;
          continue;
        }
        if (c == '"') quoted = !quoted;
        if (!quoted && c == '<') angle = true;
        if (!quoted && c == '>') angle = false;
        if (c != ',' || quoted || angle) continue;
        const std::string item = trim(values.substr(first, p - first));
        first = p + 1;
        const size_t open = item.find('<'), close = item.find('>');
        if (open == std::string::npos || close == std::string::npos || close <= open) continue;
        if (contact_uri_equal(item.substr(open + 1, close - open - 1), expected)) {
          *expires = parameter(item.substr(close + 1), "expires");
          return true;
        }
      }
    }
    line = end;
  }
  return false;
}
}  // namespace

bool SipAccountConfig::valid() const {
  for (unsigned char c : this->id)
    if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
  return this->id != "direct" && token_safe(this->id, 32) && token_safe(this->registrar, 253) &&
         token_safe(this->domain, 253) && token_safe(this->username, 64) && safe(this->password, 128, true) &&
         safe(this->auth_username, 64, true) && token_safe(this->proxy, 253, true) && this->port != 0 &&
         this->proxy_port != 0 && this->expires >= 30 && this->expires <= 86400;
}

const char* registration_state_name(RegistrationState s) {
  switch (s) {
    case RegistrationState::DISABLED:
      return "disabled";
    case RegistrationState::REGISTERING:
      return "registering";
    case RegistrationState::REGISTERED:
      return "registered";
    case RegistrationState::UNREGISTERING:
      return "unregistering";
    case RegistrationState::RETRY:
      return "retry";
    default:
      return "failed";
  }
}

bool SipDigest::parse(const std::string& value, bool proxy_auth) {
  if (value.size() > 2048 || lower(value.substr(0, 7)) != "digest ") return false;
  SipDigest next;
  const std::string fields = value.substr(7);
  next.realm = parameter(fields, "realm");
  next.nonce = parameter(fields, "nonce");
  next.opaque = parameter(fields, "opaque");
  next.algorithm = parameter(fields, "algorithm");
  if (next.algorithm.empty()) next.algorithm = "MD5";
  const std::string alg = lower(next.algorithm);
  if (alg == "md5")
    next.algorithm = "MD5";
  else if (alg == "md5-sess")
    next.algorithm = "MD5-sess";
  else if (alg == "sha-256")
    next.algorithm = "SHA-256";
  else if (alg == "sha-256-sess")
    next.algorithm = "SHA-256-sess";
  else
    return false;
  const std::string qops = lower(parameter(fields, "qop"));
  if (!qops.empty()) {
    size_t pos = 0;
    bool auth = false;
    while (pos <= qops.size()) {
      const size_t end = qops.find(',', pos);
      if (trim(qops.substr(pos, end == std::string::npos ? end : end - pos)) == "auth") auth = true;
      if (end == std::string::npos) break;
      pos = end + 1;
    }
    if (!auth) return false;
    next.qop = "auth";
  }
  if (!safe(next.realm, 256, true) || !safe(next.nonce, 512) || !safe(next.opaque, 512, true)) return false;
  next.proxy = proxy_auth;
  next.stale = lower(parameter(fields, "stale")) == "true";
  if (next.nonce == this->nonce) next.nonce_count = this->nonce_count;
  *this = std::move(next);
  return true;
}
bool SipDigest::parse_response(const std::string& message, bool proxy_auth) {
  const std::string wanted = proxy_auth ? "proxy-authenticate" : "www-authenticate";
  size_t pos = message.find("\r\n");
  bool found = false;
  SipDigest best;
  while (pos != std::string::npos) {
    const size_t next = message.find("\r\n", pos + 2);
    if (next == std::string::npos || next == pos + 2) break;
    const size_t colon = message.find(':', pos + 2);
    if (colon < next && lower(message.substr(pos + 2, colon - pos - 2)) == wanted) {
      SipDigest candidate;
      if (candidate.parse(trim(message.substr(colon + 1, next - colon - 1)), proxy_auth) &&
          (!found || (candidate.algorithm.find("SHA-256") == 0 && best.algorithm.find("SHA-256") != 0))) {
        best = std::move(candidate);
        found = true;
      }
    }
    pos = next;
  }
  if (found) {
    if (best.nonce == this->nonce) best.nonce_count = this->nonce_count;
    *this = std::move(best);
  }
  return found;
}

std::string SipDigest::authorization(const SipAccountConfig& c, const std::string& method,
                                     const std::string& target, const std::string& cnonce, Hash hash) {
  if (this->empty() || hash == nullptr || !safe(target, 1024)) return "";
  const std::string user = c.auth_username.empty() ? c.username : c.auth_username;
  const bool sess = this->algorithm.find("-sess") != std::string::npos;
  const std::string algorithm = this->algorithm.substr(0, this->algorithm.find("-sess"));
  std::string ha1 = hash(algorithm, user + ":" + this->realm + ":" + c.password);
  if (ha1.empty()) return "";
  if (sess) ha1 = hash(algorithm, ha1 + ":" + this->nonce + ":" + cnonce);
  const std::string ha2 = hash(algorithm, method + ":" + target);
  char nc[9];
  std::snprintf(nc, sizeof(nc), "%08x", static_cast<unsigned>(++this->nonce_count));
  const std::string response =
      hash(algorithm, ha1 + ":" + this->nonce + ":" +
                          (this->qop.empty() ? "" : std::string(nc) + ":" + cnonce + ":auth:") + ha2);
  if (response.empty()) return "";
  std::string result = this->proxy ? "Proxy-Authorization: Digest " : "Authorization: Digest ";
  result += "username=" + quote(user) + ", realm=" + quote(this->realm) + ", nonce=" + quote(this->nonce) +
            ", uri=" + quote(target) + ", response=" + quote(response) + ", algorithm=" + this->algorithm;
  if (!this->opaque.empty()) result += ", opaque=" + quote(this->opaque);
  if (!this->qop.empty())
    result += ", qop=auth, nc=" + std::string(nc) + ", cnonce=" + quote(cnonce);
  else if (sess)
    result += ", cnonce=" + quote(cnonce);
  return result + "\r\n";
}

int SipRegistration::find(const std::string& id) const {
  for (size_t i = 0; i < CAPACITY; ++i)
    if (this->accounts_[i].occupied && this->accounts_[i].config.id == id) return i;
  return -1;
}
std::string SipRegistration::token_() {
  return std::to_string(this->hooks_.random == nullptr ? 1 : this->hooks_.random()) + "-" +
         std::to_string(this->hooks_.random == nullptr ? 1 : this->hooks_.random());
}
bool SipRegistration::set_account(const SipAccountConfig& c, uint32_t now) {
  if (!c.valid()) return false;
  int i = this->find(c.id);
  if (i < 0)
    for (size_t slot = 0; slot < CAPACITY; ++slot)
      if (!this->accounts_[slot].occupied) {
        i = slot;
        break;
      }
  if (i < 0) return false;
  auto& a = this->accounts_[i];
  const uint32_t revision = ++this->next_config_revision_;
  a = {};
  a.config_revision = revision;
  a.config = c;
  a.occupied = true;
  a.desired = c.enabled;
  a.call_id = "register-" + this->token_();
  a.tag = this->token_();
  a.next_ms = now;
  a.scheduled = a.desired;
  return true;
}
bool SipRegistration::remove_account(const std::string& id) {
  const int i = this->find(id);
  if (i < 0) return false;
  const uint32_t revision = ++this->next_config_revision_;
  this->accounts_[i] = {};
  this->accounts_[i].config_revision = revision;
  return true;
}
bool SipRegistration::request_registration(const std::string& id, bool on, uint32_t now) {
  const int i = this->find(id);
  if (i < 0) return false;
  auto& a = this->accounts_[i];
  // Do not abandon an in-flight non-INVITE transaction. Apply the intent when
  // its final response arrives, maintaining monotonic CSeq on this Call-ID.
  a.desired = on;
  if (!a.pending) {
    a.scheduled = true;
    a.next_ms = now;
    a.auth_attempts = 0;
    a.proxy_auth_attempts = 0;
  }
  return true;
}
bool SipRegistration::is_registered(const std::string& id) const {
  const int i = this->find(id);
  return i >= 0 && this->accounts_[i].bound && this->accounts_[i].desired;
}
void SipRegistration::state_(size_t i, RegistrationState state, uint16_t status) {
  auto& a = this->accounts_[i];
  const bool changed = a.state != state || a.last_status != status;
  a.state = state;
  a.last_status = status;
  if (changed && this->hooks_.notify != nullptr)
    this->hooks_.notify(this->hooks_.context, a.config.id, state, status);
}
void SipRegistration::retry_(size_t i, uint16_t status, uint32_t now, uint32_t delay_ms) {
  auto& a = this->accounts_[i];
  a.pending = false;
  a.request.clear();
  a.auth_attempts = 0;
  a.proxy_auth_attempts = 0;
  a.failures = std::min<uint8_t>(a.failures + 1, 6);
  a.next_ms = now + (delay_ms ? delay_ms : std::min<uint32_t>(30000U << (a.failures - 1), 900000));
  a.scheduled = a.desired;
  this->state_(i, a.desired ? RegistrationState::RETRY : RegistrationState::DISABLED, status);
}
bool SipRegistration::begin_(size_t i, uint32_t now) {
  auto& a = this->accounts_[i];
  const auto& c = a.config;
  if (this->hooks_.send == nullptr || this->hooks_.resolve == nullptr || this->hooks_.local_ip == nullptr)
    return false;
  if (a.target_ip == 0) {
    a.target_ip = this->hooks_.resolve(this->hooks_.context, i, c.proxy.empty() ? c.registrar : c.proxy);
    a.target_port = c.proxy.empty() ? c.port : c.proxy_port;
  }
  if (a.target_ip == RESOLUTION_PENDING) {
    a.target_ip = 0;
    a.resolving = true;
    a.scheduled = false;
    return false;
  }
  if (a.target_ip == 0) {
    this->retry_(i, 503, now);
    return false;
  }
  const std::string local_ip = this->hooks_.local_ip(this->hooks_.context, a.target_ip);
  if (local_ip.empty() || local_ip == "0.0.0.0") {
    this->retry_(i, 503, now);
    return false;
  }
  a.contact = "sip:esphome-" + c.id + "@" + local_ip + ":" + std::to_string(this->hooks_.local_port) +
              ";transport=" + (c.tcp ? "tcp" : "udp");
  a.branch = "z9hG4bK" + this->token_();
  ++a.cseq;
  a.requested_expires = a.desired ? c.expires : 0;
  const std::string registrar_uri = "sip:" + c.registrar + ":" + std::to_string(c.port);
  std::string message =
      "REGISTER " + registrar_uri + " SIP/2.0\r\nVia: SIP/2.0/" + (c.tcp ? "TCP" : "UDP") + " " + local_ip +
      ":" + std::to_string(this->hooks_.local_port) + ";branch=" + a.branch + ";rport\r\n" +
      "Max-Forwards: 70\r\nFrom: <" + uri(c) + ">;tag=" + a.tag + "\r\nTo: <" + uri(c) + ">\r\n" +
      "Call-ID: " + a.call_id + "\r\nCSeq: " + std::to_string(a.cseq) + " REGISTER\r\n" + "Contact: <" +
      a.contact + ">;expires=" + std::to_string(a.desired ? c.expires : 0) + "\r\n" +
      "Expires: " + std::to_string(a.desired ? c.expires : 0) + "\r\nUser-Agent: ESPHome-VoIP-Stack\r\n";
  for (auto* digest : {&a.digest, &a.proxy_digest}) {
    if (digest->empty()) continue;
    const std::string authorization =
        digest->authorization(c, "REGISTER", registrar_uri, this->token_(), this->hooks_.hash);
    if (authorization.empty()) {
      this->retry_(i, 401, now);
      return false;
    }
    message += authorization;
  }
  message += "Content-Length: 0\r\n\r\n";
  a.request = std::move(message);
  a.interval_ms = 500;
  a.next_ms = now + 500;
  a.deadline_ms = now + 32000;
  a.pending = true;
  a.scheduled = false;
  if (!a.desired || !a.bound)
    this->state_(i, a.desired ? RegistrationState::REGISTERING : RegistrationState::UNREGISTERING, 0);
  if (!this->hooks_.send(this->hooks_.context, i, a.request, a.target_ip, a.target_port, c.tcp)) {
    // A record rejected by the transport is not an outstanding transaction.
    // RFC 3261 section 17.1.2.2 terminates on transport error, without Timer F.
    this->retry_(i, 503, now);
    return false;
  }
  return true;
}
void SipRegistration::tick(uint32_t now) {
  for (size_t i = 0; i < CAPACITY; ++i) {
    auto& a = this->accounts_[i];
    if (!a.occupied) continue;
    if (a.bound && reached(now, a.binding_expires_ms)) {
      a.bound = false;
      if (a.desired && a.pending) this->state_(i, RegistrationState::REGISTERING, 0);
    }
    if (a.pending) {
      if (reached(now, a.deadline_ms)) {
        this->retry_(i, 408, now);
        continue;
      }
      if (!a.config.tcp && reached(now, a.next_ms)) {
        this->hooks_.send(this->hooks_.context, i, a.request, a.target_ip, a.target_port, false);
        a.interval_ms = std::min<uint32_t>(a.interval_ms * 2, 4000);
        a.next_ms = now + a.interval_ms;
      }
    } else if (a.scheduled && reached(now, a.next_ms))
      this->begin_(i, now);
  }
}
bool SipRegistration::next_deadline(uint32_t now, uint32_t* remaining) const {
  bool found = false;
  uint32_t best = 0;
  for (const auto& a : this->accounts_) {
    if (!a.occupied) continue;
    if (a.bound) {
      const uint32_t expiry = reached(now, a.binding_expires_ms) ? 0 : a.binding_expires_ms - now;
      if (!found || expiry < best) best = expiry;
      found = true;
    }
    if (!a.pending && !a.scheduled) continue;
    const uint32_t deadline = a.pending && a.config.tcp ? a.deadline_ms : a.next_ms;
    uint32_t delta = reached(now, deadline) ? 0 : deadline - now;
    if (a.pending) delta = std::min(delta, reached(now, a.deadline_ms) ? 0U : a.deadline_ms - now);
    if (!found || delta < best) best = delta;
    found = true;
  }
  if (found) *remaining = best;
  return found;
}
bool SipRegistration::handle_response(const std::string& message, uint32_t ip, uint16_t port, bool tcp,
                                      int flow_index, uint32_t now) {
  if (message.rfind("SIP/2.0 ", 0) != 0 || header(message, "CSeq").find(" REGISTER") == std::string::npos)
    return false;
  const std::string id = header(message, "Call-ID");
  for (size_t i = 0; i < CAPACITY; ++i) {
    auto& a = this->accounts_[i];
    if (!a.occupied || id != a.call_id) continue;
    if (!a.pending || a.target_ip != ip || a.target_port != port || a.config.tcp != tcp ||
        (tcp && flow_index != static_cast<int>(i)) ||
        header(message, "CSeq") != std::to_string(a.cseq) + " REGISTER" ||
        parameter(header(message, "Via").substr(header(message, "Via").find(';') + 1), "branch") != a.branch)
      return true;
    uint32_t status = 0;
    if (message.size() < 12 || !decimal(message.substr(8, 3), &status) || status < 100 || status > 699)
      return true;
    if (status < 200) {
      a.next_ms = now + 4000;
      a.interval_ms = 4000;
      return true;
    }
    a.pending = false;
    a.request.clear();
    if (status == 401 || status == 407) {
      SipDigest challenge;
      uint8_t& attempts = status == 401 ? a.auth_attempts : a.proxy_auth_attempts;
      if (!challenge.parse_response(message, status == 407) || (++attempts > (challenge.stale ? 3 : 2))) {
        a.scheduled = false;
        this->state_(i, RegistrationState::FAILED, status);
        return true;
      }
      (status == 401 ? a.digest : a.proxy_digest) = std::move(challenge);
      this->begin_(i, now);
      return true;
    }
    if (status == 423) {
      uint32_t minimum = 0;
      if (decimal(header(message, "Min-Expires"), &minimum) && minimum > a.config.expires &&
          minimum <= 86400) {
        a.config.expires = minimum;
        this->begin_(i, now);
        return true;
      }
    }
    if (status >= 200 && status < 300) {
      uint32_t expires = a.requested_expires;
      // Require our Contact in a successful registration response. A generic
      // Expires header must not accidentally acknowledge another binding.
      std::string contact_expires;
      const bool own_contact = find_registered_contact(message, a.contact, &contact_expires);
      if (a.requested_expires > 0 && !own_contact) {
        this->retry_(i, 502, now);
        return true;
      }
      if (own_contact) {
        const std::string& value = contact_expires;
        if (!value.empty()) {
          if (!decimal(value, &expires)) {
            this->retry_(i, 502, now);
            return true;
          }
        } else {
          const std::string common = header(message, "Expires");
          if (!common.empty() && !decimal(common, &expires)) {
            this->retry_(i, 502, now);
            return true;
          }
        }
      }
      if (expires > 86400) expires = 86400;
      a.negotiated_expires = expires;
      a.failures = 0;
      a.auth_attempts = 0;
      a.proxy_auth_attempts = 0;
      a.bound = expires > 0;
      a.binding_expires_ms = now + expires * 1000;
      a.binding_ip = a.target_ip;
      if (a.desired != (a.requested_expires > 0)) {
        a.scheduled = true;
        a.next_ms = now;
        this->state_(i, expires > 0 ? RegistrationState::REGISTERED : RegistrationState::DISABLED, status);
      } else if (a.desired && expires > 0) {
        this->state_(i, RegistrationState::REGISTERED, status);
        const uint32_t jitter =
            this->hooks_.random == nullptr ? 0 : this->hooks_.random() % (expires * 50 + 1);
        a.next_ms = now + std::max<uint32_t>(1000, expires * 800 - jitter);
        a.scheduled = true;
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
        if (this->hooks_.binding_accepted != nullptr)
          this->hooks_.binding_accepted(this->hooks_.context, i, now);
#endif
      } else {
        this->state_(i, RegistrationState::DISABLED, status);
        a.scheduled = a.desired;
        a.next_ms = now + 1000;
      }
      return true;
    }
    uint32_t retry = 0;
    decimal(header(message, "Retry-After"), &retry);
    if (status == 403 || status == 404) {
      a.scheduled = false;
      this->state_(i, RegistrationState::FAILED, status);
    } else
      this->retry_(i, status, now, std::min<uint32_t>(retry, 86400) * 1000);
    return true;
  }
  return true;
}
void SipRegistration::connection_lost(size_t i, uint32_t now) {
  if (i >= CAPACITY || !this->accounts_[i].occupied) return;
  this->accounts_[i].target_ip = 0;
  this->retry_(i, 503, now);
}
void SipRegistration::resolution_complete(size_t i, uint32_t ip, uint32_t now, bool obsolete) {
  if (i >= CAPACITY || !this->accounts_[i].occupied || !this->accounts_[i].resolving) return;
  auto& a = this->accounts_[i];
  a.resolving = false;
  if (!obsolete && ip == 0) {
    this->retry_(i, 503, now);
    return;
  }
  a.target_ip = obsolete ? 0 : ip;
  a.scheduled = true;
  a.next_ms = now;
}
int SipRegistration::match_incoming(const std::string& user, uint32_t ip, bool tcp, int flow_index) const {
  for (size_t i = 0; i < CAPACITY; ++i) {
    const auto& a = this->accounts_[i];
    if (a.occupied && a.desired && a.bound && ("esphome-" + a.config.id) == user && a.binding_ip == ip &&
        a.config.tcp == tcp && (!tcp || flow_index < 0 || flow_index == static_cast<int>(i)))
      return i;
  }
  return -1;
}
}  // namespace esphome::voip_stack
#endif
