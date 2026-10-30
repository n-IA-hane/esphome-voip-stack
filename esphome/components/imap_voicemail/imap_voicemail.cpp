#include "imap_voicemail.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <esp_heap_caps.h>
#include <algorithm>

namespace esphome::imap_voicemail {
static const char *const TAG = "imap_voicemail";
using media_source::MediaSourceCommand;
using media_source::MediaSourceState;

void ImapVoicemail::set_server(const std::string &host, uint16_t port, bool tls) {
  this->lock_(); this->config_.host = host; this->config_.port = port; this->config_.tls = tls; this->unlock_();
}
void ImapVoicemail::set_credentials(const std::string &username, const std::string &password) {
  this->lock_(); this->config_.username = username; this->config_.password = password;
  this->config_.credentials_ready = !username.empty() && !password.empty(); this->unlock_();
}

void ImapVoicemail::setup() {
  this->metadata_buffer_ = static_cast<uint8_t *>(allocate_(this->metadata_size_, this->buffers_psram_));
  this->fetch_buffer_ = static_cast<uint8_t *>(allocate_(this->fetch_size_, this->buffers_psram_));
  this->fetch_payload_ = static_cast<uint8_t *>(allocate_(this->fetch_size_, this->buffers_psram_));
  this->audio_ring_ = ring_buffer::RingBuffer::create(this->audio_size_, this->buffers_psram_ ?
      ring_buffer::RingBuffer::MemoryPreference::EXTERNAL_FIRST : ring_buffer::RingBuffer::MemoryPreference::INTERNAL_FIRST);
  if (this->metadata_buffer_ == nullptr || this->fetch_buffer_ == nullptr || this->fetch_payload_ == nullptr || !this->audio_ring_) {
    this->mark_failed(); return;
  }
  if (!this->setup_worker_("imap_metadata")) return;
  this->fetch_stack_ = static_cast<StackType_t *>(allocate_(this->fetch_stack_bytes_, this->stack_psram_));
  if (this->fetch_stack_ == nullptr) { this->mark_failed(); this->on_shutdown(); return; }
  this->fetch_task_ = xTaskCreateStaticPinnedToCore([](void *argument) {
    auto *owner = static_cast<ImapVoicemail *>(argument);
    while (!owner->shutdown_.load()) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      if (!owner->shutdown_.load()) owner->playback_run_();
    }
    vTaskSuspend(nullptr);
  }, "imap_audio", this->fetch_stack_bytes_, this, this->fetch_priority_, this->fetch_stack_, &this->fetch_task_storage_, this->fetch_core_);
  if (this->fetch_task_ == nullptr) { heap_caps_free(this->fetch_stack_); this->fetch_stack_ = nullptr; this->mark_failed(); this->on_shutdown(); return; }
#ifdef USE_IMAP_VOICEMAIL_API
  if (this->managed_by_ha_)
    this->register_service(&ImapVoicemail::configure_voicemail, "configure_voicemail",
        {"server", "port", "folder", "account_id", "tls", "enabled"});
  else
    this->register_service(&ImapVoicemail::configure_server, "configure_imap_voicemail", {"server", "port", "folder", "tls"});
  if (this->account_id_.empty())
    this->register_service(&ImapVoicemail::configure_credentials, "configure_imap_credentials", {"username", "password"});
  this->register_service(&ImapVoicemail::refresh, "refresh_voicemail");
  this->register_service(&ImapVoicemail::native_play, "play_voicemail", {"uid", "uid_validity"});
  this->register_service(&ImapVoicemail::native_seen, "mark_voicemail_read", {"uid", "uid_validity", "read"});
  this->register_service(&ImapVoicemail::native_delete, "delete_voicemail", {"uid", "uid_validity"});
  this->register_service(&ImapVoicemail::native_page, "voicemail_page", {"page"});
#endif
#if defined(USE_IMAP_VOICEMAIL_ACCOUNT) && defined(USE_ESPHOME_VOIP_REGISTRATION)
  if (!this->account_id_.empty()) {
    this->parent_->add_on_account_changed_callback([this](const std::string &id) { if (id == this->account_id_) this->account_changed_(); });
    this->account_changed_();
  }
#endif
  this->ready_ = true;
  this->refresh();
}

