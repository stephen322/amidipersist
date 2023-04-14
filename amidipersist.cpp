using namespace std;

#include <string>
#include <list>
#include <set>
#include <vector>
#include <map>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>

#include <alsa/asoundlib.h>

#include "common.h"
#include "UserConnection.h"


// Globals
list<UserConnection> conn_configs;
struct alsa_state alsa_state;


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
void alsa_build_graph(snd_seq_t *sequencer, struct alsa_state &alsa_state)
{
  alsa_state.sub_map.clear();
  alsa_state.sub_map2.clear();
  alsa_state.names_map.clear();

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

    //Iterate over ports for this client
    while (snd_seq_query_next_port(sequencer, port_info) >= 0)
    {
      const string port_name = string(snd_seq_port_info_get_name(port_info));
      const seq_id port_id = snd_seq_port_info_get_port(port_info);


      //Map id's to names
      alsa_state.id_map[make_pair(client_id,port_id)] = make_pair(client_name,port_name);

      //Map names
      alsa_state.names_map.emplace(make_pair(client_name, port_name), make_pair(client_id,port_id));


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
          alsa_state.sub_map.emplace(make_pair(
            make_pair(client_id, port_id),
            make_pair(snd_seq_client_info_get_client(connected_client_info), snd_seq_port_info_get_port(connected_port_info))
          ));
          alsa_state.sub_map2[make_pair(client_id, port_id)].insert(make_pair(snd_seq_client_info_get_client(connected_client_info), snd_seq_port_info_get_port(connected_port_info)));

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
bool alsa_connect(snd_seq_t* sequencer, const midi_id &sender, const midi_id &dest)
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








bool is_connected(const midi_id &src, const midi_id &dst)
{
  auto find = alsa_state.sub_map2.find(src);
  if (find != alsa_state.sub_map2.end()) {
    if (find->second.count(dst) >= 1)
      return true;
  }
  return false;
}

void run_connections(snd_seq_t* sequencer)
{
  for (auto &config : conn_configs) {
    config.update_state(alsa_state);

    for (auto &c : config.id_connections) {
      if (is_connected(c.first,c.second)) continue;

      if (alsa_connect(sequencer, c.first, c.second)) {
        printf("Connected %s:%s (%d:%d) to %s:%s (%d:%d)\n", 
          alsa_state.id_map[c.first].first.c_str(), alsa_state.id_map[c.first].second.c_str(), c.first.first, c.first.second,
          alsa_state.id_map[c.second].first.c_str(), alsa_state.id_map[c.second].second.c_str(), c.second.first, c.second.second);
      }
      else //Connection failed
        fprintf(stderr, "Unabled to connect %s:%s (%d:%d) to %s:%s (%d:%d)\n",
          alsa_state.id_map[c.first].first.c_str(), alsa_state.id_map[c.first].second.c_str(), c.first.first, c.first.second,
          alsa_state.id_map[c.second].first.c_str(), alsa_state.id_map[c.second].second.c_str(), c.second.first, c.second.second);
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
  alsa_build_graph(seq, alsa_state);
  sequencer_close(seq);

  cerr << alsa_state.sub_map.size() << " connections:" << endl;

  for (auto const &c : alsa_state.sub_map) {
    auto src = c.first;
    auto dst = c.second;

    printf("%s:%s",    alsa_state.id_map.at(src).first.c_str(), alsa_state.id_map.at(src).second.c_str());
    printf(":%s:%s\n", alsa_state.id_map.at(dst).first.c_str(), alsa_state.id_map.at(dst).second.c_str());
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

  alsa_build_graph(seq, alsa_state);
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
        alsa_build_graph(seq, alsa_state);
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





