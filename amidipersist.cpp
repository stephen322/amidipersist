using namespace std;

#include <string>
#include <list>
#include <vector>
#include <map>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>

#include <alsa/asoundlib.h>

//Alsa midi client id and port id are unsigned char
typedef unsigned char seq_id;
enum ID_Status { ID_UNSET=0, ID_DISCOVERED, ID_USER_SET };


class user_connection;
list<user_connection> conn_configs;

//Map for client name to id
map<string, seq_id> client_map;

//Map for port name (plus client id) to port id
map<pair<seq_id,string>, seq_id> port_map;

//Map for alsa subscriptions
multimap<pair<seq_id,seq_id>, pair<seq_id,seq_id>> sub_map;

//Map id's to names
map<pair<seq_id,seq_id>, pair<string,string>> id_map;


struct midi_addr {
  string name;
  seq_id id;
  ID_Status status;
};

class user_connection
{
  protected:
    enum Addr_Type { SRC_CLIENT=0, SRC_PORT=1, DST_CLIENT=2, DST_PORT=3 };
    midi_addr addr[4];

    bool manually_disconnected; //Port unsubscribe

  public:
    //These are dangerous?  Prob should write copy/move constructor or getters
    //For now we'll make sure conn_configs is the only owner/creator
    const midi_addr &src_client = addr[SRC_CLIENT];
    const midi_addr &src_port = addr[SRC_PORT];
    const midi_addr &dst_client = addr[DST_CLIENT];
    const midi_addr &dst_port = addr[DST_PORT];

    bool all_ids_valid;
    bool is_connected;

  public:
    user_connection(string src_client, string src_port, string dst_client, string dst_port)
    {
      addr[SRC_CLIENT] = { .name=src_client, .status=ID_UNSET };
      addr[SRC_PORT] = { .name=src_port, .status=ID_UNSET };
      addr[DST_CLIENT] = { .name=dst_client, .status=ID_UNSET };
      addr[DST_PORT] = { .name=dst_port, .status=ID_UNSET };
    }

    // Sets midi id and status based on our maps of alsa graph
    void set_ids(midi_addr &client, midi_addr &port) {
      // Handle client lookups
      if (client.status != ID_USER_SET) {
        auto client_it = client_map.find(client.name);
        if (client_it == client_map.end()) {
          client.status = ID_UNSET;
        }
        else {
          client.id = client_it->second;
          client.status = ID_DISCOVERED;
        }
      }
      // Handle port lookups
      if (port.status != ID_USER_SET && client.status != ID_UNSET) {
        auto port_it = port_map.find(make_pair(client.id, port.name));
        if (port_it == port_map.end()) {
          port.status = ID_UNSET;
        }
        else {
          port.id = port_it->second;
          port.status = ID_DISCOVERED;
        }
      }
    }

    void update_state() {
      set_ids(this->addr[SRC_CLIENT], this->addr[SRC_PORT]);
      set_ids(this->addr[DST_CLIENT], this->addr[DST_PORT]);

      // Some boolean logic: AND statuses together
      bool s = true;
      for (int i=0; i<4; ++i) {
        s = s && (this->addr[i].status != ID_UNSET);
      }
      this->all_ids_valid = s;

      if (this->all_ids_valid) {
        this->is_connected = false;
        auto range = sub_map.equal_range(make_pair(this->addr[SRC_CLIENT].id, this->addr[SRC_PORT].id));
        for (auto it = range.first; it != range.second; ++it) {
          auto connected_dst = it->second;
          if (connected_dst.first == this->addr[DST_CLIENT].id && connected_dst.second == this->addr[DST_PORT].id) {
            this->is_connected = true;
            break;
          }
        }
      }
    }
};

snd_seq_t* sequencer_open()
{
  snd_seq_t *sequencer;
  snd_seq_open(&sequencer, "default", SND_SEQ_OPEN_DUPLEX, 0);
  snd_seq_set_client_name(sequencer, "amidipersist");

  return sequencer;
}

void sequencer_close(snd_seq_t* sequencer)
{
  snd_seq_close(sequencer);
}