void ImapVoicemail::account_changed_() {
#if defined(USE_IMAP_VOICEMAIL_ACCOUNT) && defined(USE_ESPHOME_VOIP_REGISTRATION)
  voip_stack::SipAccountSnapshot snapshot;
  if (!this->parent_->get_account_snapshot(this->account_id_, &snapshot)) {
    this->lock_(); this->config_.credentials_ready = false; this->operation_count_ = 0; this->unlock_();
    this->account_loaded_ = false;
    this->interrupt_worker_(); this->stop_playback_();
    this->mailbox_ = {}; this->status_ = "unconfigured";
    this->updated_.trigger(0, 0, 0); this->changed_.call(); return;
  }
  if (this->account_loaded_ && this->account_revision_ == snapshot.revision) {
    if (snapshot.registered && this->status_ == std::string("error")) this->refresh();
    return;
  }
  this->account_revision_ = snapshot.revision; this->account_loaded_ = true;
  this->lock_(); this->config_.username = snapshot.config.username; this->config_.password = snapshot.config.password;
  this->config_.account_host = snapshot.config.registrar; this->config_.credentials_ready = true; this->unlock_();
  this->interrupt_worker_(); this->stop_playback_();
  if (this->ready_) this->refresh();
#endif
}

void ImapVoicemail::configure_voicemail(std::string server, int32_t port, std::string folder,
                                       std::string account, bool tls, bool enabled) {
  if (!this->managed_by_ha_ || account != "ha" || account != this->account_id_) return;
  if (!enabled) {
    this->lock_(); this->enabled_ = false; this->operation_count_ = 0; this->unlock_();
    this->cancel_timeout("metadata_retry"); this->interrupt_worker_(); this->stop_playback_();
    this->mailbox_ = {}; this->status_ = "disabled"; this->updated_.trigger(0, 0, 0); this->changed_.call(); return;
  }
  this->configure_server(std::move(server), port, std::move(folder), tls);
}

void ImapVoicemail::configure_server(std::string server, int32_t port, std::string folder, bool tls) {
  if (server.empty() || server.size() > 253 || server.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
      port < 1 || port > 65535 || folder.empty() || folder.size() > 256 || mailbox_wire_name(folder).empty()) return;
#ifndef USE_PHONE_CLIENT_TLS
  if (tls) { ESP_LOGW(TAG, "TLS support is not included in this firmware"); return; }
#endif
  this->lock_();
  const bool same = this->enabled_ && this->config_.host == server && this->config_.port == port &&
      this->config_.folder == folder && this->config_.tls == tls;
  if (!same) {
    this->config_.host = server; this->config_.port = static_cast<uint16_t>(port);
    this->config_.folder = folder; this->config_.tls = tls; this->enabled_ = true; this->operation_count_ = 0; this->page_ = 0;
  }
  this->unlock_();
  if (same) { this->refresh_if_stale(); return; }
  this->interrupt_worker_(); this->stop_playback_(); this->mailbox_ = {};
  this->refresh();
}

void ImapVoicemail::configure_credentials(std::string username, std::string password) {
  if (!this->account_id_.empty() || username.empty() || username.size() > 1024 || password.empty() || password.size() > 1024 ||
      quote(username).empty() || quote(password).empty()) return;
  this->set_credentials(username, password); this->interrupt_worker_(); this->stop_playback_(); this->refresh();
}

void ImapVoicemail::refresh() {
  if (!this->ready_ || this->is_failed()) return;
  this->lock_(); const bool configured = this->enabled_ && this->config_.credentials_ready &&
      (!this->config_.host.empty() || !this->config_.account_host.empty()); this->unlock_();
  if (!configured) return;
  this->status_ = "updating";
  this->changed_.call();
  this->cancel_timeout("metadata_retry");
  this->refresh_requested_.store(true); this->intentional_wake_.store(true);
  if (this->metadata_idling_.load()) this->stream_.interrupt();
  this->wake_worker_();
}
void ImapVoicemail::refresh_if_stale() {
  if (this->last_success_ == 0 || millis() - this->last_success_ >= this->refresh_ms_) this->refresh();
}
void ImapVoicemail::set_page(uint32_t page) {
  const uint32_t maximum = this->mailbox_.total == 0 ? 0 : (this->mailbox_.total - 1) / this->max_messages_;
  this->lock_(); this->page_ = std::min(page, maximum); this->unlock_(); this->refresh();
}

