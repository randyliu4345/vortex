#ifndef REMOTE_BITBANG_H
#define REMOTE_BITBANG_H

#include <stdint.h>

#include "jtag_dtm.h"

class remote_bitbang_t
{
public:
  // Create a new server, listening for connections from localhost on the given
  // port.
  remote_bitbang_t(uint16_t port, jtag_dtm_t *tap);

  // Do a bit of work.
  void tick();

private:
  jtag_dtm_t *tap;

  int socket_fd;
  int client_fd;

  static const ssize_t buf_size = 64 * 1024;
  char send_buf[buf_size];
  char recv_buf[buf_size];
  ssize_t recv_start, recv_end;

  void accept();
  void execute_commands();
};

#endif

