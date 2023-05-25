// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "client/client.h"
#include "absl/strings/str_format.h"
#include "common/clock.h"
#include "common/mutex.h"
#include "common/sockets.h"
#include "proto/subspace.pb.h"
#include <cerrno>
#include <inttypes.h>

namespace subspace {

absl::Status Client::CheckConnected() const {
  if (!socket_.Connected()) {
    return absl::InternalError(
        "Client is not connected to the server; have you called Init()?");
  }
  return absl::OkStatus();
}

absl::Status Client::Init(const std::string &server_socket,
                          const std::string &client_name) {
  if (socket_.Connected()) {
    return absl::InternalError("Client is already connected to the server; "
                               "Init() called twice perhaps?");
  }
  absl::Status status = socket_.Connect(server_socket);
  if (!status.ok()) {
    return status;
  }

  Request req;
  req.mutable_init()->set_client_name(client_name);
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  status = SendRequestReceiveResponse(req, resp, fds);
  if (!status.ok()) {
    return status;
  }
  scb_fd_ = std::move(fds[resp.init().scb_fd_index()]);
  return absl::OkStatus();
}

void Client::RegisterDroppedMessageCallback(
    Subscriber *subscriber,
    std::function<void(Subscriber *, int64_t)> callback) {
  dropped_message_callbacks_[subscriber] = std::move(callback);
}

absl::Status Client::UnregisterDroppedMessageCallback(Subscriber *subscriber) {
  auto it = dropped_message_callbacks_.find(subscriber);
  if (it == dropped_message_callbacks_.end()) {
    return absl::InternalError(absl::StrFormat(
        "No dropped message callback has been registered for channel %s\n",
        subscriber->Name()));
  }
  dropped_message_callbacks_.erase(it);
  return absl::OkStatus();
}

absl::StatusOr<Publisher *>
Client::CreatePublisher(const std::string &channel_name, int slot_size,
                        int num_slots, const PublisherOptions &opts) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  Request req;
  auto *cmd = req.mutable_create_publisher();
  cmd->set_channel_name(channel_name);
  cmd->set_slot_size(slot_size);
  cmd->set_num_slots(num_slots);
  cmd->set_is_public(opts.IsPublic());
  cmd->set_is_reliable(opts.IsReliable());
  cmd->set_is_bridge(opts.IsBridge());
  cmd->set_type(opts.Type());

  // Send request to server and wait for response.
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, resp, fds);
      !status.ok()) {
    return status;
  }
  auto &pub_resp = resp.create_publisher();
  if (!pub_resp.error().empty()) {
    return absl::InternalError(pub_resp.error());
  }

  // Make a local ClientChannel object and map in the shared memory allocated
  // by the server.
  std::unique_ptr<Publisher> channel = std::make_unique<Publisher>(
      channel_name, slot_size, num_slots, pub_resp.channel_id(),
      pub_resp.publisher_id(), pub_resp.type(), opts);
  SharedMemoryFds channel_fds(std::move(fds[pub_resp.ccb_fd_index()]),
                              std::move(fds[pub_resp.buffers_fd_index()]));
  if (absl::Status status = channel->Map(std::move(channel_fds), scb_fd_);
      !status.ok()) {
    return status;
  }

  channel->SetTriggerFd(std::move(fds[pub_resp.pub_trigger_fd_index()]));
  channel->SetPollFd(std::move(fds[pub_resp.pub_poll_fd_index()]));

  // Add all subscriber triggers fds to the publisher channel.
  channel->ClearSubscribers();
  for (auto index : pub_resp.sub_trigger_fd_indexes()) {
    channel->AddSubscriber(std::move(fds[index]));
  }

  channel->SetNumUpdates(pub_resp.num_sub_updates());

  if (!opts.IsReliable()) {
    // A publisher needs a slot.  Allocate one.
    MessageSlot *slot = channel->FindFreeSlot(false, channel->GetPublisherId());
    if (slot == nullptr) {
      return absl::InternalError("No slot available for publisher");
    }
    channel->SetSlot(slot);
  } else {
    // Send a single activation message to the channel.
    absl::Status status = ActivateReliableChannel(channel.get());
    if (!status.ok()) {
      return status;
    }
  }
  channel->TriggerSubscribers();

  // channel->Dump();
  Publisher *channel_ptr = channel.get();
  channels_.insert(std::move(channel));
  return channel_ptr;
}

