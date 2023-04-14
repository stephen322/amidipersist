#ifndef USERCONNECTION_H
#define USERCONNECTION_H

#include <array>
#include <map>

#include "common.h"


class UserConnection
{
  public:
    enum Addr_Type { SRC_CLIENT=0, SRC_PORT=1, DST_CLIENT=2, DST_PORT=3 };
    // One connection request from config
    array<midi_addr,4> config;
    // Midi id connections.  Container because names can match more than 1 id.
    multimap<midi_id, midi_id> id_connections;

    bool manually_disconnected; //TODO: Port unsubscribe

  public:
    UserConnection(string src_client, string src_port, string dst_client, string dst_port)
    {
      config[SRC_CLIENT] = { .name=src_client, .status=ID_UNSET };
      config[SRC_PORT] = { .name=src_port, .status=ID_UNSET };
      config[DST_CLIENT] = { .name=dst_client, .status=ID_UNSET };
      config[DST_PORT] = { .name=dst_port, .status=ID_UNSET };
    }

    void update_state(struct alsa_state alsa_state);

};

#endif