bool ImapVoicemail::enqueue_(Operation operation) {
  this->lock_();
  if (!this->enabled_ || !this->config_.credentials_ready || this->operation_count_ == this->operations_.size()) {
    this->unlock_(); return false;
  }
  const size_t at = (this->operation_head_ + this->operation_count_) % this->operations_.size();
  this->operations_[at] = operation; ++this->operation_count_; this->unlock_();
  this->intentional_wake_.store(true);
  if (this->metadata_idling_.load()) this->stream_.interrupt();
  this->wake_worker_(); return true;
}
bool ImapVoicemail::mark_read(uint32_t uid, uint32_t validity, bool read) {
  return uid != 0 && validity != 0 && validity == this->mailbox_.uid_validity &&
      this->enqueue_({read ? OperationKind::SEEN : OperationKind::UNSEEN, uid, validity});
}
bool ImapVoicemail::delete_message(uint32_t uid, uint32_t validity) {
  return uid != 0 && validity != 0 && validity == this->mailbox_.uid_validity &&
      this->enqueue_({OperationKind::DELETE, uid, validity});
}

void ImapVoicemail::worker_run_() {
  this->intentional_wake_.store(false);
  this->metadata_idling_.store(false);
  this->lock_(); Configuration cfg = this->config_; const bool enabled = this->enabled_;
  const uint32_t generation = this->generation_.load(); this->unlock_();
  if (!enabled || !cfg.credentials_ready) return;
  const std::string host = cfg.host.empty() ? cfg.account_host : cfg.host;
  if (!this->stream_.connect(host, cfg.port, this->timeout_ms_, cfg.tls, cfg.ca_pem)) {
    this->publish_error_("connection", generation); return;
  }
  Client client(this->stream_, this->metadata_buffer_, this->metadata_size_);
  client.set_buffer_in_psram(this->buffers_psram_);
  bool ok = generation == this->generation_.load() && client.login(cfg.username, cfg.password, this->timeout_ms_) &&
      generation == this->generation_.load() && client.select(cfg.folder, this->timeout_ms_);
  while (ok && !this->shutdown_.load() && generation == this->generation_.load()) {
    this->refresh_requested_.store(false);
    bool mutated = false;
    for (;;) {
      if (generation != this->generation_.load() || this->shutdown_.load()) { ok = false; break; }
      this->lock_(); const bool pending = this->operation_count_ != 0;
      Operation operation{};
      if (pending) operation = this->operations_[this->operation_head_];
      this->unlock_();
      if (!pending) break;
      const auto consume = [&]() {
        this->lock_();
        if (generation == this->generation_.load() && this->operation_count_ != 0) {
          const auto &head = this->operations_[this->operation_head_];
          if (head.uid == operation.uid && head.validity == operation.validity && head.kind == operation.kind) {
            this->operation_head_ = (this->operation_head_ + 1) % this->operations_.size(); --this->operation_count_;
          }
        }
        this->unlock_();
      };
      if (operation.validity != client.uid_validity()) { consume(); this->publish_error_("uid_validity_changed", generation); continue; }
      const bool changed = operation.kind == OperationKind::DELETE ? client.delete_uid(operation.uid, this->timeout_ms_) :
          client.store_seen(operation.uid, operation.kind == OperationKind::SEEN, this->timeout_ms_);
      if (!changed) {
        const auto &error = client.error();
        // Read/flag/delete commands are idempotent for one UIDVALIDITY. Retain
        // an uncertain transport result for reconnect; do not lose a user
        // operation because another command interrupted IDLE concurrently.
        if (error != "connection" && error != "server_bye") consume();
        this->publish_error_(error, generation); ok = false; break;
      }
      consume();
      mutated = true;
    }
    if (!ok) break;
    if (mutated && !client.select(cfg.folder, this->timeout_ms_)) { ok = false; break; }
    this->intentional_wake_.store(false);
    this->lock_(); const uint32_t page = this->page_; this->unlock_();
    Mailbox mailbox;
    if (!client.catalogue(this->max_messages_, page, &mailbox, this->timeout_ms_)) { ok = false; break; }
    this->publish_catalogue_(std::move(mailbox), generation, client.supports_idle());
    if (!client.supports_idle()) { client.logout(1000); this->stream_.close(); return; }
    // Arm before checking the queue: an operation arriving after this check
    // interrupts the IDLE transport; one arriving earlier stays on this
    // already-authenticated session. No command can be stranded in IDLE.
    ulTaskNotifyTake(pdTRUE, 0);
    this->metadata_idling_.store(true);
    this->lock_(); const bool work_waiting = this->operation_count_ != 0 || this->refresh_requested_.load(); this->unlock_();
    if (work_waiting) { this->metadata_idling_.store(false); continue; }
    if (!client.start_idle(this->timeout_ms_)) { ok = false; break; }
    bool changed = false, received = true;
    const uint64_t renew = phone_client::monotonic_ms() + 29U * 60U * 1000U;
    while (!changed && received && phone_client::monotonic_ms() < renew && generation == this->generation_.load()) {
      received = client.wait_idle(static_cast<uint32_t>(renew - phone_client::monotonic_ms()), &changed);
    }
    if (!client.end_idle(this->timeout_ms_)) { ok = false; break; }
    this->metadata_idling_.store(false);
    if (!client.select(cfg.folder, this->timeout_ms_)) { ok = false; break; }
  }
  this->stream_.close();
  this->metadata_idling_.store(false);
  if (!this->shutdown_.load() && generation == this->generation_.load() && !this->intentional_wake_.exchange(false))
    this->publish_error_(client.error().empty() ? "connection" : client.error(), generation);
}

