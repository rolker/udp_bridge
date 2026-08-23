#ifndef UDP_BRIDGE_REMOTE_IDENTITY_H
#define UDP_BRIDGE_REMOTE_IDENTITY_H

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace udp_bridge
{

/// Identity of a statically-configured remote (issue #51).
///
/// There is exactly ONE namespace for remote identity in this bridge: the
/// name a bridge calls itself on the wire. It is what `WrappedPacket`
/// carries as `source_node`, what `remote_nodes_` is keyed by when a packet
/// arrives, and what the relay loop rule compares against.
///
/// `remotes_list` entries are *labels* — they exist to spell parameter
/// paths (`remotes.<label>.connections_list`). Before this, the label was
/// also used as the identity key for everything a static config created
/// (`remote_nodes_`, and `subscribers_[topic].remote_details`), while
/// `remotes.<label>.name` — documented in `config/example_params.yaml` as
/// "name of the remote node as known by the remote itself" — was declared
/// and never read. A remote whose own `name` differed from the hub's label
/// for it therefore occupied two keys: the label (from `on_configure`) and
/// its wire name (created on first packet). The relay loop rule, which
/// compares the wire `source_node` against `remote_details` keys, then
/// matched nothing and the hub relayed the sender's own traffic straight
/// back to it — including in a single-remote configuration, where relay is
/// supposed to be inert.
///
/// The fix is to resolve the identity once, here, at configure time:
/// `remotes.<label>.name` when set, else the label. Downstream nothing
/// changes — every static-config consumer already flows from the resolved
/// name — so the label survives only as a parameter-path spelling and the
/// wire name is the single identity namespace it was documented to be.
inline std::string resolveRemoteIdentity(const std::string& label,
                                         const std::string& configured_name)
{
  return configured_name.empty() ? label : configured_name;
}

/// Resolve every configured remote's identity and reject collisions.
///
/// Two labels that resolve to the same identity are a configuration error,
/// not a merge: they would collide in `remote_nodes_` and in every topic's
/// `remote_details`, so the second silently overwrites the first's
/// connections and rate limits. Fail loud rather than run half a routing
/// table.
///
/// @param labels_and_names (label, `remotes.<label>.name`) pairs, in
///        `remotes_list` order. An empty name means "unset".
/// @param error set to a human-readable description when a collision is
///        found; untouched otherwise. May be null.
/// @return label -> identity for every entry, or an empty map on collision.
inline std::map<std::string, std::string> resolveRemoteIdentities(
  const std::vector<std::pair<std::string, std::string> >& labels_and_names,
  std::string* error = nullptr)
{
  std::map<std::string, std::string> identity_by_label;
  std::map<std::string, std::string> label_by_identity;
  for(const auto& entry: labels_and_names)
  {
    auto identity = resolveRemoteIdentity(entry.first, entry.second);
    auto existing = label_by_identity.find(identity);
    if(existing != label_by_identity.end())
    {
      if(error)
        *error = "remotes '" + existing->second + "' and '" + entry.first
               + "' both resolve to remote name '" + identity
               + "'; remote names must be unique (see remotes.<label>.name)";
      return std::map<std::string, std::string>();
    }
    label_by_identity[identity] = entry.first;
    identity_by_label[entry.first] = identity;
  }
  return identity_by_label;
}

} // namespace udp_bridge

#endif
