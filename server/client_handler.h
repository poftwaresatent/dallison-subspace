// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef __SERVER_CLIENT_HANDLER_H
#define __SERVER_CLIENT_HANDLER_H

#include "absl/status/status.h"
#include "common/sockets.h"
#include "coroutine.h"
#include "proto/subspace.pb.h"
#include <sys/poll.h>
#include <vector>

namespace subspace {

class Server;

class ClientHandler {
public:
  ClientHandler(Server *server, UnixSocket socket)
      : server_(server), socket_(std::move(socket)) {}
  ~ClientHandler();

  // Run the client handler receiver in a coroutine.  Terminates
  // when the connection to the client is closed.
  void Run(co::Coroutine *c);

private:
  absl::Status HandleMessage(const subspace::Request &req, subspace::Response &resp,
                             std::vector<FileDescriptor> &fds);

  // These individual handler functions set any errors in the response
  // message instead of returning them to the caller.  This allows the
  // connection to remain open to the client and the client will be
  // able to display or handle the error as appropriate.
  void HandleInit(const subspace::InitRequest &req, subspace::InitResponse *response,
                  std::vector<FileDescriptor> &fds);
  void HandleCreatePublisher(const subspace::CreatePublisherRequest &req,
                             subspace::CreatePublisherResponse *response,
                             std::vector<FileDescriptor> &fds);
  void HandleCreateSubscriber(const subspace::CreateSubscriberRequest &req,
                              subspace::CreateSubscriberResponse *response,
                              std::vector<FileDescriptor> &fds);
  void HandleGetTriggers(const subspace::GetTriggersRequest &req,
                         subspace::GetTriggersResponse *response,
                         std::vector<FileDescriptor> &fds);

  void HandleRemovePublisher(const subspace::RemovePublisherRequest &req,
                             subspace::RemovePublisherResponse *response,
                             std::vector<FileDescriptor> &fds);
  void HandleRemoveSubscriber(const subspace::RemoveSubscriberRequest &req,
                              subspace::RemoveSubscriberResponse *response,
                              std::vector<FileDescriptor> &fds);
  Server *server_;
  UnixSocket socket_;
  char buffer_[kMaxMessage];
  std::string client_name_;
};

} // namespace subspace

#endif // __SERVER_CLIENT_HANDLER_H