absl::StatusOr<Subscriber *>
Client::CreateSubscriber(const std::string &channel_name,
                         const SubscriberOptions &opts) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  Request req;
  auto *cmd = req.mutable_create_subscriber();
  cmd->set_channel_name(channel_name);
  cmd->set_subscriber_id(-1); // New subscriber is being created.
  cmd->set_is_reliable(opts.IsReliable());
  cmd->set_is_bridge(opts.IsBridge());
  cmd->set_type(opts.Type());

  // Send request to server and wait for response.
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, resp, fds);
      !status.ok()) {
    return status;
  }

  auto &sub_resp = resp.create_subscriber();
  if (!sub_resp.error().empty()) {
    return absl::InternalError(sub_resp.error());
  }

  // Make a local Subscriber object and map in the shared memory allocated
  // by the server.
  std::unique_ptr<Subscriber> channel = std::make_unique<Subscriber>(
      channel_name, sub_resp.slot_size(), sub_resp.num_slots(),
      sub_resp.channel_id(), sub_resp.subscriber_id(), sub_resp.type(), opts);
  SharedMemoryFds channel_fds(std::move(fds[sub_resp.ccb_fd_index()]),
                              std::move(fds[sub_resp.buffers_fd_index()]));
  if (absl::Status status = channel->Map(std::move(channel_fds), scb_fd_);
      !status.ok()) {
    return status;
  }

  channel->SetTriggerFd(std::move(fds[sub_resp.trigger_fd_index()]));
  channel->SetPollFd(std::move(fds[sub_resp.poll_fd_index()]));

  // Add all publisher triggers fds to the subscriber channel.
  channel->ClearPublishers();
  for (auto index : sub_resp.reliable_pub_trigger_fd_indexes()) {
    channel->AddPublisher(std::move(fds[index]));
  }

  channel->SetNumUpdates(sub_resp.num_pub_updates());

  // Trigger the subscriber to pick up all existing messages.
  channel->Trigger();

  // channel->Dump();

  Subscriber *channel_ptr = channel.get();
  channels_.insert(std::move(channel));
  return channel_ptr;
}

absl::StatusOr<void *> Client::GetMessageBuffer(Publisher *publisher) {
  publisher->ClearPollFd();

  if (absl::Status status = ReloadSubscribersIfNecessary(publisher);
      !status.ok()) {
    return status;
  }

  if (publisher->IsReliable() && publisher->CurrentSlot() == nullptr) {
    // We are a reliable publisher and don't have a slot yet.  Try to allocate
    // one now.  If we fail, we return nullptr so that the caller knows to try
    // again.
    //
    // If there are no subscribers to the channel, don't allow a message to
    // be published yet.  This is because since there are no subscribers
    // there are no slots with reliable_ref_count > 0 and therefore nothing
    // to stop the publisher taking all the slots.  An incoming subscriber
    // would miss all those messages and that's not reliable.
    if (publisher->NumSubscribers() == 0) {
      return nullptr;
    }
    MessageSlot *slot =
        publisher->FindFreeSlot(true, publisher->GetPublisherId());
    if (slot == nullptr) {
      return nullptr;
    }
    publisher->SetSlot(slot);
  }
  void *buffer = publisher->GetCurrentBufferAddress();
  if (buffer == nullptr) {
    return absl::InternalError(
        absl::StrFormat("Channel %s has no buffer", publisher->Name()));
  }
  return buffer;
}

absl::StatusOr<const Message> Client::PublishMessage(Publisher *publisher,
                                                     int64_t message_size) {
  return PublishMessageInternal(publisher, message_size, /*omit_prefix=*/false);
}

