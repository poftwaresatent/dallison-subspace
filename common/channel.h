
// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef __COMMON_CHANNEL_H
#define __COMMON_CHANNEL_H

#include <stdio.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/bitset.h"
#include "common/fd.h"
#include <cstdint>
#include <pthread.h>
#include <string>

namespace subspace {

// This is stored immediately before the channel buffer in shared
// memory.  It is transferred intact across the TCP bridges.
// 32 bytes long.
//
// Since this is used primarily for channel bridging, we
// include 4 bytes of padding at offset 0 so that that the
// Socket::SendMessage function has somewhere to put the
// length of the message and avoid 2 sends to the socket.
//
// Note that this precludes us mapping the subscriber's
// channel in read-only memory since the bridge will need
// to write to the padding address when it calls
// Socket::SendMessage.
//
// On the receiving end of the bridge, the padding is
// not received and will not be written to.
struct MessagePrefix {
  int32_t padding; // Padding for Socket::SendMessage.
  int32_t message_size;
  int64_t ordinal;
  uint64_t timestamp;
  int64_t flags;
};

// Flag for flags field in MessagePrefix.
constexpr int kMessageActivate = 1; // This is a reliable activation message.
constexpr int kMessageBridged = 2;  // This message came from the bridge.
constexpr int kMessageSeen = 4;     // Message has been seen.

// We need a max channels number because the size of things in
// shared memory needs to be fixed.
constexpr int kMaxChannels = 1024;

// Maximum number of owners for a lot.  One per subscriber reference
// and publisher reference.  Best if it's a multiple of 64 because
// it's used as the size in a BitSet.
constexpr int kMaxSlotOwners = 1024;

// Max length of a channel name in shared memory.  A name longer
// this this will be truncated but the full name will be available
// in process memory.
constexpr size_t kMaxChannelName = 64;

// This is a global (to a server) structure in shared memory that holds
// counts for the number of updates to publishers and subscribers on
// that server.  The server updates these counts when a publisher or
// subscriber is created or deleted.  The purpose is to allow a client
// to check if it needs to update its local information about the
// channel by contacting the server.  Things such as trigger file
// descriptors are distributed by the server to clients.
//
// This is in shared memory, but it is only ever written by the
// server so there is no lock required to access it in the clients.
struct ChannelCounters {
  uint16_t num_pub_updates;   // Number of updates to publishers.
  uint16_t num_sub_updates;   // Number of updates to subscribers.
  uint16_t num_pubs;          // Current number of publishers.
  uint16_t num_reliable_pubs; // Current number of reliable publishers.
  uint16_t num_subs;          // Current number of subscribers.
  uint16_t num_reliable_subs; // Current number of reliable subscribers.
};

struct SystemControlBlock {
  ChannelCounters counters[kMaxChannels];
};

// Message slots are held in a double linked list, each element of
// which is a SlotListElement (embedded at offset 0 in the MessageSlot
// struct in shared memory).  The linked lists do not use pointers
// because this is in shared memory mapped at different virtual
// addresses in each client.  Instead they use an offset from the
// start of the ChannelControlBlock (CCB) as a pointer.
struct SlotListElement {
  int32_t prev;
  int32_t next;
};

// Double linked list header in shared memory.
struct SlotList {
  int32_t first;
  int32_t last;
};

// This is the meta data for a slot.  It is always in a linked list.
struct MessageSlot {
  SlotListElement element;
  int32_t id;                 // Unique ID for slot (0...num_slots-1).
  int16_t ref_count;          // Number of subscribers referring to this slot.
  int16_t reliable_ref_count; // Number of reliable subscriber references.
  int64_t ordinal;            // Message ordinal held currently in slot.
  int64_t message_size;       // Size of message held in slot.
  BitSet<kMaxSlotOwners> owners; // One bit per publisher/subscriber.
};

// The control data for a channel.  This memory is
// allocated by the server and mapped into the process
// for all publishers and subscribers.  Each mapped CCB is mapped
// at a virtual address chosen by the OS.
//
// This is in shared memory so no pointers are possible.
struct ChannelControlBlock {          // a.k.a CCB
  char channel_name[kMaxChannelName]; // So that you can see the name in a
                                      // debugger or hexdump.
  int num_slots;
  int slot_size;        // Slot size not including the MessagePrefix.
  int64_t next_ordinal; // Next ordinal to use.

  // Statistics counters.
  int64_t total_bytes;
  int64_t total_messages;

  // Slot lists.
  // Active list: slots with active messages in them.
  // Busy list: slots allocated to publishers
  // Free list: slots not allocated.
  SlotList active_list;
  SlotList busy_list;
  SlotList free_list;

  pthread_mutex_t lock; // Lock for this channel only.