// Alsa midi client:port traversal
// Template from alsamidicable - https://github.com/dgslomin/divs-midi-utilities
void alsa_build_graph(snd_seq_t *sequencer)
{
  client_map.clear();
  port_map.clear();
  sub_map.clear();

  snd_seq_client_info_t *client_info;
  snd_seq_client_info_t *connected_client_info;
  snd_seq_port_info_t *port_info;
  snd_seq_port_info_t *connected_port_info;
  snd_seq_query_subscribe_t *subscriptions;

  snd_seq_client_info_malloc(&client_info);
  snd_seq_client_info_malloc(&connected_client_info);
  snd_seq_port_info_malloc(&port_info);
  snd_seq_port_info_malloc(&connected_port_info);
  snd_seq_query_subscribe_malloc(&subscriptions);

  snd_seq_client_info_set_client(client_info, -1);

  //Iterate over clients
  while (snd_seq_query_next_client(sequencer, client_info) >= 0)
  {
    seq_id client_id = snd_seq_client_info_get_client(client_info);
    const string client_name = string(snd_seq_client_info_get_name(client_info));
    snd_seq_port_info_set_client(port_info, client_id);
    snd_seq_port_info_set_port(port_info, -1);

    //Map client name to id
    client_map[client_name] = client_id;

    //Iterate over ports for this client
    while (snd_seq_query_next_port(sequencer, port_info) >= 0)
    {
      const string port_name = string(snd_seq_port_info_get_name(port_info));
      const seq_id port_id = snd_seq_port_info_get_port(port_info);

      //Map port name to id
      port_map[make_pair(client_id,port_name)] = port_id;

      //Map id's to names
      id_map[make_pair(client_id,port_id)] = make_pair(client_name,port_name);

      //If we are an input(read)-capable port, check for subscriptions
      if (snd_seq_port_info_get_capability(port_info) & SND_SEQ_PORT_CAP_SUBS_READ)
      {
        snd_seq_query_subscribe_set_root(subscriptions, snd_seq_port_info_get_addr(port_info));
        snd_seq_query_subscribe_set_type(subscriptions, SND_SEQ_QUERY_SUBS_READ);
        snd_seq_query_subscribe_set_index(subscriptions, 0);

        //Iterate over connected ports
        while (snd_seq_query_port_subscribers(sequencer, subscriptions) >= 0)
        {
          snd_seq_addr_t *connected_port_address = (snd_seq_addr_t *)(snd_seq_query_subscribe_get_addr(subscriptions));
          snd_seq_get_any_port_info(sequencer, connected_port_address->client, connected_port_address->port, connected_port_info);
          snd_seq_get_any_client_info(sequencer, snd_seq_port_info_get_client(connected_port_info), connected_client_info);

          //Map subscription
          sub_map.emplace(make_pair(
            make_pair(client_id, port_id),
            make_pair(snd_seq_client_info_get_client(connected_client_info), snd_seq_port_info_get_port(connected_port_info))
          ));

          snd_seq_query_subscribe_set_index(subscriptions, snd_seq_query_subscribe_get_index(subscriptions) + 1);
        }
      }
    }
  }

  snd_seq_query_subscribe_free(subscriptions);
  snd_seq_port_info_free(connected_port_info);
  snd_seq_port_info_free(port_info);
  snd_seq_client_info_free(connected_client_info);
  snd_seq_client_info_free(client_info);
}

// Connect sender client:port to dest client:port
bool alsa_connect(snd_seq_t* sequencer, const pair<seq_id,seq_id> &sender, const pair<seq_id,seq_id> &dest)
{
  snd_seq_addr_t from = { sender.first, sender.second };
  snd_seq_addr_t to = { dest.first, dest.second };

  snd_seq_port_subscribe_t * sub;
  snd_seq_port_subscribe_alloca(&sub);

  snd_seq_port_subscribe_set_sender(sub, &from);
  snd_seq_port_subscribe_set_dest(sub, &to);
  /*
  //Additional modes/flags
  snd_seq_port_subscribe_set_queue(sub, 0);
  snd_seq_port_subscribe_set_exclusive(sub, 0);
  snd_seq_port_subscribe_set_time_update(sub, 0);
  snd_seq_port_subscribe_set_time_real(sub, 0);
  */

  int r = snd_seq_subscribe_port(sequencer, sub);
  return (!r);
}







void run_connections(snd_seq_t* sequencer)
{
  for (auto &c : conn_configs) {
    c.update_state();
    if (c.is_connected) continue;

    if (alsa_connect(sequencer, make_pair(c.src_client.id,c.src_port.id), make_pair(c.dst_client.id,c.dst_port.id))) {
      printf("Connected %s:%s (%d:%d) to %s:%s (%d:%d)\n", 
        c.src_client.name.c_str(), c.src_port.name.c_str(), c.src_client.id, c.src_port.id,
        c.dst_client.name.c_str(), c.dst_port.name.c_str(), c.dst_client.id, c.dst_port.id);
    }
  }
}