absl::StatusOr<const Message>
Client::PublishMessageInternal(Publisher *publisher, int64_t message_size,
                               bool omit_prefix) {
  // Check if there are any new subscribers and if so, load their trigger fds.
  if (absl::Status status = ReloadSubscribersIfNecessary(publisher);
      !status.ok()) {
    return status;
  }

  publisher->SetMessageSize(message_size);

  MessageSlot *old_slot = publisher->CurrentSlot();
  if (debug_) {
    if (old_slot != nullptr) {
      printf("publish old slot: %d: %" PRId64 "\n", old_slot->id,
             old_slot->ordinal);
    }
  }
  bool notify = false;
  Channel::PublishedMessage msg = publisher->ActivateSlotAndGetAnother(
      publisher->IsReliable(), /*is_activation=*/false, omit_prefix, &notify);

  // Prevent use of old_slot.
  old_slot = nullptr;

  publisher->SetSlot(msg.new_slot);

  // Only trigger subscribers if we need to.
  // We could trigger for every message, but that is unnecessary and
  // slower.  It would basically mean a write to pipe for every
  // message sent.  That's fast, but if we can avoid it, things
  // would be faster.
  if (notify) {
    publisher->TriggerSubscribers();
  }

  if (msg.new_slot == nullptr) {
    if (publisher->IsReliable()) {
      // Reliable publishers don't get a slot until it's asked for.
      return Message(message_size, nullptr, msg.ordinal, msg.timestamp);
    }
    return absl::InternalError(
        absl::StrFormat("Out of slots for channel %s", publisher->Name()));
  }

  if (debug_) {
    printf("publish new slot: %d: %" PRId64 "\n", msg.new_slot->id,
           msg.new_slot->ordinal);
  }

  return Message(message_size, nullptr, msg.ordinal, msg.timestamp);
}

absl::Status Client::WaitForReliablePublisher(Publisher *publisher) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  if (!publisher->IsReliable()) {
    return absl::InternalError("Unreliable publishers can't wait");
  }
  // Check if there are any new subscribers and if so, load their trigger fds.
  if (absl::Status status = ReloadSubscribersIfNecessary(publisher);
      !status.ok()) {
    return status;
  }
  if (co_ != nullptr) {
    // Coroutine aware.  Yield control back until the poll fd is triggered.
    co_->Wait(publisher->GetPollFd().Fd(), POLLIN);
  } else {
    struct pollfd fd = {.fd = publisher->GetPollFd().Fd(), .events = POLLIN};
    int e = ::poll(&fd, 1, -1);
    // Since we are waiting forever will can only get the value 1 from the poll.
    // We will never get 0 since there is no timeout.  Anything else (can only
    // be -1) will be an error.
    if (e != 1) {
      return absl::InternalError(
          absl::StrFormat("Error from poll waiting for reliable publisher: %s",
                          strerror(errno)));
    }
  }
  return absl::OkStatus();
}