  // Variable number of MessageSlot structs (num_slots long).
  MessageSlot slots[0];
};

absl::StatusOr<SystemControlBlock *>
CreateSystemControlBlock(FileDescriptor &fd);

// This holds the shared memory file descriptors for a channel.
// ccb: Channel Control Block
// buffers: message buffer memory.
struct SharedMemoryFds {
  SharedMemoryFds() = default;
  SharedMemoryFds(FileDescriptor ccb, FileDescriptor buffers)
      : ccb(std::move(ccb)), buffers(std::move(buffers)) {}
  SharedMemoryFds(const SharedMemoryFds &) = delete;

  SharedMemoryFds(SharedMemoryFds &&c) {
    ccb = std::move(c.ccb);
    buffers = std::move(c.buffers);
  }
  SharedMemoryFds &operator=(const SharedMemoryFds &) = delete;

  SharedMemoryFds &operator=(SharedMemoryFds &&c) {
    ccb = std::move(c.ccb);
    buffers = std::move(c.buffers);
    return *this;
  }

  FileDescriptor ccb;     // Channel Control Block.
  FileDescriptor buffers; // Message Buffers.
};

// Aligned to given power of 2.
template <int64_t alignment> int64_t Aligned(int64_t v) {
  return (v + (alignment - 1)) & ~(alignment - 1);
}

// This is the representation of a channel as seen by a publisher
// or subscriber.  There is one of these objects per publisher
// and per subscriber.  The object is created by the client
// after communicating with the server for it to allocate
// the shared memory, or get the file descriptors of existing
// shared memory.
//
// The server allocates the shared memory for a channel and
// keeps the file descriptors for the POSIX shared memory,
// which it distributes to the clients upon request.  Clients
// use mmap to map the shared memory into their address
// space.  If there are multiple publishers or subscribers in the
// same process, each of them maps in the shared memory. No attempt
// is made to share the Channel objects.
class Channel {
public:
  struct PublishedMessage {
    MessageSlot *new_slot;
    int64_t ordinal;
    uint64_t timestamp;
  };

  Channel(const std::string &name, int slot_size, int num_slots,
          int channel_id, std::string type);
  ~Channel() { Unmap(); }

  // Allocate the shared memory for a channel.  The num_slots_
  // and slot_size_ member variables will either be 0 (for a subscriber
  // to channel with no publishers), or will contain the channel
  // size parameters.  Unless there's an error, it returns the
  // file descriptors for the allocated CCB and buffers.  The
  // SCB has already been allocated and will be mapped in for
  // this channel.  This is only used in the server.
  absl::StatusOr<SharedMemoryFds> Allocate(const FileDescriptor &scb_fd);

  // Client-side channel mapping.  The SharedMemoryFds contains the
  // file descriptors for the CCB and buffers.  The num_slots_
  // and slot_size_ member variables contain either 0 or the
  // channel size parameters.
  absl::Status Map(SharedMemoryFds fds, const FileDescriptor &scb_fd);
  void Unmap();

  const std::string &Name() const { return name_; }

  // For debug, prints the contents of the three linked lists in
  // shared memory,
  void PrintLists();

  // A placeholder is a channel created for a subscriber where there are
  // no publishers and thus the shared memory is not yet valid.
  bool IsPlaceholder() const { return NumSlots() == 0; }

  // What is the address of the message buffer (after the MessagePrefix)
  // for the slot given a slot id.
  void *GetBufferAddress(int slot_id) const {
    return buffers_ +
           (sizeof(MessagePrefix) + Aligned<32>(slot_size_)) * slot_id +
           sizeof(MessagePrefix);
  }

  // Gets the address for the message buffer given a slot pointer.
  void *GetBufferAddress(MessageSlot *slot) const {
    return buffers_ +
           (sizeof(MessagePrefix) + Aligned<32>(slot_size_)) * slot->id +
           sizeof(MessagePrefix);
  }

  // Find a free slot to use.  This will come from the free list if there
  // is a free slot.  Otherwise it will search for the first unreferenced slot
  // in the active list.  If reliable is true, the search will not go past
  // a slot with a reliable_ref_count > 0.  The owner is the ID of the
  // subscriber or publisher, allocated by the server.
  //
  // This locks the CCB.
  MessageSlot *FindFreeSlot(bool reliable, int owner);

  // A publisher is done with its busy slot (it now contains a message).  The
  // slot is moved from the busy list to the end of the active list and other
  // slot is obtained, unless reliable is set to true.  The new slot
  // is returned and this will be nullptr if reliable is true (reliable
  // publishers get a new slot later).  The function fills in the
  // MessagePrefix.
  //
  // The args are:
  // reliable: this is for a reliable publisher, so don't allocate a new slot
  // is_activation: this is an activation message for a reliable publisher
  // owner: the ID of the publisher
  // omit_prefix: don't fill in the MessagePrefix (used when a message
  //              is read from the bridge)
  // notify: set to true if we should notify the subscribers.
  //
  // Locks the CCB.
  PublishedMessage ActivateSlotAndGetAnother(MessageSlot *slot, bool reliable,
                                             bool is_activation, int owner,
                                             bool omit_prefix, bool *notify);

