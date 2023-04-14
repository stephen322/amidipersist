#include "UserConnection.h"
#include "common.h"

using namespace std;

void UserConnection::update_state(struct alsa_state alsa_state)
{
  //TODO: for each ID_USER_SET, set names


  //Iterate through all src id -> dst id combos and place in id_connections.  (Names can match more than 1 id.)
  id_connections.clear();
  auto src_names = alsa_state.names_map.equal_range(make_pair(this->config[SRC_CLIENT].name, this->config[SRC_PORT].name));
  auto dst_names = alsa_state.names_map.equal_range(make_pair(this->config[DST_CLIENT].name, this->config[DST_PORT].name));
  for (auto sit = src_names.first; sit != src_names.second; ++sit) {
    for (auto dit = dst_names.first; dit != dst_names.second; ++dit) {
    // if ID_USER_SET id does not match, continue

    auto &srcidpair = sit->second;
    auto &dstidpair = dit->second;

    id_connections.emplace(make_pair(srcidpair.first, srcidpair.second), make_pair(dstidpair.first, dstidpair.second));
    }
  }
}