absl::Status Client::WaitForSubscriber(Subscriber *subscriber) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }

  if (co_ != nullptr) {
    // Coroutine aware.  Yield control back until the poll fd is triggered.
    co_->Wait(subscriber->GetPollFd().Fd(), POLLIN);
  } else {
    struct pollfd fd = {.fd = subscriber->GetPollFd().Fd(), .events = POLLIN};
    int e = ::poll(&fd, 1, -1);
    // Since we are waiting forever will can only get the value 1 from the poll.
    // We will never get 0 since there is no timeout.  Anything else (can only
    // be -1) will be an error.
    if (e != 1) {
      return absl::InternalError(absl::StrFormat(
          "Error from poll waiting for subscriber: %s", strerror(errno)));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<const Message>
Client::ReadMessageInternal(Subscriber *subscriber, ReadMode mode,
                            bool pass_activation, bool clear_trigger) {
  if (clear_trigger) {
    subscriber->ClearPollFd();
  }

  MessageSlot *new_slot = nullptr;
  MessageSlot *old_slot = subscriber->CurrentSlot();
  int64_t last_ordinal = -1;
  if (old_slot != nullptr) {
    last_ordinal = old_slot->ordinal;
    if (debug_) {
      printf("old slot: %d: %" PRId64 "\n", old_slot->id, last_ordinal);
    }
  }

  switch (mode) {
  case ReadMode::kReadNext:
    new_slot = subscriber->NextSlot();
    break;
  case ReadMode::kReadNewest:
    new_slot = subscriber->LastSlot();
    break;
  }
  // At this point, old_slot may have been reused so don't reference it
  // for any data.
  old_slot = nullptr; // Prevent any accidental use.

  if (new_slot == nullptr) {
    // I'm out of messages to read, trigger the publishers to give me
    // some more.  This is only for reliable publishers.
    subscriber->TriggerReliablePublishers();
    return Message();
  }
  subscriber->SetSlot(new_slot);

  if (debug_) {
    printf("new_slot: %d: %" PRId64 "\n", new_slot->id, new_slot->ordinal);
  }

  if (last_ordinal != -1 && new_slot->ordinal != (last_ordinal + 1)) {
    // We dropped a message.  If we have a callback registered for this
    // channel, call it with the number of dropped messages.
    auto it = dropped_message_callbacks_.find(subscriber);
    if (it != dropped_message_callbacks_.end()) {
      it->second(subscriber, new_slot->ordinal - last_ordinal);
    }
  }
  MessagePrefix *prefix = subscriber->Prefix(new_slot);
  if (prefix != nullptr) {
    if ((prefix->flags & kMessageActivate) != 0) {
      if (!pass_activation) {
        return ReadMessageInternal(subscriber, mode,
                                   /* pass_activation=*/false,
                                   /* clear_trigger=*/false);
      }
    }
  }
  return Message(new_slot->message_size, subscriber->GetCurrentBufferAddress(),
                 subscriber->CurrentOrdinal(), subscriber->Timestamp());
}

absl::StatusOr<const Message> Client::ReadMessage(Subscriber *subscriber,
                                                  ReadMode mode) {
  // If the channel is a placeholder (no publishers present), contact the
  // server to see if there is now a publisher.  This will reload the shared
  // memory.  If there still isn't a publisher, we will still be a placeholder.
  if (subscriber->IsPlaceholder()) {
    absl::Status status = ReloadSubscriber(subscriber);
    if (!status.ok() || subscriber->IsPlaceholder()) {
      subscriber->ClearPollFd();
      return Message();
    }
  }

  // Check if there are any new reliable publishers and if so, load their
  // trigger fds.
  absl::Status status = ReloadReliablePublishersIfNecessary(subscriber);
  if (!status.ok()) {
    return status;
  }

  return ReadMessageInternal(subscriber, mode, /*pass_activation=*/false,
                             /*clear_trigger=*/true);
}

absl::StatusOr<const Message>
Client::FindMessageInternal(Subscriber *subscriber, uint64_t timestamp) {

  MessageSlot *new_slot = subscriber->FindMessage(timestamp);
  if (new_slot == nullptr) {
    // Not found.
    return Message();
  }
  return Message(new_slot->message_size, subscriber->GetCurrentBufferAddress(),
                 subscriber->CurrentOrdinal(), subscriber->Timestamp());
}

absl::StatusOr<const Message> Client::FindMessage(Subscriber *subscriber,
                                                  uint64_t timestamp) {
  // If the channel is a placeholder (no publishers present), contact the
  // server to see if there is now a publisher.  This will reload the shared
  // memory.  If there still isn't a publisher, we will still be a placeholder.
  if (subscriber->IsPlaceholder()) {
    absl::Status status = ReloadSubscriber(subscriber);
    if (!status.ok() || subscriber->IsPlaceholder()) {
      subscriber->ClearPollFd();
      return Message();
    }
  }

  // Check if there are any new reliable publishers and if so, load their
  // trigger fds.
  absl::Status status = ReloadReliablePublishersIfNecessary(subscriber);
  if (!status.ok()) {
    return status;
  }
  return FindMessageInternal(subscriber, timestamp);
}

struct pollfd Client::GetPollFd(Subscriber *subscriber) {
  struct pollfd fd = {.fd = subscriber->GetPollFd().Fd(), .events = POLLIN};
  return fd;
}

struct pollfd Client::GetPollFd(Publisher *publisher) {
  static struct pollfd fd { .fd = -1, .events = POLLIN };
  if (!publisher->IsReliable()) {
    return fd;
  }
  fd = {.fd = publisher->GetPollFd().Fd(), .events = POLLIN};
  return fd;
}

int64_t Client::GetCurrentOrdinal(Subscriber *sub) const {
  MessageSlot *slot = sub->CurrentSlot();
  if (slot == nullptr) {
    return -1;
  }
  return slot->ordinal;
}

absl::Status Client::ReloadSubscriber(Subscriber *subscriber) {
  // Check if there are any updates to the publishers
  // since that last time we checked.
  SystemControlBlock *scb = subscriber->GetScb();
  int updates = scb->counters[subscriber->GetChannelId()].num_pub_updates;
  if (subscriber->NumUpdates() == updates) {
    return absl::OkStatus();
  }
  subscriber->SetNumUpdates(updates);

  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  Request req;
  auto *cmd = req.mutable_create_subscriber();
  cmd->set_channel_name(subscriber->Name());
  cmd->set_subscriber_id(subscriber->GetSubscriberId());

  // Send request to server and wait for response.
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, resp, fds);
      !status.ok()) {
    return status;
  }

  auto &sub_resp = resp.create_subscriber();
  if (!sub_resp.error().empty()) {
    return absl::InternalError(sub_resp.error());
  }

  // Unmap the channel memory.
  subscriber->Unmap();

  SharedMemoryFds channel_fds(std::move(fds[sub_resp.ccb_fd_index()]),
                              std::move(fds[sub_resp.buffers_fd_index()]));
  subscriber->SetSlots(sub_resp.slot_size(), sub_resp.num_slots());

  if (absl::Status status = subscriber->Map(std::move(channel_fds), scb_fd_);
      !status.ok()) {
    return status;
  }

  subscriber->SetTriggerFd(fds[sub_resp.trigger_fd_index()]);
  subscriber->SetPollFd(fds[sub_resp.poll_fd_index()]);

  // Add all publisher trigger fds to the subscriber channel.
  subscriber->ClearPublishers();
  for (auto index : sub_resp.reliable_pub_trigger_fd_indexes()) {
    subscriber->AddPublisher(fds[index]);
  }
  // subscriber->Dump();
  return absl::OkStatus();
}

