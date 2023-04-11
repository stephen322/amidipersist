using namespace std;

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>

#include <alsa/asoundlib.h>

//Alsa midi client id and port id are unsigned char...
typedef uint8_t seq_id;


struct user_connection
{
  string source_client_name;
  string source_port_name;
  string dest_client_name;
  string dest_port_name;

  //TODO: Support specifying id number in addition to text name
};

vector<user_connection> conn_configs;

//Map for client name to id
map<string, seq_id> client_map;

//Map for port name (plus client id) to port id
map<pair<seq_id,string>, seq_id> port_map;

//Map for alsa subscriptions
map<pair<seq_id,seq_id>, pair<seq_id,seq_id>> sub_map;



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
    snd_seq_port_info_set_client(port_info, client_id);
    snd_seq_port_info_set_port(port_info, -1);

    //Map client name to id
    client_map[string(snd_seq_client_info_get_name(client_info))] = client_id;

    //Iterate over ports for this client
    while (snd_seq_query_next_port(sequencer, port_info) >= 0)
    {
      const char *port_name = snd_seq_port_info_get_name(port_info);
      const seq_id port_id = snd_seq_port_info_get_port(port_info);

      //Map port name to id
      port_map[make_pair(client_id,string(port_name))] = port_id;

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
          sub_map[make_pair(client_id, port_id)] = make_pair(snd_seq_client_info_get_client(connected_client_info), snd_seq_port_info_get_port(connected_port_info));

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

// Lookup a client:port name, and if found place its id's in target
bool lookup_name(pair<seq_id,seq_id> &target, const string client, const string port)
{
  auto client_it = client_map.find(client);
  if (client_it == client_map.end())
    return false;

  seq_id client_id = client_it->second;

  auto port_it = port_map.find(make_pair(client_id, port));
  if (port_it == port_map.end())
    return false;

  target.first = client_id;
  target.second = port_it->second;

  return true;
}

bool is_connected(const pair<seq_id,seq_id> &source, const pair<seq_id,seq_id> &dest)
{
  auto it = sub_map.find(source);
  if (it == sub_map.end())
    return false;

  auto connected_dst = it->second;
  return (connected_dst.first == dest.first && connected_dst.second == dest.second);
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
  // Loop through connection config, make the connection if the ports are online and not already connected
  pair<seq_id,seq_id> confsrc, confdst;
  for (auto const& c : conn_configs) {
    bool ids_found = lookup_name(confsrc, c.source_client_name, c.source_port_name) && lookup_name(confdst, c.dest_client_name, c.dest_port_name);

    if (!ids_found) continue;
    if (is_connected(confsrc, confdst)) continue;

    if (alsa_connect(sequencer, confsrc, confdst))
      //std::format is c++20?  A bit too new...
      printf("Connected %s:%s (%d:%d) to %s:%s (%d:%d)\n", 
        c.source_client_name.c_str(), c.source_port_name.c_str(), confsrc.first, confsrc.second,
        c.dest_client_name.c_str(), c.dest_port_name.c_str(), confdst.first, confdst.second);
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

    user_connection c = { cols[0], cols[1], cols[2], cols[3] };
    conn_configs.push_back(c);
  }

  cout << conn_configs.size() << " connection request(s) loaded" << endl;
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
      case SND_SEQ_EVENT_CLIENT_CHANGE:
      case SND_SEQ_EVENT_PORT_START:
      case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
      case SND_SEQ_EVENT_PORT_CHANGE:
        cout << "Alsa midi layout change detected.  Refreshing connections." << endl;
        sleep(1);
        snd_seq_drop_input(seq);
        alsa_build_graph(seq);
        run_connections(seq);
        break;

      case SND_SEQ_EVENT_CLIENT_START:
      case SND_SEQ_EVENT_CLIENT_EXIT:
      case SND_SEQ_EVENT_PORT_EXIT:
      case SND_SEQ_EVENT_PORT_SUBSCRIBED:
      default:
        break;
    }
    snd_seq_free_event(ev);
  }



  sequencer_close(seq);
  return 0;
}