void ImapVoicemail::publish_catalogue_(Mailbox mailbox, uint32_t generation, bool idle_supported) {
  this->lock_();
  if (generation == this->generation_.load()) {
    this->pending_mailbox_ = std::move(mailbox); this->pending_generation_ = generation;
    this->catalogue_ready_ = true; this->no_idle_ = !idle_supported;
  }
  this->unlock_(); this->notify_main_();
}
void ImapVoicemail::publish_error_(const std::string &error, uint32_t generation, bool retry) {
  this->lock_();
  if (generation == this->generation_.load()) { this->pending_error_ = error; this->pending_generation_ = generation; this->pending_error_retry_ = retry; }
  this->unlock_(); this->notify_main_();
}
void ImapVoicemail::publish_playback_(MediaSourceState state, uint32_t generation) {
  this->lock_();
  if (generation == this->play_generation_.load()) {
    this->pending_playback_state_ = state; this->pending_playback_generation_ = generation; this->playback_ready_ = true;
  }
  this->unlock_(); this->notify_main_();
}

void ImapVoicemail::loop() {
  this->disable_loop();
  this->lock_();
  const bool catalogue = this->catalogue_ready_; this->catalogue_ready_ = false;
  Mailbox mailbox; if (catalogue) mailbox = std::move(this->pending_mailbox_);
  const uint32_t generation = this->pending_generation_; const bool no_idle = this->no_idle_;
  std::string error; error.swap(this->pending_error_); const bool retry = this->pending_error_retry_;
  const bool playback = this->playback_ready_; this->playback_ready_ = false;
  const auto state = this->pending_playback_state_; const uint32_t playback_generation = this->pending_playback_generation_;
  this->unlock_();
  if (generation == this->generation_.load()) {
    if (catalogue) {
      if (this->mailbox_.uid_validity != 0 && this->mailbox_.uid_validity != mailbox.uid_validity) this->stop_playback_();
      this->mailbox_ = std::move(mailbox); this->last_success_ = millis(); this->status_ = "ready";
      this->status_clear_warning(); this->cancel_timeout("metadata_retry");
      this->updated_.trigger(this->mailbox_.total, static_cast<uint32_t>(this->mailbox_.messages.size()), this->mailbox_.uid_validity);
      this->changed_.call();
      if (no_idle) this->Component::set_timeout("metadata_retry", this->refresh_ms_, [this]() { this->refresh(); });
    }
    if (!error.empty()) {
      if (retry) this->status_ = "error";
      this->status_set_warning(); this->error_.trigger(error); this->changed_.call();
      ESP_LOGW(TAG, "IMAP operation failed (%s)", error.c_str());
      if (retry) this->Component::set_timeout("metadata_retry", std::min<uint32_t>(this->refresh_ms_, 30000), [this]() { this->refresh(); });
    }
  }
  if (playback && playback_generation == this->play_generation_.load()) this->set_state_(state);
}