absl::Status Client::ReloadSubscribersIfNecessary(Publisher *publisher) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }

  SystemControlBlock *scb = publisher->GetScb();
  int updates = scb->counters[publisher->GetChannelId()].num_sub_updates;
  if (publisher->NumUpdates() == updates) {
    return absl::OkStatus();
  }
  publisher->SetNumUpdates(updates);

  // We do have updates, get a new list of subscriber for
  // the channel.
  Request req;
  auto *cmd = req.mutable_get_triggers();
  cmd->set_channel_name(publisher->Name());

  // Send request to server and wait for response.
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, resp, fds);
      !status.ok()) {
    return status;
  }

  auto &sub_resp = resp.get_triggers();
  // Add all subscriber triggers fds to the publisher channel.
  publisher->ClearSubscribers();
  for (auto index : sub_resp.sub_trigger_fd_indexes()) {
    publisher->AddSubscriber(fds[index]);
  }
  return absl::OkStatus();
}

absl::Status
Client::ReloadReliablePublishersIfNecessary(Subscriber *subscriber) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  // Check if there are any updates to the publishers
  // since that last time we checked.
  SystemControlBlock *scb = subscriber->GetScb();
  int updates = scb->counters[subscriber->GetChannelId()].num_pub_updates;
  if (subscriber->NumUpdates() == updates) {
    return absl::OkStatus();
  }
  subscriber->SetNumUpdates(updates);

  // We do have updates, get a new list of subscriber for
  // the channel.
  Request req;
  auto *cmd = req.mutable_get_triggers();
  cmd->set_channel_name(subscriber->Name());

  // Send request to server and wait for response.
  Response resp;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, resp, fds);
      !status.ok()) {
    return status;
  }

  auto &sub_resp = resp.get_triggers();
  // Add all subscriber triggers fds to the publisher channel.
  subscriber->ClearPublishers();
  for (auto index : sub_resp.reliable_pub_trigger_fd_indexes()) {
    subscriber->AddPublisher(fds[index]);
  }
  return absl::OkStatus();
}

