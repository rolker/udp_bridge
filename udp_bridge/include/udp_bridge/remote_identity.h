#ifndef UDP_BRIDGE_REMOTE_IDENTITY_H
#define UDP_BRIDGE_REMOTE_IDENTITY_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "udp_bridge/packet.h"

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

/// Resolve every configured remote's identity and reject collisions,
/// over-long names, and any identity equal to this bridge's own name.
///
/// Two labels that resolve to the same identity are a configuration error,
/// not a merge: they would collide in `remote_nodes_` and in every topic's
/// `remote_details`, so the second silently overwrites the first's
/// connections and rate limits. Fail loud rather than run half a routing
/// table.
///
/// An identity longer than `maximum_node_name_length` is a configuration
/// error for the same reason (issue #51). It cannot be carried on the wire,
/// so the remote's packets arrive stamped with a shortened `source_node`
/// that never equals the identity this table is keyed by: the relay loop
/// rule matches nothing and the hub echoes the sender's own traffic back to
/// it. Truncating here instead would ALSO reopen the collision this
/// function exists to close, because two identities differing only after
/// the limit would become one wire name. Reject it and say so.
///
/// A remote resolving to THIS bridge's own name is rejected for a third
/// reason (issue #51). Such an entry installs a `RemoteNode` for ourselves
/// in `remote_nodes_`, and once it is there `unwrap()` can no longer refuse
/// our own packets: its self-packet check sits in the not-found branch, so
/// a successful lookup walks straight past it and the bridge starts
/// treating its own traffic as a peer's. `RemoteNode`'s constructor does
/// assert on it, but an assert is compiled out of a release build, which is
/// where this would be met. Catch it here, before the RemoteNode exists.
///
/// An identity that is the EMPTY string is rejected for a fourth (issue
/// #51). It can only arise from an empty `remotes_list` entry with no
/// `name` set, and `""` is a reserved sentinel on the send path: an empty
/// remote name there means "this is a connection request, not a message to
/// a named remote". A remote filed under it is therefore silently
/// unroutable — `allRemotes()` hands it straight to that path — and no
/// packet can ever match it either, since the wire always carries the
/// sender's real name. Same class as the two above: an identity that
/// cannot mean what the config says it means.
///
/// @param labels_and_names (label, `remotes.<label>.name`) pairs, in
///        `remotes_list` order. An empty name means "unset".
/// @param local_name this bridge's own wire name, which no remote may
///        resolve to. Empty disables the check (for callers that have no
///        local name to compare against).
/// @param error set to a human-readable description when an identity is
///        rejected; untouched otherwise. May be null.
/// @return label -> identity for every entry, or an empty map on rejection.
inline std::map<std::string, std::string> resolveRemoteIdentities(
  const std::vector<std::pair<std::string, std::string> >& labels_and_names,
  const std::string& local_name = std::string(),
  std::string* error = nullptr)
{
  std::map<std::string, std::string> identity_by_label;
  std::map<std::string, std::string> label_by_identity;
  for(const auto& entry: labels_and_names)
  {
    // A label repeated in `remotes_list` is not a collision: it names the
    // same parameter block, so it resolves to the same identity and
    // configuring it twice was always idempotent. Skip it rather than
    // reporting that a remote collides with itself.
    if(identity_by_label.count(entry.first))
      continue;
    auto identity = resolveRemoteIdentity(entry.first, entry.second);
    if(identity.empty())
    {
      if(error)
        *error = "a remotes_list entry is empty and sets no name, so it"
                 " resolves to the empty remote name. The empty name is"
                 " reserved on the send path (it marks a connection"
                 " request), so such a remote can never be routed to and"
                 " no arriving packet can ever match it. Give the entry a"
                 " label, or set remotes.<label>.name.";
      return std::map<std::string, std::string>();
    }
    if(!node_name_fits(identity))
    {
      if(error)
        *error = node_name_too_long_error(
          entry.second.empty()
            ? "remote label (remotes_list entry; set remotes." + entry.first
              + ".name to a shorter wire name if the label must stay)"
            : "remote name (remotes." + entry.first + ".name)",
          identity);
      return std::map<std::string, std::string>();
    }
    if(!local_name.empty() && identity == local_name)
    {
      if(error)
        *error = "remote '" + entry.first + "' resolves to remote name '"
               + identity + "', which is this bridge's own name. A bridge"
                 " cannot be its own remote: it would be installed in"
                 " remote_nodes_ under our name and the receive path would"
                 " then accept our own packets as a peer's. Change either"
                 " the `name` parameter or remotes." + entry.first + ".name.";
      return std::map<std::string, std::string>();
    }
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