audio::AudioFileType ImapVoicemail::file_type_(const AudioPart &part) {
#ifdef USE_AUDIO_WAV_SUPPORT
  if (part.mime == "audio/wav" || part.mime == "audio/x-wav" || part.mime == "audio/wave") return audio::AudioFileType::WAV;
#endif
#ifdef USE_AUDIO_MP3_SUPPORT
  if (part.mime == "audio/mpeg" || part.mime == "audio/mp3") return audio::AudioFileType::MP3;
#endif
#ifdef USE_AUDIO_OPUS_SUPPORT
  if (part.mime == "audio/ogg" || part.mime == "audio/opus") return audio::AudioFileType::OPUS;
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
  if (part.mime == "audio/flac" || part.mime == "audio/x-flac") return audio::AudioFileType::FLAC;
#endif
  return audio::AudioFileType::NONE;
}
bool ImapVoicemail::can_play(uint32_t uid) const {
  for (const auto &message : this->mailbox_.messages)
    if (message.uid == uid) return file_type_(message.audio) != audio::AudioFileType::NONE;
  return false;
}
bool ImapVoicemail::can_handle(const std::string &uri) const {
  uint32_t validity, uid;
  const auto &cfg = this->config_;
  return parse_message_uri(uri, cfg.host.empty() ? cfg.account_host : cfg.host, cfg.port, cfg.folder, &validity, &uid);
}
bool ImapVoicemail::request_play_uid(uint32_t uid, uint32_t validity) {
#ifdef USE_IMAP_VOICEMAIL_ACCOUNT
  if (this->parent_ != nullptr && !this->parent_->is_idle()) return false;
#endif
  if (validity == 0) validity = this->mailbox_.uid_validity;
  if (!this->has_listener() || validity != this->mailbox_.uid_validity || !this->can_play(uid)) return false;
  const auto &cfg = this->config_;
  this->request_play_uri_(make_message_uri(cfg.host.empty() ? cfg.account_host : cfg.host, cfg.port, cfg.folder, validity, uid));
  return true;
}
bool ImapVoicemail::play_uri(const std::string &uri) {
#ifdef USE_IMAP_VOICEMAIL_ACCOUNT
  // The native player defers URI requests. Recheck admission when the queued
  // request reaches this source, after any intervening telephone transition.
  if (this->parent_ != nullptr && !this->parent_->is_idle()) return false;
#endif
  uint32_t validity, uid;
  if (!this->ready_ || this->is_failed() || !this->enabled_ || !this->has_listener() ||
      !parse_message_uri(uri, this->config_.host.empty() ? this->config_.account_host : this->config_.host,
                         this->config_.port, this->config_.folder, &validity, &uid) || validity != this->mailbox_.uid_validity) return false;
  const auto found = std::find_if(this->mailbox_.messages.begin(), this->mailbox_.messages.end(), [=](const Message &message) { return message.uid == uid; });
  if (found == this->mailbox_.messages.end() || file_type_(found->audio) == audio::AudioFileType::NONE) return false;
  this->stop_playback_(false);
  this->lock_(); this->play_message_ = *found; this->play_config_ = this->config_; this->play_requested_ = true; this->unlock_();
  this->paused_.store(false); this->set_state_(MediaSourceState::PLAYING); xTaskNotifyGive(this->fetch_task_); return true;
}