// A reliable publisher always sends a single activation message when it
// is created.  This is to ensure that the reliable subscribers see
// on message and thus keep a reference to it.
absl::Status Client::ActivateReliableChannel(Publisher *publisher) {
  MessageSlot *slot =
      publisher->FindFreeSlot(/*reliable=*/true, publisher->GetPublisherId());
  if (slot == nullptr) {
    return absl::InternalError(
        absl::StrFormat("Channel %s has no free slots", publisher->Name()));
  }
  publisher->SetSlot(slot);

  void *buffer = publisher->GetCurrentBufferAddress();
  if (buffer == nullptr) {
    return absl::InternalError(
        absl::StrFormat("Channel %s has no buffer", publisher->Name()));
  }
  slot->message_size = 1;

  publisher->ActivateSlotAndGetAnother(/*reliable=*/true,
                                       /*is_activation=*/true,
                                       /*omit_prefix=*/false);
  publisher->SetSlot(nullptr);
  publisher->TriggerSubscribers();

  return absl::OkStatus();
}

absl::Status Client::RemoveChannel(ClientChannel *channel) {
  if (!channels_.contains(channel)) {
    return absl::InternalError(
        absl::StrFormat("Channel %s not found\n", channel->Name()));
  }
  channels_.erase(channel);
  return absl::OkStatus();
}

absl::Status Client::RemovePublisher(Publisher *publisher) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  Request req;
  auto *cmd = req.mutable_remove_publisher();
  cmd->set_channel_name(publisher->Name());
  cmd->set_publisher_id(publisher->GetPublisherId());

  // Send request to server and wait for response.
  Response response;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, response, fds);
      !status.ok()) {
    return status;
  }

  auto &resp = response.remove_publisher();
  if (!resp.error().empty()) {
    return absl::InternalError(resp.error());
  }
  return RemoveChannel(publisher);
}

absl::Status Client::RemoveSubscriber(Subscriber *subscriber) {
  if (absl::Status status = CheckConnected(); !status.ok()) {
    return status;
  }
  Request req;
  auto *cmd = req.mutable_remove_subscriber();
  cmd->set_channel_name(subscriber->Name());
  cmd->set_subscriber_id(subscriber->GetSubscriberId());

  // Send request to server and wait for response.
  Response response;
  std::vector<FileDescriptor> fds;
  fds.reserve(100);
  if (absl::Status status = SendRequestReceiveResponse(req, response, fds);
      !status.ok()) {
    return status;
  }

  auto &resp = response.remove_subscriber();
  if (!resp.error().empty()) {
    return absl::InternalError(resp.error());
  }
  return RemoveChannel(subscriber);
}

const ChannelCounters &Client::GetChannelCounters(ClientChannel *channel) {
  return channel->GetCounters();
}

absl::Status
Client::SendRequestReceiveResponse(const Request &req, Response &response,
                                   std::vector<FileDescriptor> &fds) {
  // SendMessage needs 4 bytes before the buffer passed to
  // use for the length.
  char *sendbuf = buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(buffer_) - sizeof(int32_t);

  if (!req.SerializeToArray(sendbuf, kSendBufLen)) {
    return absl::InternalError("Failed to serialize request");
  }

  size_t length = req.ByteSizeLong();
  absl::StatusOr<ssize_t> n = socket_.SendMessage(sendbuf, length, co_);
  if (!n.ok()) {
    socket_.Close();
    return n.status();
  }

  // Wait for response and put it in the same buffer we used for send.
  n = socket_.ReceiveMessage(buffer_, sizeof(buffer_), co_);
  if (!n.ok()) {
    socket_.Close();
    return n.status();
  }
  if (!response.ParseFromArray(buffer_, *n)) {
    socket_.Close();
    return absl::InternalError("Failed to parse response");
  }
  absl::Status s = socket_.ReceiveFds(fds, co_);
  if (!s.ok()) {
    socket_.Close();
  }
  return s;
}

} // namespace subspace