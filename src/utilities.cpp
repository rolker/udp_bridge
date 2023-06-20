#include "udp_bridge/utilities.h"

namespace udp_bridge
{

std::string topicString(const std::string& topic, bool keep_slashes, char legal_first, char legal_char)
{
  std::string ret = topic;
  for(int i = 0; i < ret.size(); i++)
    if(!(std::isalnum(ret[i]) || ret[i] == '_' || (ret[i] == '/' && keep_slashes)))
      ret[i] = legal_char;
  // \todo, check for more than just the first char, also check followong /'s if they are kept 
  if(!(std::isalpha(ret[0]) || ret[0] == '/'))
    ret = legal_first+ret;
  return ret;
}

}