void ImapVoicemail::stop_playback_(bool publish_idle) {
  const uint32_t generation = ++this->play_generation_;
  this->lock_(); this->play_requested_ = false; this->unlock_();
  this->paused_.store(false); this->fetch_stream_.interrupt();
  if (this->fetch_task_ != nullptr) xTaskNotifyGive(this->fetch_task_);
  if (publish_idle) this->publish_playback_(MediaSourceState::IDLE, generation);
}
void ImapVoicemail::handle_command(MediaSourceCommand command) {
  if (command == MediaSourceCommand::STOP) { this->stop_playback_(); return; }
  if (command == MediaSourceCommand::PAUSE) {
    if (this->get_state() == MediaSourceState::PLAYING) { this->paused_.store(true); this->set_state_(MediaSourceState::PAUSED); }
    return;
  }
  if (command == MediaSourceCommand::PLAY) {
    if (this->paused_.exchange(false)) { this->set_state_(MediaSourceState::PLAYING); xTaskNotifyGive(this->fetch_task_); }
    return;
  }
  if (command == MediaSourceCommand::NEXT || command == MediaSourceCommand::PREVIOUS) {
    for (size_t i = 0; i < this->mailbox_.messages.size(); ++i) if (this->mailbox_.messages[i].uid == this->play_message_.uid) {
      const size_t next = command == MediaSourceCommand::NEXT ? i + 1 : i == 0 ? this->mailbox_.messages.size() : i - 1;
      if (next < this->mailbox_.messages.size()) this->request_play_uid(this->mailbox_.messages[next].uid);
      return;
    }
  }
}

bool ImapVoicemail::wait_playback_() {
  while (this->paused_.load() && this->running_play_generation_ == this->play_generation_.load() && !this->shutdown_.load())
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return this->running_play_generation_ == this->play_generation_.load() && !this->shutdown_.load();
}
bool ImapVoicemail::decode_(bool finished) {
  if (!this->wait_playback_() || !this->decoder_) return false;
  return this->decoder_->decode(finished) != audio::AudioDecoderState::FAILED;
}
bool ImapVoicemail::feed_audio_(const uint8_t *data, size_t length) {
  while (length != 0) {
    if (!this->wait_playback_()) return false;
    const size_t count = std::min(length, this->audio_ring_->free());
    if (count != 0) {
      const size_t written = this->audio_ring_->write_without_replacement(data, count, 0, true);
      data += written; length -= written;
    }
    if (!this->decode_(false)) return false;
  }
  return true;
}
size_t ImapVoicemail::audio_sink_write(uint8_t *data, size_t length, TickType_t wait) {
  if (!this->decoder_ || this->paused_.load() || this->running_play_generation_ != this->play_generation_.load()) return 0;
  const auto &info = this->decoder_->get_audio_stream_info();
  if (!info.has_value()) return 0;
  const size_t written = this->write_output(data, length, static_cast<uint32_t>(wait) * portTICK_PERIOD_MS, *info);
  if (written == 0 && wait != 0 && this->running_play_generation_ == this->play_generation_.load())
    ulTaskNotifyTake(pdTRUE, wait);  // DMA completion, pause/resume, or STOP wakes us.
  if (written != 0 && this->mark_read_on_play_ && !this->seen_sent_) {
    this->seen_sent_ = true;
    this->enqueue_({OperationKind::SEEN, this->running_uid_, this->running_uid_validity_});
  }
  return written;
}