  // A subscriber wants to find a slot with a message in it.  There are
  // two ways to get this:
  // NextSlot: gets the next slot in the active list
  // LastSlot: gets the last slot in the active list.
  // Can return nullptr if there is no slot.
  // If reliable is true, the reliable_ref_count in the MessageSlot will
  // be manipulated.  The owner is the subscriber ID.
  //
  // Locks the CCB.
  MessageSlot *NextSlot(MessageSlot *slot, bool reliable, int owner);
  MessageSlot *LastSlot(MessageSlot *slot, bool reliable, int owner);

  // Get a pointer to the MessagePrefix for a given slot.
  MessagePrefix *Prefix(MessageSlot *slot) const {
    MessagePrefix *p = reinterpret_cast<MessagePrefix *>(
        buffers_ +
        (sizeof(MessagePrefix) + Aligned<32>(slot_size_)) * slot->id);
    return p;
  }

  void Dump();

  void SetSlots(int slot_size, int num_slots) {
    slot_size_ = slot_size;
    num_slots_ = num_slots;
  }

  int SlotSize() const { return slot_size_; }
  int NumSlots() const { return num_slots_; }
  void CleanupSlots(int owner, bool reliable);

  int GetChannelId() const { return channel_id_; }

  int NumUpdates() const { return num_updates_; }
  void SetNumUpdates(int num_updates) { num_updates_ = num_updates; }

  SystemControlBlock *GetScb() const { return scb_; }

  // Gets the statistics counters.  Locks the CCB.
  void GetCounters(int64_t &total_bytes, int64_t &total_messages);

  void SetDebug(bool v) { debug_ = v; }

  // Search the active list for a message with the given timestamp.  If found,
  // move the owner to the slot found.  Return nullptr if nothing found in which
  // no slot ownership changes are done.  This uses the memory inside buffer
  // to perform a fast search of the slots.  The caller keeps onership of the
  // buffer, but this function will modify it.  This is to avoid memory allocation
  // for every search or buffer allocation for every subscriber when searches
  // are rare.
  MessageSlot *FindActiveSlotByTimestamp(MessageSlot *old_slot,
                                         uint64_t timestamp, bool reliable,
                                         int owner,
                                         std::vector<MessageSlot *> &buffer);

  void SetType(std::string type) { type_ = std::move(type); }
  const std::string Type() const { return type_; }

private:
  int32_t ToCCBOffset(void *addr) {
    return (int32_t)(reinterpret_cast<char *>(addr) -
                     reinterpret_cast<char *>(ccb_));
  }

  void *FromCCBOffset(int32_t offset) {
    return reinterpret_cast<char *>(ccb_) + offset;
  }

  void ListInsertAtEnd(SlotList *list, SlotListElement *e) {
    int32_t offset = ToCCBOffset(e);
    if (list->last == 0) {
      list->first = list->last = offset;
    } else {
      SlotListElement *last =
          reinterpret_cast<SlotListElement *>(FromCCBOffset(list->last));
      last->next = offset;
      e->prev = list->last;
      list->last = offset;
    }
  }

  static void ListInit(SlotList *list) { list->first = list->last = 0; }

  static void ListElementInit(SlotListElement *e) { e->prev = e->next = 0; }

  void ListRemove(SlotList *list, SlotListElement *e) {
    if (e->prev == 0) {
      list->first = e->next;
    } else {
      SlotListElement *prev =
          reinterpret_cast<SlotListElement *>(FromCCBOffset(e->prev));
      prev->next = e->next;
    }
    if (e->next == 0) {
      list->last = e->prev;
    } else {
      SlotListElement *next =
          reinterpret_cast<SlotListElement *>(FromCCBOffset(e->next));
      next->prev = e->prev;
    }
    e->prev = e->next = 0;
  }

  void PrintList(const SlotList *list);

  void AddToBusyList(MessageSlot *slot) {
    ListInsertAtEnd(&ccb_->busy_list, &slot->element);
  }
  void AddToActiveList(MessageSlot *slot) {
    ListInsertAtEnd(&ccb_->active_list, &slot->element);
  }
  MessageSlot *FindFreeSlotLocked(bool reliable, int owner);

  std::string name_;
  int num_slots_;
  int slot_size_;
  int channel_id_; // ID allocated from server.
  std::string type_;

  uint16_t num_updates_ = 0;

  SystemControlBlock *scb_;
  ChannelControlBlock *ccb_;
  char *buffers_ = nullptr;
  bool debug_ = false;
};

} // namespace subspace
#endif /* __COMMON_CHANNEL_H */