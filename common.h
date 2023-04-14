#ifndef COMMON_H
#define COMMON_H

#include <utility>
#include <list>
#include <set>
#include <map>
#include <string>


using namespace std;

//Alsa midi client id and port id are unsigned char
typedef unsigned char seq_id;
typedef pair<seq_id,seq_id> midi_id;

enum ID_Status { ID_UNSET=0, ID_DISCOVERED, ID_USER_SET };
struct midi_addr {
  string name;
  seq_id id;
  ID_Status status;
};

struct alsa_state {
    //Map names to id's
    multimap<pair<string,string>, midi_id> names_map;

    //Map id's to names
    map<midi_id, pair<string,string>> id_map;

    //Map for alsa subscriptions
    multimap<midi_id, midi_id> sub_map;   //Multimap (easy iteration, .size() at a glance)
    map<midi_id, set<midi_id>> sub_map2;  //Set for quick lookup of connected ports...I'll keep both maps for now.
};

#endif