void ImapVoicemail::playback_run_() {
  this->lock_();
  if (!this->play_requested_) { this->unlock_(); return; }
  this->play_requested_ = false;
  const Message message = this->play_message_; const Configuration cfg = this->play_config_;
  this->running_play_generation_ = this->play_generation_.load(); const uint32_t generation = this->running_play_generation_;
  this->running_config_generation_ = this->generation_.load();
  this->running_uid_ = message.uid; this->running_uid_validity_ = message.uid_validity;
  this->unlock_();
  this->seen_sent_ = false; this->audio_ring_->reset();
  // AudioDecoder owns format-specific state and output transfers. Construct
  // a fresh public decoder per file so STOP never leaks an earlier file's
  // partial header/output. Network/ring/task storage stays boot-owned.
  this->decoder_ = make_unique<audio::AudioDecoder>(this->audio_size_, this->decoder_output_size_);
  std::weak_ptr<ring_buffer::RingBuffer> input = this->audio_ring_;
  bool ok = this->decoder_->add_source(input) == ESP_OK && this->decoder_->add_sink(this) == ESP_OK &&
      this->decoder_->start(file_type_(message.audio)) == ESP_OK;
  const std::string host = cfg.host.empty() ? cfg.account_host : cfg.host;
  ok = ok && this->fetch_stream_.connect(host, cfg.port, this->timeout_ms_, cfg.tls, cfg.ca_pem) && this->wait_playback_();
  Client client(this->fetch_stream_, this->fetch_buffer_, this->fetch_size_);
  client.set_fetch_buffer(this->fetch_payload_, this->fetch_size_);
  ok = ok && client.login(cfg.username, cfg.password, this->timeout_ms_) && this->wait_playback_() &&
      client.select(cfg.folder, this->timeout_ms_) && client.uid_validity() == message.uid_validity;
  TransferDecoder transfer(message.audio.encoding);
  uint32_t offset = 0;
  while (ok && this->wait_playback_()) {
    size_t count = 0;
    const size_t request = offset < this->max_audio_bytes_ ? std::min<size_t>(this->fetch_size_, this->max_audio_bytes_ - offset) : 1;
    if (!client.fetch_audio(message.uid, message.audio, offset, request, &count, this->timeout_ms_) ||
        (offset >= this->max_audio_bytes_ && count != 0)) { ok = false; break; }
    if (!this->wait_playback_() || !transfer.feed(client.fetched_data(), count, [this](const uint8_t *data, size_t length) { return this->feed_audio_(data, length); })) { ok = false; break; }
    offset += static_cast<uint32_t>(count);
    if (count < request) { ok = transfer.finish(); break; }
  }
  if (ok && this->wait_playback_()) {
    while (this->wait_playback_()) {
      const auto state = this->decoder_->decode(true);
      if (state == audio::AudioDecoderState::FINISHED) break;
      if (state == audio::AudioDecoderState::FAILED) { ok = false; break; }
    }
  }
  this->fetch_stream_.close(); this->decoder_.reset();
  if (generation == this->play_generation_.load()) {
    if (!ok) this->publish_error_(client.error().empty() ? "audio_decode" : client.error(), this->running_config_generation_, false);
    this->publish_playback_(ok ? MediaSourceState::IDLE : MediaSourceState::ERROR, generation);
  }
}

void ImapVoicemail::native_play(std::string uid, std::string validity) { uint32_t u, v; if (parse_uid(uid, &u) && parse_uid(validity, &v)) this->request_play_uid(u, v); }
void ImapVoicemail::native_seen(std::string uid, std::string validity, bool seen) { uint32_t u, v; if (parse_uid(uid, &u) && parse_uid(validity, &v)) this->mark_read(u, v, seen); }
void ImapVoicemail::native_delete(std::string uid, std::string validity) { uint32_t u, v; if (parse_uid(uid, &u) && parse_uid(validity, &v)) this->delete_message(u, v); }
void ImapVoicemail::native_page(int32_t page) { if (page >= 0) this->set_page(static_cast<uint32_t>(page)); }
void ImapVoicemail::on_shutdown() {
  WorkerComponent::on_shutdown(); this->fetch_stream_.interrupt(); ++this->play_generation_;
  if (this->fetch_task_ != nullptr) xTaskNotifyGive(this->fetch_task_);
}
void ImapVoicemail::dump_config() {
  ESP_LOGCONFIG(TAG, "IMAP voicemail: %s, port %u, TLS %s, catalogue capacity %u", this->status_, this->config_.port,
                this->config_.tls ? "on" : "off", this->max_messages_);
}
}  // namespace esphome::imap_voicemail
