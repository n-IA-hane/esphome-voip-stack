#pragma once

#include "esphome/components/phone_client/worker.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/core/automation.h"
#include "imap_protocol.h"
#ifdef USE_IMAP_VOICEMAIL_ACCOUNT
#include "esphome/components/voip_stack/voip_stack.h"
#endif
#ifdef USE_IMAP_VOICEMAIL_API
#include "esphome/components/api/custom_api_device.h"
#endif

namespace esphome::imap_voicemail {

class ImapVoicemail : public phone_client::WorkerComponent, public media_source::MediaSource,
                      public audio::AudioSinkCallback
#ifdef USE_IMAP_VOICEMAIL_API
    , public api::CustomAPIDevice
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
#ifdef USE_IMAP_VOICEMAIL_ACCOUNT
  void set_parent(voip_stack::VoipStack *parent) { this->parent_ = parent; }
#endif
  void set_account_id(const std::string &value) { this->account_id_ = value; }
  void set_managed_by_ha(bool value) { this->managed_by_ha_ = value; this->enabled_ = !value; }
  void set_server(const std::string &host, uint16_t port, bool tls);
  void set_folder(const std::string &folder) { this->config_.folder = folder; }
  void set_credentials(const std::string &username, const std::string &password);
  void set_ca_certificate(const std::string &value) { this->config_.ca_pem = value; }
  void set_timeout(uint32_t value) { this->timeout_ms_ = value; }
  void set_refresh_interval(uint32_t value) { this->refresh_ms_ = value; }
  void set_max_messages(uint8_t value) { this->max_messages_ = value; }
  void set_metadata_buffer_size(size_t value) { this->metadata_size_ = value; }
  void set_fetch_chunk_size(size_t value) { this->fetch_size_ = value; }
  void set_audio_buffer_size(size_t value) { this->audio_size_ = value; }
  void set_decoder_output_buffer_size(size_t value) { this->decoder_output_size_ = value; }
  void set_max_audio_bytes(uint32_t value) { this->max_audio_bytes_ = value; }
  void set_fetch_task_core(int value) { this->fetch_core_ = value < 0 ? tskNO_AFFINITY : value; }
  void set_fetch_task_priority(uint8_t value) { this->fetch_priority_ = value; }
  void set_fetch_task_stack_size(size_t value) { this->fetch_stack_bytes_ = value; }
  void set_mark_read_on_play(bool value) { this->mark_read_on_play_ = value; }
  void refresh();
  void refresh_if_stale();
  void set_page(uint32_t page);
  uint32_t page_index() const { return this->page_; }
  bool has_newer() const { return this->page_ != 0; }
  bool has_older() const { return (static_cast<uint64_t>(this->page_) + 1) * this->max_messages_ < this->mailbox_.total; }
  void older() { if (this->has_older()) this->set_page(this->page_ + 1); }
  void newer() { if (this->has_newer()) this->set_page(this->page_ - 1); }
  uint32_t selected_uid() const { return this->play_message_.uid; }
  uint32_t selected_uid_validity() const { return this->play_message_.uid_validity; }
  bool enabled() const { return this->enabled_; }
  bool available() const { return this->enabled_ && this->ready_ && !this->is_failed() && this->last_success_ != 0 && this->status_ != std::string("error"); }
  size_t unread_on_page() const {
    size_t count = 0;
    for (const auto &message : this->mailbox_.messages) if (!message.seen && !message.deleted) ++count;
    return count;
  }
  template<typename F> void add_on_update_callback(F &&callback) { this->changed_.add(std::forward<F>(callback)); }
  // Cancels this source. An immediate speaker-pipeline flush remains owned by
  // the native media_player.stop action, not by an IMAP transport adapter.
  void stop() { this->handle_command(media_source::MediaSourceCommand::STOP); }
  bool request_play_uid(uint32_t uid, uint32_t uid_validity = 0);
  bool mark_read(uint32_t uid, uint32_t uid_validity, bool read = true);
  bool delete_message(uint32_t uid, uint32_t uid_validity);
  size_t message_count() const { return this->mailbox_.messages.size(); }
  uint32_t total_count() const { return this->mailbox_.total; }
  uint32_t uid_validity() const { return this->mailbox_.uid_validity; }
  const Message *get_message(size_t index) const { return index < this->mailbox_.messages.size() ? &this->mailbox_.messages[index] : nullptr; }
  bool can_play(uint32_t uid) const;
  const char *status() const { return this->status_; }
  Trigger<uint32_t, uint32_t, uint32_t> *get_update_trigger() { return &this->updated_; }
  Trigger<std::string> *get_error_trigger() { return &this->error_; }
  bool play_uri(const std::string &uri) override;
  bool can_handle(const std::string &uri) const override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool has_internal_playlist() const override { return true; }
  size_t audio_sink_write(uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override {
    if (frames != 0 && this->fetch_task_ != nullptr) xTaskNotifyGive(this->fetch_task_);
  }
  void configure_voicemail(std::string server, int32_t port, std::string folder, std::string account_id, bool tls, bool enabled);
  void configure_server(std::string server, int32_t port, std::string folder, bool tls);
  void configure_credentials(std::string username, std::string password);
  void native_play(std::string uid, std::string validity);
  void native_seen(std::string uid, std::string validity, bool seen);
  void native_delete(std::string uid, std::string validity);
  void native_page(int32_t page);

 protected:
  struct Configuration {
    std::string host, account_host, folder{"INBOX"}, username, password, ca_pem;
    uint16_t port{143};
    bool tls{false}, credentials_ready{false};
  } config_;
  enum class OperationKind : uint8_t { SEEN, UNSEEN, DELETE };
  struct Operation { OperationKind kind; uint32_t uid, validity; };
  void worker_run_() override;
  void playback_run_();
  bool enqueue_(Operation operation);
  void account_changed_();
  void publish_catalogue_(Mailbox mailbox, uint32_t generation, bool idle_supported);
  void publish_error_(const std::string &error, uint32_t generation, bool retry = true);
  void publish_playback_(media_source::MediaSourceState state, uint32_t generation);
  void stop_playback_(bool publish_idle = true);
  bool decode_(bool finished);
  bool feed_audio_(const uint8_t *data, size_t length);
  bool wait_playback_();
  void set_state_(media_source::MediaSourceState state) {
    if (this->get_state() == state) return;
    media_source::MediaSource::set_state_(state);
    this->changed_.call();
  }
  static audio::AudioFileType file_type_(const AudioPart &part);
#ifdef USE_IMAP_VOICEMAIL_ACCOUNT
  voip_stack::VoipStack *parent_{nullptr};
#endif
  std::string account_id_;
  uint32_t account_revision_{0}, timeout_ms_{5000}, refresh_ms_{300000}, page_{0}, last_success_{0};
  uint8_t max_messages_{16};
  bool account_loaded_{false}, managed_by_ha_{false}, enabled_{true}, ready_{false}, mark_read_on_play_{true};
  const char *status_{"unconfigured"};
  uint8_t *metadata_buffer_{nullptr}, *fetch_buffer_{nullptr}, *fetch_payload_{nullptr};
  size_t metadata_size_{16384}, fetch_size_{8192}, audio_size_{16384}, decoder_output_size_{8192};
  uint32_t max_audio_bytes_{16 * 1024 * 1024};
  phone_client::TcpStream fetch_stream_;
  TaskHandle_t fetch_task_{nullptr};
  StaticTask_t fetch_task_storage_{};
  StackType_t *fetch_stack_{nullptr};
  size_t fetch_stack_bytes_{8192};
  int fetch_core_{tskNO_AFFINITY};
  uint8_t fetch_priority_{2};
  std::atomic<uint32_t> play_generation_{1};
  std::atomic<bool> paused_{false}, intentional_wake_{false};
  std::atomic<bool> metadata_idling_{false}, refresh_requested_{false};
  uint32_t running_play_generation_{0};
  uint32_t running_uid_{0}, running_uid_validity_{0}, running_config_generation_{0};
  bool play_requested_{false}, seen_sent_{false};
  Message play_message_;
  Configuration play_config_;
  std::shared_ptr<ring_buffer::RingBuffer> audio_ring_;
  std::unique_ptr<audio::AudioDecoder> decoder_;
  std::array<Operation, 8> operations_{};
  size_t operation_head_{0}, operation_count_{0};
  Mailbox mailbox_, pending_mailbox_;
  uint32_t pending_generation_{0};
  bool catalogue_ready_{false}, no_idle_{false};
  std::string pending_error_;
  bool pending_error_retry_{true};
  bool playback_ready_{false};
  media_source::MediaSourceState pending_playback_state_{media_source::MediaSourceState::IDLE};
  uint32_t pending_playback_generation_{0};
  Trigger<uint32_t, uint32_t, uint32_t> updated_;
  Trigger<std::string> error_;
  LazyCallbackManager<void()> changed_;
};

template<typename... Ts> class RefreshAction : public Action<Ts...>, public Parented<ImapVoicemail> {
 public:
  void play(const Ts &...x) override { this->parent_->refresh(); }
};
enum class ControlKind : uint8_t { PLAY, READ, DELETE, PAGE };
template<typename... Ts> class ControlAction : public Action<Ts...>, public Parented<ImapVoicemail> {
 public:
  void set_kind(ControlKind value) { this->kind_ = value; }
  TEMPLATABLE_VALUE(uint32_t, uid)
  TEMPLATABLE_VALUE(uint32_t, uid_validity)
  TEMPLATABLE_VALUE(bool, read)
  void play(const Ts &...x) override {
    const auto uid = this->uid_.value(x...);
    const auto validity = this->uid_validity_.value(x...);
    switch (this->kind_) {
      case ControlKind::PLAY: this->parent_->request_play_uid(uid, validity); break;
      case ControlKind::READ: this->parent_->mark_read(uid, validity, this->read_.value(x...)); break;
      case ControlKind::DELETE: this->parent_->delete_message(uid, validity); break;
      case ControlKind::PAGE: this->parent_->set_page(uid); break;
    }
  }
 protected:
  ControlKind kind_{ControlKind::PLAY};
};
}  // namespace esphome::imap_voicemail