// Load connection config file
// This file should be a colon delimited file with 4 columns, the names of: src_client:src_port:dst_client:dst_port
// -The colon can be escaped with backslash
// -Empty lines and lines beginning with # are ignored
void parse_config(ifstream &infile)
{
  string line;
  int linecount = 0;
  while (getline(infile, line))
  {
    ++linecount;

    //Skip empty lines
    if (line == "") 
      continue;
    //Skip comment lines
    if (line[0] == '#')
      continue;

    vector<string> cols;
    istringstream iss(line);
    string cell;
    int i = 0;

    while (getline(iss,cell,':')) {
      if (i>=4) break;

      //Handle escape char backslash before colon
      while (cell.back() == '\\') {
        string cellcont;
        cell.pop_back();
        cell.push_back(':');
        getline(iss,cellcont,':');
        cell.append(cellcont);
      }

      cols.push_back(cell);
      ++i;
    }
    if (i<3) {
      cerr << "Config line " << linecount << ": insufficient parameters" << endl;
      continue;
    }

    conn_configs.emplace_back(cols[0], cols[1], cols[2], cols[3]);
  }

  cout << conn_configs.size() << " connection request(s) loaded" << endl;
}


void dump() {
  auto seq = sequencer_open();
  alsa_build_graph(seq);
  sequencer_close(seq);

  cerr << sub_map.size() << " connections:" << endl;

  for (auto const &c : sub_map) {
    auto src = c.first;
    auto dst = c.second;

    printf("%s:%s",    id_map.at(src).first.c_str(), id_map.at(src).second.c_str());
    printf(":%s:%s\n", id_map.at(dst).first.c_str(), id_map.at(dst).second.c_str());
  }

  exit(0);
}


void usage(char *program_name)
{
  fprintf(stderr, "Usage: %s [-f <filename>]\t (default: './amidipersist.connections')\n\n", program_name);
  fprintf(stderr, "\tFile format:\n");
  fprintf(stderr, "\t\tFile should contain colon-delimited midi names of: src_client:src_port:dst_client:dst_port\n");
  fprintf(stderr, "\t\tColon can be escaped with '\\'\n");
  fprintf(stderr, "\t\tEmpty lines and lines beginning with # are ignored\n");
  fprintf(stderr, "\nOptions:\n");
  fprintf(stderr, "\t--once\tDon't persist: run connections once and exit; do not listen for events.\n");
  fprintf(stderr, "\t--dump\tDump names of current alsa subscriptions and exit.\n");
  exit(1);
}

int main(int argc, char **argv)
{
  const char *filename = "amidipersist.connections";
  bool runonce = false;

  for (int i=1; i < argc; ++i)
	{
		if (strcmp(argv[i], "-f") == 0)
		{
      if (++i == argc) usage(argv[0]);
      filename = argv[i];
		}
    else if (strcmp(argv[i], "--once") == 0)
      runonce = true;
    else if (strcmp(argv[i], "--dump") == 0)
      dump();
    else if (strcmp(argv[i], "--lax") == 0)
      //TODO: lax mode - don't reconnect manually disconnected items, only if port re-created
      ;
    else if (strcmp(argv[i], "--help") == 0)
      usage(argv[0]);
		else
			usage(argv[0]);
	}

  ifstream infile(filename);
  parse_config(infile);

  if (conn_configs.size() == 0) {
    cerr << "No configs found.  Exiting." << endl;
    exit(1);
  }



  auto seq = sequencer_open();

  alsa_build_graph(seq);
  run_connections(seq);

  if (runonce) {
    sequencer_close(seq);
    exit(0);
  }


  //Create monitor port
  int monitor_port = snd_seq_create_simple_port(seq, "amidipersist Client Monitor",
    SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
    SND_SEQ_PORT_TYPE_APPLICATION);
  if (monitor_port < 0) {
    cerr << "Error creating monitor port." << endl;
    exit(1);
  }

  //Subscribe to System:Announce (0:1)
  snd_seq_connect_from(seq, monitor_port, 0, 1);




  //Watch Events
  snd_seq_event_t *ev;
  while (1) {
    snd_seq_event_input(seq, &ev);
    switch(ev->type)
    {
      //Too lazy to implement individual state changes (for now).  Lets just wait a second and re-build.
      case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
        //TODO: Lax mode
        //find our port and update manually disconnected

      case SND_SEQ_EVENT_CLIENT_CHANGE:
      case SND_SEQ_EVENT_PORT_START:
      case SND_SEQ_EVENT_PORT_CHANGE:
        //TODO: Lax mode
        //loop find manually disconnected ports, if match, disabled manually disconnected mode

        cout << "Alsa midi layout change detected.  Refreshing connections." << endl;
        sleep(1);
        snd_seq_drop_input(seq);
        alsa_build_graph(seq);
        run_connections(seq);
        break;

      case SND_SEQ_EVENT_CLIENT_START:
      case SND_SEQ_EVENT_CLIENT_EXIT:
      case SND_SEQ_EVENT_PORT_EXIT:
        //TODO: Lax mode
        //find manually disconnected ports, if match, disabled manually disconnected mode
      case SND_SEQ_EVENT_PORT_SUBSCRIBED:
        //TODO: Lax mode
        //find manually disconnected ports, if match, disabled manually disconnected mode
      default:
        break;
    }
    snd_seq_free_event(ev);
  }



  sequencer_close(seq);
  return 0;
}





